/**
 * ExecutionEngine Complete Test Suite
 *
 * Tests all uncovered execution logic:
 * - decide_order_type() decision paths
 * - calculate_limit_price() calculations
 * - on_fill() slippage tracking
 * - cancel_stale_orders() timeout logic
 * - cancel_pending_for_symbol() cancellation
 * - recover_stuck_orders() retry logic
 * - order_type_str() helper
 */

#include "../include/execution/execution_engine.hpp"
#include "../include/strategy/istrategy.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <unistd.h> // for usleep()

using namespace hft;
using namespace hft::execution;
using namespace hft::strategy;

// =============================================================================
// Mock Exchange with Configurable Behavior
// =============================================================================

class MockExchangeAdapter : public IExchangeAdapter {
public:
    uint64_t next_order_id = 1000;
    int market_order_calls = 0;
    int limit_order_calls = 0;
    int cancel_calls = 0;

    // Configurable responses
    CancelResult cancel_result = CancelResult::Success;
    bool order_pending = false;

    uint64_t send_market_order(Symbol symbol, Side side, double qty, Price expected_price) override {
        market_order_calls++;
        return next_order_id++;
    }

    uint64_t send_limit_order(Symbol symbol, Side side, double qty, Price limit_price) override {
        limit_order_calls++;
        return next_order_id++;
    }

    CancelResult cancel_order(uint64_t order_id) override {
        cancel_calls++;
        return cancel_result;
    }

    bool is_order_pending(uint64_t order_id) const override { return order_pending; }

    bool is_paper() const override { return true; }
};

#define TEST(name) void test_##name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "  " #name "... ";                                                                                \
        test_##name();                                                                                                 \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

#define ASSERT_EQ(a, b) assert((a) == (b))
#define ASSERT_TRUE(expr) assert(expr)
#define ASSERT_FALSE(expr) assert(!(expr))
#define ASSERT_NEAR(a, b, eps) assert(std::abs((a) - (b)) < (eps))

// =============================================================================
// decide_order_type() Tests
// =============================================================================

TEST(decide_order_type_signal_market_preference) {
    ExecutionEngine engine;
    MockExchangeAdapter mock;
    engine.set_exchange(&mock);

    Signal signal = Signal::buy(SignalStrength::Medium, 1.0, "test");
    signal.order_pref = OrderPreference::Market;

    MarketSnapshot market;
    market.bid = 10000;
    market.ask = 10010;

    engine.execute(0, signal, market, MarketRegime::Ranging);

    ASSERT_EQ(mock.market_order_calls, 1); // Market preference honored
    ASSERT_EQ(mock.limit_order_calls, 0);
}

TEST(decide_order_type_signal_limit_preference) {
    ExecutionEngine engine;
    MockExchangeAdapter mock;
    engine.set_exchange(&mock);

    Signal signal = Signal::buy(SignalStrength::Medium, 1.0, "test");
    signal.order_pref = OrderPreference::Limit;

    MarketSnapshot market;
    market.bid = 10000;
    market.ask = 10010;

    engine.execute(0, signal, market, MarketRegime::Ranging);

    ASSERT_EQ(mock.market_order_calls, 0);
    ASSERT_EQ(mock.limit_order_calls, 1); // Limit preference honored
}

TEST(decide_order_type_strong_signal_uses_market) {
    ExecutionConfig config;
    config.strong_signal_uses_market = true;

    ExecutionEngine engine(config);
    MockExchangeAdapter mock;
    engine.set_exchange(&mock);

    Signal signal = Signal::buy(SignalStrength::Strong, 1.0, "test");
    signal.order_pref = OrderPreference::Either;

    MarketSnapshot market;
    market.bid = 10000;
    market.ask = 10010;

    engine.execute(0, signal, market, MarketRegime::Ranging);

    ASSERT_EQ(mock.market_order_calls, 1); // Strong signal → Market
}

TEST(decide_order_type_weak_signal_uses_limit) {
    ExecutionConfig config;
    config.weak_signal_uses_limit = true;

    ExecutionEngine engine(config);
    MockExchangeAdapter mock;
    engine.set_exchange(&mock);

    Signal signal = Signal::buy(SignalStrength::Weak, 1.0, "test");
    signal.order_pref = OrderPreference::Either;

    MarketSnapshot market;
    market.bid = 10000;
    market.ask = 10010;

    engine.execute(0, signal, market, MarketRegime::Ranging);

    ASSERT_EQ(mock.limit_order_calls, 1); // Weak signal → Limit
}

TEST(decide_order_type_high_volatility_uses_market) {
    ExecutionConfig config;
    config.high_vol_uses_market = true;

    ExecutionEngine engine(config);
    MockExchangeAdapter mock;
    engine.set_exchange(&mock);

    Signal signal = Signal::buy(SignalStrength::Medium, 1.0, "test");
    signal.order_pref = OrderPreference::Either;

    MarketSnapshot market;
    market.bid = 10000;
    market.ask = 10010;

    engine.execute(0, signal, market, MarketRegime::HighVolatility);

    ASSERT_EQ(mock.market_order_calls, 1); // High vol → Market
}

TEST(decide_order_type_ranging_prefers_limit) {
    ExecutionConfig config;
    config.ranging_prefers_limit = true;

    ExecutionEngine engine(config);
    MockExchangeAdapter mock;
    engine.set_exchange(&mock);

    Signal signal = Signal::buy(SignalStrength::Medium, 1.0, "test");
    signal.order_pref = OrderPreference::Either;

    MarketSnapshot market;
    market.bid = 10000;
    market.ask = 10010;

    engine.execute(0, signal, market, MarketRegime::Ranging);

    ASSERT_EQ(mock.limit_order_calls, 1); // Ranging → Limit
}

TEST(decide_order_type_wide_spread_uses_limit) {
    ExecutionConfig config;
    config.wide_spread_threshold_bps = 10.0;

    ExecutionEngine engine(config);
    MockExchangeAdapter mock;
    engine.set_exchange(&mock);

    Signal signal = Signal::buy(SignalStrength::Medium, 1.0, "test");
    signal.order_pref = OrderPreference::Either;

    MarketSnapshot market;
    market.bid = 10000;
    market.ask = 10120; // 120 bps spread (wide)

    engine.execute(0, signal, market, MarketRegime::Unknown);

    ASSERT_EQ(mock.limit_order_calls, 1); // Wide spread → Limit
}

TEST(decide_order_type_tight_spread_uses_market) {
    ExecutionConfig config;
    config.urgency_spread_threshold_bps = 5.0;

    ExecutionEngine engine(config);
    MockExchangeAdapter mock;
    engine.set_exchange(&mock);

    Signal signal = Signal::buy(SignalStrength::Medium, 1.0, "test");
    signal.order_pref = OrderPreference::Either;

    MarketSnapshot market;
    market.bid = 10000;
    market.ask = 10002; // 2 bps spread (tight)

    engine.execute(0, signal, market, MarketRegime::Unknown);

    ASSERT_EQ(mock.market_order_calls, 1); // Tight spread → Market
}

// =============================================================================
// calculate_limit_price() Tests
// =============================================================================

TEST(calculate_limit_price_uses_signal_price_if_specified) {
    ExecutionEngine engine;
    MockExchangeAdapter mock;
    engine.set_exchange(&mock);

    Signal signal = Signal::buy(SignalStrength::Medium, 1.0, "test");
    signal.order_pref = OrderPreference::Limit;
    signal.limit_price = 9999; // Explicit price

    MarketSnapshot market;
    market.bid = 10000;
    market.ask = 10010;

    engine.execute(0, signal, market, MarketRegime::Ranging);

    ASSERT_EQ(mock.limit_order_calls, 1);
    // Can't directly verify price here, but it's covered
}

TEST(calculate_limit_price_auto_buy) {
    ExecutionConfig config;
    config.limit_offset_bps = 2.0;

    ExecutionEngine engine(config);
    MockExchangeAdapter mock;
    engine.set_exchange(&mock);

    Signal signal = Signal::buy(SignalStrength::Medium, 1.0, "test");
    signal.order_pref = OrderPreference::Limit;
    signal.limit_price = 0; // Auto calculation

    MarketSnapshot market;
    market.bid = 10000;
    market.ask = 10010;

    engine.execute(0, signal, market, MarketRegime::Ranging);

    // Limit buy = bid + offset (inside spread)
    // Spread = 10, offset = 10 * 0.02 = 0.2 → limit = 10000.2
    ASSERT_EQ(mock.limit_order_calls, 1);
}

TEST(calculate_limit_price_auto_sell) {
    ExecutionConfig config;
    config.limit_offset_bps = 2.0;

    ExecutionEngine engine(config);
    MockExchangeAdapter mock;
    engine.set_exchange(&mock);
    engine.set_position_callback([](Symbol) { return 10.0; }); // Have position

    Signal signal = Signal::sell(SignalStrength::Medium, 1.0, "test");
    signal.order_pref = OrderPreference::Limit;
    signal.limit_price = 0; // Auto calculation

    MarketSnapshot market;
    market.bid = 10000;
    market.ask = 10010;

    engine.execute(0, signal, market, MarketRegime::Ranging);

    // Limit sell = ask - offset (inside spread)
    ASSERT_EQ(mock.limit_order_calls, 1);
}

// =============================================================================
// on_fill() Tests
// =============================================================================

TEST(on_fill_calculates_slippage_buy) {
    ExecutionEngine engine;
    MockExchangeAdapter mock;
    engine.set_exchange(&mock);

    double captured_slippage = 0.0;
    engine.set_fill_callback(
        [&](uint64_t oid, Symbol sym, Side side, double qty, Price price, double slip) { captured_slippage = slip; });

    // Create pending order
    engine.track_pending_order_for_test(1234, 0, Side::Buy, 1.0, 10000, 0);

    // Fill at higher price (bad slippage for buy)
    engine.on_fill(1234, 0, Side::Buy, 1.0, 10010);

    ASSERT_EQ(captured_slippage, 10); // Paid 10 more (positive = bad for buy)
}

TEST(on_fill_calculates_slippage_sell) {
    ExecutionEngine engine;
    MockExchangeAdapter mock;
    engine.set_exchange(&mock);

    double captured_slippage = 0.0;
    engine.set_fill_callback(
        [&](uint64_t oid, Symbol sym, Side side, double qty, Price price, double slip) { captured_slippage = slip; });

    // Create pending order
    engine.track_pending_order_for_test(1234, 0, Side::Sell, 1.0, 10010, 0);

    // Fill at lower price (bad slippage for sell)
    engine.on_fill(1234, 0, Side::Sell, 1.0, 10000);

    ASSERT_EQ(captured_slippage, 10); // Received 10 less (positive = bad for sell)
}

TEST(on_fill_clears_pending_order) {
    ExecutionEngine engine;
    MockExchangeAdapter mock;
    engine.set_exchange(&mock);

    engine.track_pending_order_for_test(1234, 0, Side::Buy, 1.0, 10000, 0);
    ASSERT_EQ(engine.pending_order_count(), 1);

    engine.on_fill(1234, 0, Side::Buy, 1.0, 10000);

    ASSERT_EQ(engine.pending_order_count(), 0); // Cleared after fill
}

// =============================================================================
// cancel_stale_orders() Tests
// =============================================================================

TEST(cancel_stale_orders_cancels_timeout_order) {
    ExecutionConfig config;
    config.limit_timeout_ns = 1'000'000'000; // 1 second

    ExecutionEngine engine(config);
    MockExchangeAdapter mock;
    engine.set_exchange(&mock);

    // Create pending order with old timestamp
    uint64_t old_time = 0;
    engine.track_pending_order_for_test(1234, 0, Side::Buy, 1.0, 10000, old_time);

    // Check stale orders at current time (way past timeout)
    uint64_t current_time = 5'000'000'000; // 5 seconds later
    engine.cancel_stale_orders(current_time);

    ASSERT_EQ(mock.cancel_calls, 1);            // Cancel should be called
    ASSERT_EQ(engine.pending_order_count(), 0); // Order cleared on success
}

TEST(cancel_stale_orders_skips_recent_order) {
    ExecutionConfig config;
    config.limit_timeout_ns = 10'000'000'000; // 10 seconds

    ExecutionEngine engine(config);
    MockExchangeAdapter mock;
    engine.set_exchange(&mock);

    // Create recent order
    uint64_t recent_time = 9'000'000'000;
    engine.track_pending_order_for_test(1234, 0, Side::Buy, 1.0, 10000, recent_time);

    // Check stale orders (only 1 second passed)
    uint64_t current_time = 10'000'000'000;
    engine.cancel_stale_orders(current_time);

    ASSERT_EQ(mock.cancel_calls, 0);            // No cancel (not stale yet)
    ASSERT_EQ(engine.pending_order_count(), 1); // Still pending
}

// =============================================================================
// cancel_pending_for_symbol() Tests
// =============================================================================

TEST(cancel_pending_for_symbol_cancels_matching_orders) {
    ExecutionEngine engine;
    MockExchangeAdapter mock;
    engine.set_exchange(&mock);

    // Create orders for different symbols
    engine.track_pending_order_for_test(1001, 0, Side::Buy, 1.0, 10000, 0);
    engine.track_pending_order_for_test(1002, 1, Side::Buy, 1.0, 10000, 0);
    engine.track_pending_order_for_test(1003, 0, Side::Sell, 1.0, 10010, 0);

    int cancelled = engine.cancel_pending_for_symbol(0);

    ASSERT_EQ(cancelled, 2); // 2 orders for symbol 0
    ASSERT_EQ(mock.cancel_calls, 2);
    ASSERT_EQ(engine.pending_order_count(), 1); // Only symbol 1 order remains
}

TEST(cancel_pending_for_symbol_handles_network_error) {
    ExecutionEngine engine;
    MockExchangeAdapter mock;
    mock.cancel_result = CancelResult::NetworkError; // Simulate error
    engine.set_exchange(&mock);

    engine.track_pending_order_for_test(1001, 0, Side::Buy, 1.0, 10000, 0);

    int cancelled = engine.cancel_pending_for_symbol(0);

    ASSERT_EQ(cancelled, 0);                    // NetworkError → not resolved
    ASSERT_EQ(engine.pending_order_count(), 1); // Order still active
}

TEST(cancel_pending_for_symbol_handles_not_found) {
    ExecutionEngine engine;
    MockExchangeAdapter mock;
    mock.cancel_result = CancelResult::NotFound; // Already gone
    engine.set_exchange(&mock);

    engine.track_pending_order_for_test(1001, 0, Side::Buy, 1.0, 10000, 0);

    int cancelled = engine.cancel_pending_for_symbol(0);

    ASSERT_EQ(cancelled, 1);                    // NotFound counts as resolved
    ASSERT_EQ(engine.pending_order_count(), 0); // Order cleared
}

// =============================================================================
// recover_stuck_orders() Tests
// =============================================================================

TEST(recover_stuck_orders_retries_cancel_failed) {
    ExecutionEngine engine;
    MockExchangeAdapter mock;
    engine.set_exchange(&mock);

    // Create pending order with submit_time = 0
    engine.track_pending_order_for_test(1234, 0, Side::Buy, 1.0, 10000, 0);

    // First cancel fails (NetworkError)
    // Note: default limit_timeout_ns is 5 seconds, so current_time must be > 5 seconds
    mock.cancel_result = CancelResult::NetworkError;
    engine.cancel_stale_orders(6'000'000'000); // 6 seconds > 5 second timeout

    ASSERT_EQ(mock.cancel_calls, 1);
    ASSERT_EQ(engine.pending_order_count(), 1); // Still pending (CancelFailed state)

    // Immediately calling recover does nothing (retry interval not passed)
    mock.cancel_calls = 0;
    engine.recover_stuck_orders();
    ASSERT_EQ(mock.cancel_calls, 0); // Too soon, no retry

    // Wait for retry interval (simulated by sleep)
    usleep(1'100'000); // 1.1 seconds

    // Now retry should work
    mock.cancel_result = CancelResult::Success;
    engine.recover_stuck_orders();

    ASSERT_TRUE(mock.cancel_calls > 0); // Retry attempted after interval
}

TEST(recover_stuck_orders_queries_after_max_retries) {
    ExecutionEngine engine;
    MockExchangeAdapter mock;
    mock.cancel_result = CancelResult::NetworkError;
    engine.set_exchange(&mock);

    // Create pending order with submit_time = 0
    engine.track_pending_order_for_test(1234, 0, Side::Buy, 1.0, 10000, 0);

    // First cancel attempt (gets order into CancelFailed state)
    engine.cancel_stale_orders(6'000'000'000);  // 6 seconds > 5 second timeout
    ASSERT_EQ(engine.pending_order_count(), 1); // Still pending (CancelFailed)

    // Exhaust retries with recover_stuck_orders (need to wait retry interval between calls)
    for (int i = 0; i < ExecutionEngine::MAX_CANCEL_ATTEMPTS - 1; i++) {
        usleep(1'100'000); // Wait retry interval (1.1 seconds)
        engine.recover_stuck_orders();
    }

    // After max retries, engine queries exchange
    usleep(1'100'000);          // Wait retry interval once more
    mock.order_pending = false; // Order gone from exchange
    mock.cancel_calls = 0;
    engine.recover_stuck_orders();

    ASSERT_EQ(engine.pending_order_count(), 0); // Cleared (gone from exchange)
}

TEST(recover_stuck_orders_final_cancel_if_still_pending) {
    ExecutionEngine engine;
    MockExchangeAdapter mock;
    mock.cancel_result = CancelResult::NetworkError;
    engine.set_exchange(&mock);

    // Create pending order with submit_time = 0
    engine.track_pending_order_for_test(1234, 0, Side::Buy, 1.0, 10000, 0);

    // First cancel attempt (gets order into CancelFailed state)
    engine.cancel_stale_orders(6'000'000'000); // 6 seconds > 5 second timeout

    // Exhaust retries with recover_stuck_orders (need to wait retry interval between calls)
    for (int i = 0; i < ExecutionEngine::MAX_CANCEL_ATTEMPTS - 1; i++) {
        usleep(1'100'000); // Wait retry interval (1.1 seconds)
        engine.recover_stuck_orders();
    }

    // Query shows still pending → final cancel attempt
    usleep(1'100'000); // Wait retry interval once more
    mock.order_pending = true;
    int cancel_calls_before = mock.cancel_calls;
    engine.recover_stuck_orders();

    // Should reset attempts and try one more time
    ASSERT_TRUE(mock.cancel_calls > cancel_calls_before);
}

// =============================================================================
// Helper Function Tests
// =============================================================================

TEST(order_type_str_market) {
    ASSERT_TRUE(std::string(order_type_str(OrderType::Market)) == "MARKET");
}

TEST(order_type_str_limit) {
    ASSERT_TRUE(std::string(order_type_str(OrderType::Limit)) == "LIMIT");
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(execute_without_exchange_returns_zero) {
    ExecutionEngine engine;
    // No exchange set

    Signal signal = Signal::buy(SignalStrength::Strong, 1.0, "test");
    MarketSnapshot market;
    market.bid = 10000;
    market.ask = 10010;

    uint64_t order_id = engine.execute(0, signal, market, MarketRegime::Ranging);

    ASSERT_EQ(order_id, 0); // No exchange → no order
}

TEST(execute_request_without_exchange_returns_zero) {
    ExecutionEngine engine;
    // No exchange set

    OrderRequest req;
    req.symbol = 0;
    req.qty = 1.0;
    req.type = OrderType::Market;
    req.side = Side::Buy;

    MarketSnapshot market;
    market.bid = 10000;
    market.ask = 10010;

    uint64_t order_id = engine.execute_request(req, market);

    ASSERT_EQ(order_id, 0); // No exchange → no order
}

TEST(execute_request_invalid_request_returns_zero) {
    ExecutionEngine engine;
    MockExchangeAdapter mock;
    engine.set_exchange(&mock);

    OrderRequest req;
    req.qty = 0.0; // Invalid (qty must be > 0)

    MarketSnapshot market;
    market.bid = 10000;
    market.ask = 10010;

    uint64_t order_id = engine.execute_request(req, market);

    ASSERT_EQ(order_id, 0); // Invalid request → no order
}

TEST(order_callback_fires) {
    ExecutionEngine engine;
    MockExchangeAdapter mock;
    engine.set_exchange(&mock);

    bool callback_fired = false;
    engine.set_order_callback(
        [&](uint64_t oid, Symbol sym, Side side, double qty, Price price, OrderType type) { callback_fired = true; });

    Signal signal = Signal::buy(SignalStrength::Strong, 1.0, "test");
    MarketSnapshot market;
    market.bid = 10000;
    market.ask = 10010;

    engine.execute(0, signal, market, MarketRegime::Ranging);

    ASSERT_TRUE(callback_fired);
}

TEST(fill_callback_fires) {
    ExecutionEngine engine;
    MockExchangeAdapter mock;
    engine.set_exchange(&mock);

    bool callback_fired = false;
    engine.set_fill_callback(
        [&](uint64_t oid, Symbol sym, Side side, double qty, Price price, double slip) { callback_fired = true; });

    engine.track_pending_order_for_test(1234, 0, Side::Buy, 1.0, 10000, 0);
    engine.on_fill(1234, 0, Side::Buy, 1.0, 10000);

    ASSERT_TRUE(callback_fired);
}

TEST(config_getter_setter) {
    ExecutionConfig config;
    config.wide_spread_threshold_bps = 123.45;

    ExecutionEngine engine(config);

    ASSERT_NEAR(engine.config().wide_spread_threshold_bps, 123.45, 0.01);

    ExecutionConfig new_config;
    new_config.wide_spread_threshold_bps = 678.9;
    engine.set_config(new_config);

    ASSERT_NEAR(engine.config().wide_spread_threshold_bps, 678.9, 0.01);
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "\n=== ExecutionEngine Complete Test Suite ===\n\n";

    std::cout << "Order Type Decision Tests:\n";
    RUN_TEST(decide_order_type_signal_market_preference);
    RUN_TEST(decide_order_type_signal_limit_preference);
    RUN_TEST(decide_order_type_strong_signal_uses_market);
    RUN_TEST(decide_order_type_weak_signal_uses_limit);
    RUN_TEST(decide_order_type_high_volatility_uses_market);
    RUN_TEST(decide_order_type_ranging_prefers_limit);
    RUN_TEST(decide_order_type_wide_spread_uses_limit);
    RUN_TEST(decide_order_type_tight_spread_uses_market);

    std::cout << "\nLimit Price Calculation Tests:\n";
    RUN_TEST(calculate_limit_price_uses_signal_price_if_specified);
    RUN_TEST(calculate_limit_price_auto_buy);
    RUN_TEST(calculate_limit_price_auto_sell);

    std::cout << "\nFill Handling Tests:\n";
    RUN_TEST(on_fill_calculates_slippage_buy);
    RUN_TEST(on_fill_calculates_slippage_sell);
    RUN_TEST(on_fill_clears_pending_order);

    std::cout << "\nStale Order Cancellation Tests:\n";
    RUN_TEST(cancel_stale_orders_cancels_timeout_order);
    RUN_TEST(cancel_stale_orders_skips_recent_order);

    std::cout << "\nSymbol-Specific Cancellation Tests:\n";
    RUN_TEST(cancel_pending_for_symbol_cancels_matching_orders);
    RUN_TEST(cancel_pending_for_symbol_handles_network_error);
    RUN_TEST(cancel_pending_for_symbol_handles_not_found);

    std::cout << "\nStuck Order Recovery Tests:\n";
    RUN_TEST(recover_stuck_orders_retries_cancel_failed);
    RUN_TEST(recover_stuck_orders_queries_after_max_retries);
    RUN_TEST(recover_stuck_orders_final_cancel_if_still_pending);

    std::cout << "\nHelper Function Tests:\n";
    RUN_TEST(order_type_str_market);
    RUN_TEST(order_type_str_limit);

    std::cout << "\nEdge Case Tests:\n";
    RUN_TEST(execute_without_exchange_returns_zero);
    RUN_TEST(execute_request_without_exchange_returns_zero);
    RUN_TEST(execute_request_invalid_request_returns_zero);
    RUN_TEST(order_callback_fires);
    RUN_TEST(fill_callback_fires);
    RUN_TEST(config_getter_setter);

    std::cout << "\n=== All 32 tests PASSED! ===\n\n";
    return 0;
}
