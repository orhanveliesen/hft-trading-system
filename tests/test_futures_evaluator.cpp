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
#include "../include/util/time_utils.hpp"

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
    // basis = futures_mid - spot_mid = 50125 - 50000 = 125
    // basis_bps = 125 / 50000 * 10000 = 25 bps
    metrics->on_spot_bbo(0, 49995.0, 50005.0, 1000000);    // spot_mid = 50000
    metrics->on_futures_bbo(0, 50120.0, 50130.0, 1000000); // futures_mid = 50125
    metrics->on_mark_price(0, 50125.0, 50000.0, 0.0001, 10000000, 1000000);

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
    // Small basis (5 bps, below 20 bps hedge threshold)
    metrics->on_spot_bbo(1, 49995.0, 50005.0, 1000000);     // spot_mid = 50000
    metrics->on_futures_bbo(1, 50000.0, 50010.0, 1000000);  // futures_mid = 50005 (basis = 5 bps)
    double funding_rate = -0.0006;                          // -0.06%
    uint64_t next_funding_ms = 10000000 + (60 * 60 * 1000); // 1 hour away (Normal phase)
    metrics->on_mark_price(1, 50005.0, 50000.0, funding_rate, next_funding_ms, 1000000);

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
    metrics->on_spot_bbo(2, 49995.0, 50005.0, 1000000);    // spot_mid = 50000
    metrics->on_futures_bbo(2, 50000.0, 50010.0, 1000000); // futures_mid = 50005 (basis = 5 bps)
    double funding_rate = 0.0006;                          // +0.06%
    uint64_t next_funding_ms = 10000000 + (60 * 60 * 1000);
    metrics->on_mark_price(2, 50005.0, 50000.0, funding_rate, next_funding_ms, 1000000);

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
    // Need: -25 bps → basis = -125 → futures_mid = 49875
    metrics->on_spot_bbo(3, 49995.0, 50005.0, 1000000);    // spot_mid = 50000
    metrics->on_futures_bbo(3, 49870.0, 49880.0, 1000000); // futures_mid = 49875 (basis = -25 bps)
    double funding_rate = 0.0001;
    uint64_t next_funding_ms = 10000000;
    metrics->on_mark_price(3, 49875.0, 50000.0, funding_rate, next_funding_ms, 1000000);

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
    // Use realistic timestamps based on wall clock
    uint64_t now_ns = util::wall_clock_ns();
    uint64_t now_ms = now_ns / 1'000'000;
    uint64_t next_funding_ms = now_ms - (5 * 60 * 1000); // 5 min ago (PostFunding)

    metrics->on_spot_bbo(4, 49995.0, 50005.0, now_ns / 1000);    // spot_mid = 50000
    metrics->on_futures_bbo(4, 50000.0, 50010.0, now_ns / 1000); // futures_mid = 50005
    double funding_rate = -0.0006;
    metrics->on_mark_price(4, 50005.0, 50000.0, funding_rate, next_funding_ms, now_ns / 1000);

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
    metrics->on_spot_bbo(5, 49995.0, 50005.0, 1000000);    // spot_mid = 50000
    metrics->on_futures_bbo(5, 50000.0, 50010.0, 1000000); // futures_mid = 50005
    double funding_rate = 0.0006;
    uint64_t next_funding_ms = 10000000 + (60 * 60 * 1000);
    metrics->on_mark_price(5, 50005.0, 50000.0, funding_rate, next_funding_ms, 1000000);

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
    metrics->on_spot_bbo(6, 49995.0, 50005.0, 1000000);    // spot_mid = 50000
    metrics->on_futures_bbo(6, 50005.0, 50015.0, 1000000); // futures_mid = 50010 (basis = 2 bps < 20 threshold)
    double funding_rate = 0.0003;                          // 0.03% (below 0.05% threshold)
    uint64_t next_funding_ms = 10000000 + (60 * 60 * 1000);
    metrics->on_mark_price(6, 50010.0, 50000.0, funding_rate, next_funding_ms, 1000000);

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
    metrics->on_spot_bbo(7, 49995.0, 50005.0, 1000000);    // spot_mid = 50000
    metrics->on_futures_bbo(7, 50120.0, 50130.0, 1000000); // futures_mid = 50125 (basis = 25 bps > 20 threshold)
    double funding_rate = 0.0001;
    uint64_t next_funding_ms = 10000000;
    metrics->on_mark_price(7, 50125.0, 50000.0, funding_rate, next_funding_ms, 1000000);

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

void test_farming_exit_on_funding_reversal_short() {
    std::cout << "Test: Farming short exits when funding reverses to negative...";

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

    // Open farming short position (was farming positive funding)
    positions.open_position(10, PositionSource::Farming, Side::Sell, 1.0, 50000 * 10000, 1000000000);

    // Setup futures metrics: funding reversed to negative (was positive when entered)
    metrics->on_spot_bbo(10, 49995.0, 50005.0, 1000000);    // spot_mid = 50000
    metrics->on_futures_bbo(10, 50000.0, 50010.0, 1000000); // futures_mid = 50005
    double funding_rate = -0.0006;                          // Now negative (reversed)
    uint64_t next_funding_ms = 10000000 + (60 * 60 * 1000); // Normal phase
    metrics->on_mark_price(10, 50005.0, 50000.0, funding_rate, next_funding_ms, 1000000);

    // No spot position
    StrategyPosition spot_position;

    // Setup market snapshot
    MarketSnapshot market;
    market.bid = 50000 * 10000;
    market.ask = 50010 * 10000;

    // Execute
    evaluator.evaluate(10, market, spot_position);

    // Verify
    assert(captured.type == CapturedEvent::CloseShort);
    assert(captured.symbol == 10);
    assert(captured.qty == 1.0);
    assert(captured.reason == std::string("farming_reversed"));

    std::cout << " ✓" << std::endl;
}

void test_hedge_exit_on_spot_position_closed() {
    std::cout << "Test: Hedge exits when spot position is closed...";

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

    // Open hedge short position
    positions.open_position(11, PositionSource::Hedge, Side::Sell, 1.5, 50000 * 10000, 1000000000);

    // Setup futures metrics: positive basis (still profitable)
    metrics->on_spot_bbo(11, 49995.0, 50005.0, 1000000);    // spot_mid = 50000
    metrics->on_futures_bbo(11, 50120.0, 50130.0, 1000000); // futures_mid = 50125 (basis = 25 bps > 20 threshold)
    double funding_rate = 0.0001;
    uint64_t next_funding_ms = 10000000;
    metrics->on_mark_price(11, 50125.0, 50000.0, funding_rate, next_funding_ms, 1000000);

    // NO spot position (closed)
    StrategyPosition spot_position;

    // Setup market snapshot
    MarketSnapshot market;
    market.bid = 50000 * 10000;
    market.ask = 50010 * 10000;

    // Execute
    evaluator.evaluate(11, market, spot_position);

    // Verify
    assert(captured.type == CapturedEvent::CloseShort);
    assert(captured.symbol == 11);
    assert(captured.qty == 1.5);
    assert(captured.reason == std::string("hedge_spot_closed"));

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
    test_farming_exit_on_funding_reversal_short();
    test_hedge_exit_on_spot_position_closed();

    // Infrastructure tests
    test_cooldown_prevents_rapid_evaluation();
    test_no_action_when_thresholds_not_met();
    test_no_duplicate_hedge_position();

    std::cout << "\n✓ All 10 FuturesEvaluator integration tests passed!" << std::endl;
    return 0;
}
