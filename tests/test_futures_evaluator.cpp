/**
 * Test suite for FuturesEvaluator
 *
 * Tests FuturesEvaluator class with real MetricsManager + EventBus:
 * - Hedge mode publishes FuturesSellEvent when basis > threshold
 * - Farming mode publishes FuturesBuyEvent/FuturesSellEvent based on funding
 * - Exit logic publishes FuturesCloseLongEvent/FuturesCloseShortEvent
 * - Cooldown prevents rapid evaluations
 */

#include "../include/core/event_bus.hpp"
#include "../include/core/events.hpp"
#include "../include/core/futures_evaluator.hpp"
#include "../include/core/metrics_manager.hpp"
#include "../include/execution/funding_scheduler.hpp"
#include "../include/execution/futures_position.hpp"

#include <cassert>
#include <iostream>
#include <memory>

using namespace hft;
using namespace hft::core;
using namespace hft::execution;

// Event capture for testing
struct CapturedEvent {
    enum Type { None, FuturesBuy, FuturesSell, CloseLong, CloseShort };
    Type type = None;
    Symbol symbol = 0;
    double qty = 0.0;
    std::string reason;
};

void test_hedge_mode_publishes_sell_event() {
    std::cout << "Test: Hedge mode publishes FuturesSellEvent...";

    // Setup (heap-allocate to avoid stack overflow)
    EventBus bus;
    auto metrics = std::make_unique<MetricsManager>();
    FuturesPosition positions;
    FuturesEvaluator evaluator(&bus, metrics.get(), &positions);

    // Subscribe to FuturesSellEvent
    CapturedEvent captured;
    bus.subscribe<FuturesSellEvent>([&](const FuturesSellEvent& e) {
        captured.type = CapturedEvent::FuturesSell;
        captured.symbol = e.symbol;
        captured.qty = e.qty;
        captured.reason = e.reason;
    });

    // Setup futures metrics: basis = 25 bps (> 20 threshold)
    // mark=50010, index=50000 → basis = 10 → basis_bps = 10 / 50000 * 10000 = 2 bps (too low)
    // Need: 20 bps → basis = 50000 * 0.002 = 100 → mark = 50100
    double index_price = 50000.0;
    double mark_price = 50100.0; // 100 basis = 20 bps
    double funding_rate = 0.0001;
    uint64_t next_funding_ms = 10000000;
    metrics->on_mark_price(0, mark_price, index_price, funding_rate, next_funding_ms, 1000000);

    // Setup spot position (has position to hedge)
    StrategyPosition spot_position;
    spot_position.quantity = 1.5;
    spot_position.avg_entry_price = 50000 * 10000;

    // Setup market snapshot
    MarketSnapshot market;
    market.bid = 50000 * 10000;
    market.ask = 50010 * 10000;

    // Execute
    evaluator.evaluate(0, market, spot_position);

    // Verify
    assert(captured.type == CapturedEvent::FuturesSell);
    assert(captured.symbol == 0);
    assert(captured.qty == 1.5); // Matches spot position size
    assert(captured.reason == std::string("hedge_cash_carry"));

    std::cout << " ✓" << std::endl;
}

void test_farming_mode_publishes_buy_event_on_negative_funding() {
    std::cout << "Test: Farming mode publishes FuturesBuyEvent on negative funding...";

    // Setup
    EventBus bus;
    auto metrics = std::make_unique<MetricsManager>();
    FuturesPosition positions;
    FuturesEvaluator evaluator(&bus, metrics.get(), &positions);

    // Subscribe to FuturesBuyEvent
    CapturedEvent captured;
    bus.subscribe<FuturesBuyEvent>([&](const FuturesBuyEvent& e) {
        captured.type = CapturedEvent::FuturesBuy;
        captured.symbol = e.symbol;
        captured.qty = e.qty;
        captured.reason = e.reason;
    });

    // Setup futures metrics: funding = -0.06% (negative, extreme)
    double index_price = 50000.0;
    double mark_price = 50005.0;                            // Small basis (5 bps, below hedge threshold)
    double funding_rate = -0.0006;                          // -0.06%
    uint64_t next_funding_ms = 10000000 + (60 * 60 * 1000); // 1 hour away (Normal phase)
    metrics->on_mark_price(1, mark_price, index_price, funding_rate, next_funding_ms, 1000000);

    // No spot position
    StrategyPosition spot_position;

    // Setup market snapshot
    MarketSnapshot market;
    market.bid = 50000 * 10000;
    market.ask = 50010 * 10000;

    // Execute
    evaluator.evaluate(1, market, spot_position);

    // Verify
    assert(captured.type == CapturedEvent::FuturesBuy);
    assert(captured.symbol == 1);
    assert(captured.qty == 1.0); // Fixed size
    assert(captured.reason == std::string("funding_farming_negative"));

    std::cout << " ✓" << std::endl;
}

void test_farming_mode_publishes_sell_event_on_positive_funding() {
    std::cout << "Test: Farming mode publishes FuturesSellEvent on positive funding...";

    // Setup
    EventBus bus;
    auto metrics = std::make_unique<MetricsManager>();
    FuturesPosition positions;
    FuturesEvaluator evaluator(&bus, metrics.get(), &positions);

    // Subscribe to FuturesSellEvent
    CapturedEvent captured;
    bus.subscribe<FuturesSellEvent>([&](const FuturesSellEvent& e) {
        captured.type = CapturedEvent::FuturesSell;
        captured.symbol = e.symbol;
        captured.qty = e.qty;
        captured.reason = e.reason;
    });

    // Setup futures metrics: funding = +0.06% (positive, extreme)
    double index_price = 50000.0;
    double mark_price = 50005.0;
    double funding_rate = 0.0006; // +0.06%
    uint64_t next_funding_ms = 10000000 + (60 * 60 * 1000);
    metrics->on_mark_price(2, mark_price, index_price, funding_rate, next_funding_ms, 1000000);

    // No spot position
    StrategyPosition spot_position;

    // Setup market snapshot
    MarketSnapshot market;
    market.bid = 50000 * 10000;
    market.ask = 50010 * 10000;

    // Execute
    evaluator.evaluate(2, market, spot_position);

    // Verify
    assert(captured.type == CapturedEvent::FuturesSell);
    assert(captured.symbol == 2);
    assert(captured.qty == 1.0);
    assert(captured.reason == std::string("funding_farming_positive"));

    std::cout << " ✓" << std::endl;
}

void test_exit_logic_publishes_close_short_on_backwardation() {
    std::cout << "Test: Exit logic publishes FuturesCloseShortEvent on backwardation...";

    // Setup
    EventBus bus;
    auto metrics = std::make_unique<MetricsManager>();
    FuturesPosition positions;
    FuturesEvaluator evaluator(&bus, metrics.get(), &positions);

    // Subscribe to FuturesCloseShortEvent
    CapturedEvent captured;
    bus.subscribe<FuturesCloseShortEvent>([&](const FuturesCloseShortEvent& e) {
        captured.type = CapturedEvent::CloseShort;
        captured.symbol = e.symbol;
        captured.qty = e.qty;
        captured.reason = e.reason;
    });

    // Open hedge short position manually
    positions.open_position(3, PositionSource::Hedge, Side::Sell, 1.5, 50000 * 10000, 1000000000);

    // Setup futures metrics: basis = -25 bps (backwardation)
    // mark < index → negative basis → backwardation
    double index_price = 50000.0;
    double mark_price = 49900.0; // -100 basis = -20 bps
    double funding_rate = 0.0001;
    uint64_t next_funding_ms = 10000000;
    metrics->on_mark_price(3, mark_price, index_price, funding_rate, next_funding_ms, 1000000);

    // Spot position still exists
    StrategyPosition spot_position;
    spot_position.quantity = 1.5;
    spot_position.avg_entry_price = 50000 * 10000;

    // Setup market snapshot
    MarketSnapshot market;
    market.bid = 50000 * 10000;
    market.ask = 50010 * 10000;

    // Execute
    evaluator.evaluate(3, market, spot_position);

    // Verify
    assert(captured.type == CapturedEvent::CloseShort);
    assert(captured.symbol == 3);
    assert(captured.qty == 1.5);
    assert(captured.reason == std::string("hedge_backwardation"));

    // Verify position removed
    auto* pos = positions.get_position(3, PositionSource::Hedge, Side::Sell);
    assert(pos == nullptr);

    std::cout << " ✓" << std::endl;
}

void test_exit_logic_publishes_close_long_on_postfunding() {
    std::cout << "Test: Exit logic publishes FuturesCloseLongEvent on PostFunding...";

    // Setup
    EventBus bus;
    auto metrics = std::make_unique<MetricsManager>();
    FuturesPosition positions;
    FuturesEvaluator evaluator(&bus, metrics.get(), &positions);

    // Subscribe to FuturesCloseLongEvent
    CapturedEvent captured;
    bus.subscribe<FuturesCloseLongEvent>([&](const FuturesCloseLongEvent& e) {
        captured.type = CapturedEvent::CloseLong;
        captured.symbol = e.symbol;
        captured.qty = e.qty;
        captured.reason = e.reason;
    });

    // Open farming long position manually
    positions.open_position(4, PositionSource::Farming, Side::Buy, 1.0, 50000 * 10000, 1000000000);

    // Setup futures metrics: PostFunding phase (5 min after funding)
    double index_price = 50000.0;
    double mark_price = 50005.0;
    double funding_rate = -0.0006;
    uint64_t next_funding_ms = 1000000 - (5 * 60 * 1000); // 5 min ago (PostFunding)
    metrics->on_mark_price(4, mark_price, index_price, funding_rate, next_funding_ms, 1000000);

    // No spot position
    StrategyPosition spot_position;

    // Setup market snapshot
    MarketSnapshot market;
    market.bid = 50000 * 10000;
    market.ask = 50010 * 10000;

    // Execute
    evaluator.evaluate(4, market, spot_position);

    // Verify
    assert(captured.type == CapturedEvent::CloseLong);
    assert(captured.symbol == 4);
    assert(captured.qty == 1.0);
    assert(captured.reason == std::string("farming_postfunding"));

    std::cout << " ✓" << std::endl;
}

void test_cooldown_prevents_rapid_evaluation() {
    std::cout << "Test: Cooldown prevents rapid evaluation...";

    // Setup
    EventBus bus;
    auto metrics = std::make_unique<MetricsManager>();
    FuturesPosition positions;
    FuturesEvaluator evaluator(&bus, metrics.get(), &positions);

    // Subscribe to FuturesSellEvent (count events)
    int event_count = 0;
    bus.subscribe<FuturesSellEvent>([&](const FuturesSellEvent& e) {
        (void)e;
        event_count++;
    });

    // Setup futures metrics: high funding (should trigger farming)
    double index_price = 50000.0;
    double mark_price = 50005.0;
    double funding_rate = 0.0006;
    uint64_t next_funding_ms = 10000000 + (60 * 60 * 1000);
    metrics->on_mark_price(5, mark_price, index_price, funding_rate, next_funding_ms, 1000000);

    // No spot position
    StrategyPosition spot_position;

    // Setup market snapshot
    MarketSnapshot market;
    market.bid = 50000 * 10000;
    market.ask = 50010 * 10000;

    // First evaluate - should publish event
    evaluator.evaluate(5, market, spot_position);
    assert(event_count == 1);

    // Second evaluate immediately - should be ignored (cooldown = 5s)
    evaluator.evaluate(5, market, spot_position);
    assert(event_count == 1); // Still 1, second ignored

    std::cout << " ✓" << std::endl;
}

void test_no_action_when_thresholds_not_met() {
    std::cout << "Test: No action when thresholds not met...";

    // Setup
    EventBus bus;
    auto metrics = std::make_unique<MetricsManager>();
    FuturesPosition positions;
    FuturesEvaluator evaluator(&bus, metrics.get(), &positions);

    // Subscribe to all events
    int event_count = 0;
    bus.subscribe<FuturesBuyEvent>([&](const FuturesBuyEvent& e) {
        (void)e;
        event_count++;
    });
    bus.subscribe<FuturesSellEvent>([&](const FuturesSellEvent& e) {
        (void)e;
        event_count++;
    });

    // Setup futures metrics: below all thresholds
    double index_price = 50000.0;
    double mark_price = 50010.0;  // 10 basis = 2 bps (below 20 bps threshold)
    double funding_rate = 0.0003; // 0.03% (below 0.05% threshold)
    uint64_t next_funding_ms = 10000000 + (60 * 60 * 1000);
    metrics->on_mark_price(6, mark_price, index_price, funding_rate, next_funding_ms, 1000000);

    // Has spot position but basis too low for hedge
    StrategyPosition spot_position;
    spot_position.quantity = 1.5;
    spot_position.avg_entry_price = 50000 * 10000;

    // Setup market snapshot
    MarketSnapshot market;
    market.bid = 50000 * 10000;
    market.ask = 50010 * 10000;

    // Execute
    evaluator.evaluate(6, market, spot_position);

    // Verify no events published
    assert(event_count == 0);

    std::cout << " ✓" << std::endl;
}

void test_no_duplicate_hedge_position() {
    std::cout << "Test: No duplicate hedge position...";

    // Setup
    EventBus bus;
    auto metrics = std::make_unique<MetricsManager>();
    FuturesPosition positions;
    FuturesEvaluator evaluator(&bus, metrics.get(), &positions);

    // Subscribe to FuturesSellEvent
    int event_count = 0;
    bus.subscribe<FuturesSellEvent>([&](const FuturesSellEvent& e) {
        (void)e;
        event_count++;
    });

    // Open hedge position manually (already hedged)
    positions.open_position(7, PositionSource::Hedge, Side::Sell, 1.5, 50000 * 10000, 1000000000);

    // Setup futures metrics: high basis (would normally trigger hedge)
    double index_price = 50000.0;
    double mark_price = 50100.0; // 100 basis = 20 bps
    double funding_rate = 0.0001;
    uint64_t next_funding_ms = 10000000;
    metrics->on_mark_price(7, mark_price, index_price, funding_rate, next_funding_ms, 1000000);

    // Has spot position
    StrategyPosition spot_position;
    spot_position.quantity = 1.5;
    spot_position.avg_entry_price = 50000 * 10000;

    // Setup market snapshot
    MarketSnapshot market;
    market.bid = 50000 * 10000;
    market.ask = 50010 * 10000;

    // Execute - should NOT open duplicate hedge
    evaluator.evaluate(7, market, spot_position);

    // Verify no new event (already hedged)
    assert(event_count == 0);

    std::cout << " ✓" << std::endl;
}

int main() {
    std::cout << "\n=== FuturesEvaluator Integration Tests ===" << std::endl;

    // Entry logic tests
    test_hedge_mode_publishes_sell_event();
    test_farming_mode_publishes_buy_event_on_negative_funding();
    test_farming_mode_publishes_sell_event_on_positive_funding();

    // Exit logic tests
    test_exit_logic_publishes_close_short_on_backwardation();
    test_exit_logic_publishes_close_long_on_postfunding();

    // Infrastructure tests
    test_cooldown_prevents_rapid_evaluation();
    test_no_action_when_thresholds_not_met();
    test_no_duplicate_hedge_position();

    std::cout << "\n✓ All 8 FuturesEvaluator integration tests passed!" << std::endl;
    return 0;
}
