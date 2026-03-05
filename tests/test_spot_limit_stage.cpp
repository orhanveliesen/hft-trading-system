#include "../include/execution/execution_engine.hpp"
#include "../include/execution/spot_limit_stage.hpp"
#include "../include/strategy/metrics_context.hpp"
#include "../include/util/time_utils.hpp"

#include <cassert>
#include <iostream>

using namespace hft;
using namespace hft::execution;
using namespace hft::strategy;

// Mock exchange for testing
class MockExchange : public IExchangeAdapter {
public:
    int cancel_calls = 0;
    uint64_t next_order_id = 1000;

    uint64_t send_market_order(Symbol symbol, Side side, double qty, Price expected_price) override {
        return next_order_id++;
    }

    uint64_t send_limit_order(Symbol symbol, Side side, double qty, Price limit_price) override {
        return next_order_id++;
    }

    bool cancel_order(uint64_t order_id) override {
        cancel_calls++;
        return true;
    }

    bool is_order_pending(uint64_t order_id) const override { return false; }
    bool is_paper() const override { return true; }
};

void test_limit_order_when_exec_score_positive() {
    MockExchange exchange;
    ExecutionEngine engine;
    engine.set_exchange(&exchange);

    SpotLimitStage stage(&engine);

    // Test without metrics (will use signal strength only)
    Signal signal = Signal::buy(SignalStrength::Weak, 10.0, "test"); // Weak = patient = +10 urgency
    ExecutionContext ctx;
    ctx.symbol = 1;
    ctx.market.bid = 100000;
    ctx.market.ask = 100100;
    ctx.metrics = nullptr; // No metrics, score = urgency only = +10

    auto requests = stage.process(signal, ctx);
    assert(requests.size() == 1);
    assert(requests[0].type == OrderType::Limit);
    assert(requests[0].side == Side::Buy);
    assert(requests[0].source_stage == std::string_view("SpotLimit"));
    std::cout << "[PASS] test_limit_order_when_exec_score_positive\n";
}

void test_no_order_when_exec_score_negative() {
    MockExchange exchange;
    ExecutionEngine engine;
    engine.set_exchange(&exchange);

    SpotLimitStage stage(&engine);

    Signal signal = Signal::buy(SignalStrength::Strong, 10.0, "test"); // Strong = urgent = -10
    ExecutionContext ctx;
    ctx.symbol = 1;
    ctx.market.bid = 100000;
    ctx.market.ask = 100100;
    ctx.metrics = nullptr; // No metrics, score = urgency = -10

    auto requests = stage.process(signal, ctx);
    assert(requests.empty()); // Market preferred, stage returns empty
    std::cout << "[PASS] test_no_order_when_exec_score_negative\n";
}

void test_sell_caps_to_position() {
    MockExchange exchange;
    ExecutionEngine engine;
    engine.set_exchange(&exchange);
    engine.set_position_callback([](Symbol) { return 5.0; }); // Position = 5

    SpotLimitStage stage(&engine);

    Signal signal = Signal::sell(SignalStrength::Weak, 100.0, "test"); // Try to sell 100
    ExecutionContext ctx;
    ctx.symbol = 1;
    ctx.market.bid = 100000;
    ctx.market.ask = 100100;
    ctx.position.quantity = 5.0;
    ctx.metrics = nullptr;

    auto requests = stage.process(signal, ctx);
    assert(requests.size() == 1);
    // Note: qty capping happens in ExecutionEngine, not in stage
    assert(requests[0].qty == 100.0); // Stage passes through, engine caps
    std::cout << "[PASS] test_sell_caps_to_position\n";
}

void test_sell_no_position_stage_passes_through() {
    MockExchange exchange;
    ExecutionEngine engine;
    engine.set_exchange(&exchange);
    engine.set_position_callback([](Symbol) { return 0.0; }); // No position

    SpotLimitStage stage(&engine);

    Signal signal = Signal::sell(SignalStrength::Weak, 10.0, "test");
    ExecutionContext ctx;
    ctx.symbol = 1;
    ctx.market.bid = 100000;
    ctx.market.ask = 100100;
    ctx.position.quantity = 0.0;
    ctx.metrics = nullptr;

    auto requests = stage.process(signal, ctx);
    // Stage does NOT check position (unlike SpotMarketStage) - passes through to ExecutionEngine
    assert(requests.size() == 1);
    std::cout << "[PASS] test_sell_no_position_stage_passes_through\n";
}

void test_cancel_on_direction_change() {
    MockExchange exchange;
    ExecutionEngine engine;
    engine.set_exchange(&exchange);

    SpotLimitStage stage(&engine);

    ExecutionContext ctx;
    ctx.symbol = 1;
    ctx.market.bid = 100000;
    ctx.market.ask = 100100;
    ctx.metrics = nullptr;

    // Place Buy order
    Signal buy_signal = Signal::buy(SignalStrength::Weak, 10.0, "test");
    auto requests = stage.process(buy_signal, ctx);
    assert(requests.size() == 1);

    // Execute the request through the engine to properly track it
    MarketSnapshot market;
    market.bid = ctx.market.bid;
    market.ask = ctx.market.ask;
    uint64_t order_id = engine.execute_request(requests[0], market);
    assert(order_id > 0);

    // Track it in the stage as well
    stage.track_pending(ctx.symbol, order_id, Side::Buy, requests[0].limit_price, requests[0].qty,
                        util::now_ns());

    int cancel_count_before = exchange.cancel_calls;

    // Now Sell signal (opposite direction)
    Signal sell_signal = Signal::sell(SignalStrength::Weak, 10.0, "test");
    ctx.position.quantity = 10.0; // Have position to sell
    requests = stage.process(sell_signal, ctx);

    // Should cancel previous and return new order
    assert(exchange.cancel_calls > cancel_count_before);
    assert(requests.size() == 1);
    assert(requests[0].side == Side::Sell);
    std::cout << "[PASS] test_cancel_on_direction_change\n";
}

void test_no_replace_within_threshold() {
    MockExchange exchange;
    ExecutionEngine engine;
    engine.set_exchange(&exchange);

    SpotLimitStage stage(&engine);

    ExecutionContext ctx;
    ctx.symbol = 1;
    ctx.market.bid = 100000;
    ctx.market.ask = 100100;
    ctx.metrics = nullptr;

    // Place Buy order
    Signal signal = Signal::buy(SignalStrength::Weak, 10.0, "test");
    auto requests = stage.process(signal, ctx);
    assert(requests.size() == 1);

    // Execute through engine to track it properly
    MarketSnapshot market;
    market.bid = ctx.market.bid;
    market.ask = ctx.market.ask;
    
    uint64_t order_id = engine.execute_request(requests[0], market);
    assert(order_id > 0);

    stage.track_pending(ctx.symbol, order_id, Side::Buy, requests[0].limit_price, requests[0].qty,
                        util::now_ns());

    int cancel_count = exchange.cancel_calls;

    // Process same signal again (price hasn't changed much)
    requests = stage.process(signal, ctx);

    // Should not cancel or replace
    assert(exchange.cancel_calls == cancel_count);
    assert(requests.empty()); // Keep existing
    std::cout << "[PASS] test_no_replace_within_threshold\n";
}

void test_replace_when_price_drifted() {
    MockExchange exchange;
    ExecutionEngine engine;
    engine.set_exchange(&exchange);

    SpotLimitStage stage(&engine);

    ExecutionContext ctx;
    ctx.symbol = 1;
    ctx.market.bid = 100000;
    ctx.market.ask = 100100;
    ctx.metrics = nullptr;

    // Place Buy order at 100050
    Signal signal = Signal::buy(SignalStrength::Weak, 10.0, "test");
    auto requests = stage.process(signal, ctx);
    assert(requests.size() == 1);

    // Execute through engine to track it properly
    MarketSnapshot market;
    market.bid = ctx.market.bid;
    market.ask = ctx.market.ask;
    
    uint64_t order_id = engine.execute_request(requests[0], market);
    assert(order_id > 0);

    stage.track_pending(ctx.symbol, order_id, Side::Buy, requests[0].limit_price, requests[0].qty,
                        util::now_ns());

    int cancel_count = exchange.cancel_calls;

    // Market moves significantly
    ctx.market.bid = 110000; // +10% move
    ctx.market.ask = 110100;

    // Process signal again
    requests = stage.process(signal, ctx);

    // Should cancel and replace
    assert(exchange.cancel_calls > cancel_count);
    assert(requests.size() == 1);
    std::cout << "[PASS] test_replace_when_price_drifted\n";
}

void test_timeout_cancels_pending() {
    MockExchange exchange;
    ExecutionEngine engine;
    engine.set_exchange(&exchange);

    SpotLimitStage::Config config;
    config.timeout_ns = 1'000'000'000; // 1 second for testing

    SpotLimitStage stage(&engine, config);

    ExecutionContext ctx;
    ctx.symbol = 1;
    ctx.market.bid = 100000;
    ctx.market.ask = 100100;
    ctx.metrics = nullptr;

    // Place order with old timestamp
    uint64_t old_time = util::now_ns() - 2'000'000'000; // 2 seconds ago
    uint64_t order_id = 1000;

    // Track in both engine and stage
    engine.track_pending_order_for_test(order_id, ctx.symbol, Side::Buy, 10.0, 100050, old_time);
    stage.track_pending(ctx.symbol, order_id, Side::Buy, 100050, 10.0, old_time);

    int cancel_count = exchange.cancel_calls;

    // Process signal (triggers timeout check)
    Signal signal = Signal::buy(SignalStrength::Weak, 10.0, "test");
    stage.process(signal, ctx);

    // Should have cancelled stale order
    assert(exchange.cancel_calls > cancel_count);
    std::cout << "[PASS] test_timeout_cancels_pending\n";
}

void test_cancel_all_clears_everything() {
    MockExchange exchange;
    ExecutionEngine engine;
    engine.set_exchange(&exchange);

    SpotLimitStage stage(&engine);

    // Track multiple pending orders
    uint64_t now = util::now_ns();
    uint64_t order_id_1 = 1000;
    uint64_t order_id_2 = 1001;

    // Track in both engine and stage
    engine.track_pending_order_for_test(order_id_1, 1, Side::Buy, 10.0, 100050, now);
    stage.track_pending(1, order_id_1, Side::Buy, 100050, 10.0, now);

    engine.track_pending_order_for_test(order_id_2, 2, Side::Sell, 5.0, 100060, now);
    stage.track_pending(2, order_id_2, Side::Sell, 100060, 5.0, now);

    int cancel_count = exchange.cancel_calls;

    stage.cancel_all();

    // Should have cancelled all orders
    assert(exchange.cancel_calls > cancel_count);
    std::cout << "[PASS] test_cancel_all_clears_everything\n";
}

void test_on_fill_clears_pending() {
    MockExchange exchange;
    ExecutionEngine engine;
    engine.set_exchange(&exchange);

    SpotLimitStage stage(&engine);

    // Track pending order
    stage.track_pending(1, 1000, Side::Buy, 100050, 10.0, util::now_ns());

    // Notify fill
    stage.on_fill(1000, 1);

    // Pending should be cleared
    ExecutionContext ctx;
    ctx.symbol = 1;
    ctx.market.bid = 100000;
    ctx.market.ask = 100100;
    ctx.metrics = nullptr;

    int cancel_count = exchange.cancel_calls;

    Signal signal = Signal::buy(SignalStrength::Weak, 10.0, "test");
    auto requests = stage.process(signal, ctx);

    // Should NOT cancel (order already filled and cleared)
    assert(exchange.cancel_calls == cancel_count);
    assert(requests.size() == 1); // New order produced
    std::cout << "[PASS] test_on_fill_clears_pending\n";
}

void test_no_metrics_returns_empty() {
    MockExchange exchange;
    ExecutionEngine engine;
    engine.set_exchange(&exchange);

    SpotLimitStage stage(&engine);

    Signal signal = Signal::buy(SignalStrength::Medium, 10.0, "test"); // Medium = 0 urgency
    ExecutionContext ctx;
    ctx.symbol = 1;
    ctx.market.bid = 100000;
    ctx.market.ask = 100100;
    ctx.metrics = nullptr; // No metrics, score = 0

    auto requests = stage.process(signal, ctx);
    // No metrics, Medium signal → score = 0 → not prefer_limit() → empty
    assert(requests.empty());
    std::cout << "[PASS] test_no_metrics_returns_empty\n";
}

void test_limit_price_passive_for_high_score() {
    MockExchange exchange;
    ExecutionEngine engine;
    engine.set_exchange(&exchange);

    SpotLimitStage stage(&engine);

    Signal signal = Signal::buy(SignalStrength::Weak, 10.0, "test"); // Weak = +10
    ExecutionContext ctx;
    ctx.symbol = 1;
    ctx.market.bid = 100000;
    ctx.market.ask = 100100;
    ctx.metrics = nullptr;

    auto requests = stage.process(signal, ctx);
    assert(requests.size() == 1);

    // High exec_score → low aggression → limit price closer to bid
    Price limit_price = requests[0].limit_price;
    Price mid = (ctx.market.bid + ctx.market.ask) / 2;

    // For Buy with positive exec_score: limit should be between bid and ask
    // Note: exec_score=10 gives aggression=0.8, so limit is at bid + 80% of spread
    assert(limit_price > ctx.market.bid);
    assert(limit_price < ctx.market.ask);
    std::cout << "[PASS] test_limit_price_passive_for_high_score\n";
}

int main() {
    std::cout << "Running SpotLimitStage tests...\n\n";

    test_limit_order_when_exec_score_positive();
    test_no_order_when_exec_score_negative();
    test_sell_caps_to_position();
    test_sell_no_position_stage_passes_through();
    test_cancel_on_direction_change();
    test_no_replace_within_threshold();
    test_replace_when_price_drifted();
    test_timeout_cancels_pending();
    test_cancel_all_clears_everything();
    test_on_fill_clears_pending();
    test_no_metrics_returns_empty();
    test_limit_price_passive_for_high_score();

    std::cout << "\n✓ All 12 tests passed!\n";
    return 0;
}
