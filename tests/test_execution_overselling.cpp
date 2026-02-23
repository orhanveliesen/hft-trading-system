/**
 * ExecutionEngine Overselling Prevention Test Suite
 *
 * Tests that ExecutionEngine prevents selling more than owned position.
 * This is a critical safety feature for trading systems.
 *
 * BUG DISCOVERED: ExecutionEngine was executing SELL orders without
 * checking if there was enough position, leading to overselling.
 *
 * Run with: ./test_execution_overselling
 */

#include "../include/execution/execution_engine.hpp"
#include "../include/strategy/istrategy.hpp"

#include <cassert>
#include <cmath>
#include <functional>
#include <iostream>
#include <string>

using namespace hft;
using namespace hft::execution;
using namespace hft::strategy;

// =============================================================================
// Mock Exchange Adapter for Testing
// =============================================================================

class MockExchangeAdapter : public IExchangeAdapter {
public:
    // Track orders sent
    struct SentOrder {
        Symbol symbol;
        Side side;
        double qty;
        Price price;
        bool is_market;
    };

    std::vector<SentOrder> orders;
    uint64_t next_order_id = 1000;

    uint64_t send_market_order(Symbol symbol, Side side, double qty, Price expected_price) override {
        orders.push_back({symbol, side, qty, expected_price, true});
        return next_order_id++;
    }

    uint64_t send_limit_order(Symbol symbol, Side side, double qty, Price limit_price) override {
        orders.push_back({symbol, side, qty, limit_price, false});
        return next_order_id++;
    }

    bool cancel_order(uint64_t order_id) override { return true; }

    bool is_order_pending(uint64_t order_id) const override { return false; }

    bool is_paper() const override { return true; }

    void clear() { orders.clear(); }
};

// =============================================================================
// Test Macros
// =============================================================================

#define TEST(name) void test_##name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "  Running " #name "... ";                                                                        \
        test_##name();                                                                                                 \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

#define ASSERT_EQ(a, b)                                                                                                \
    do {                                                                                                               \
        if ((a) != (b)) {                                                                                              \
            std::cerr << "\nFAILED: " << #a << " != " << #b << "\n";                                                   \
            assert(false);                                                                                             \
        }                                                                                                              \
    } while (0)

#define ASSERT_DOUBLE_NEAR(a, b, tol)                                                                                  \
    do {                                                                                                               \
        if (std::abs((a) - (b)) > (tol)) {                                                                             \
            std::cerr << "\nFAILED: " << #a << " != " << #b << " (" << (a) << " != " << (b) << ")\n";                  \
            assert(false);                                                                                             \
        }                                                                                                              \
    } while (0)

#define ASSERT_TRUE(expr)                                                                                              \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            std::cerr << "\nFAILED: " << #expr << " is false\n";                                                       \
            assert(false);                                                                                             \
        }                                                                                                              \
    } while (0)

// =============================================================================
// Tests
// =============================================================================

TEST(buy_order_executes_without_position_check) {
    // BUY orders should always execute (subject to capital limits, but not position)
    MockExchangeAdapter mock;
    ExecutionEngine engine;
    engine.set_exchange(&mock);

    // Use factory method for signal
    Signal buy_signal = Signal::buy(SignalStrength::Strong, 1.0, "test");

    MarketSnapshot market;
    market.bid = 50000;
    market.ask = 50010;

    // No position callback set - buy should still work
    uint64_t order_id = engine.execute(0, buy_signal, market, MarketRegime::Ranging);

    ASSERT_TRUE(order_id > 0);
    ASSERT_EQ(mock.orders.size(), 1u);
    ASSERT_EQ(mock.orders[0].side, Side::Buy);
    ASSERT_DOUBLE_NEAR(mock.orders[0].qty, 1.0, 0.001);
}

TEST(sell_order_rejected_when_no_position) {
    // SELL orders should be rejected when no position exists
    MockExchangeAdapter mock;
    ExecutionEngine engine;
    engine.set_exchange(&mock);

    // Set position callback that returns 0 (no position)
    engine.set_position_callback([](Symbol) { return 0.0; });

    Signal sell_signal = Signal::sell(SignalStrength::Strong, 1.0, "test");

    MarketSnapshot market;
    market.bid = 50000;
    market.ask = 50010;

    uint64_t order_id = engine.execute(0, sell_signal, market, MarketRegime::Ranging);

    // Order should be rejected (0 = no order sent)
    ASSERT_EQ(order_id, 0u);
    ASSERT_EQ(mock.orders.size(), 0u); // No order should be sent
}

TEST(sell_order_limited_to_available_position) {
    // SELL orders should be limited to available position
    MockExchangeAdapter mock;
    ExecutionEngine engine;
    engine.set_exchange(&mock);

    // Position callback: we own 0.5 units
    engine.set_position_callback([](Symbol) { return 0.5; });

    Signal sell_signal = Signal::sell(SignalStrength::Strong, 1.0, "test"); // Try to sell 1.0

    MarketSnapshot market;
    market.bid = 50000;
    market.ask = 50010;

    uint64_t order_id = engine.execute(0, sell_signal, market, MarketRegime::Ranging);

    // Order should be sent but with limited quantity
    ASSERT_TRUE(order_id > 0);
    ASSERT_EQ(mock.orders.size(), 1u);
    ASSERT_EQ(mock.orders[0].side, Side::Sell);
    ASSERT_DOUBLE_NEAR(mock.orders[0].qty, 0.5, 0.001); // Limited to position
}

TEST(sell_order_full_quantity_when_position_sufficient) {
    // SELL orders should execute full quantity when position is sufficient
    MockExchangeAdapter mock;
    ExecutionEngine engine;
    engine.set_exchange(&mock);

    // Position callback: we own 2.0 units
    engine.set_position_callback([](Symbol) { return 2.0; });

    Signal sell_signal = Signal::sell(SignalStrength::Strong, 1.0, "test"); // Sell 1.0

    MarketSnapshot market;
    market.bid = 50000;
    market.ask = 50010;

    uint64_t order_id = engine.execute(0, sell_signal, market, MarketRegime::Ranging);

    ASSERT_TRUE(order_id > 0);
    ASSERT_EQ(mock.orders.size(), 1u);
    ASSERT_DOUBLE_NEAR(mock.orders[0].qty, 1.0, 0.001); // Full quantity
}

TEST(no_position_callback_allows_sell_for_backwards_compat) {
    // When no position callback is set, sell orders execute (backwards compatibility)
    // This might be the case for short-selling or other strategies
    MockExchangeAdapter mock;
    ExecutionEngine engine;
    engine.set_exchange(&mock);

    // No position callback set

    Signal sell_signal = Signal::sell(SignalStrength::Strong, 1.0, "test");

    MarketSnapshot market;
    market.bid = 50000;
    market.ask = 50010;

    uint64_t order_id = engine.execute(0, sell_signal, market, MarketRegime::Ranging);

    // Without callback, order goes through (backwards compatibility)
    ASSERT_TRUE(order_id > 0);
    ASSERT_EQ(mock.orders.size(), 1u);
}

TEST(position_callback_receives_correct_symbol) {
    // Verify the correct symbol ID is passed to position callback
    MockExchangeAdapter mock;
    ExecutionEngine engine;
    engine.set_exchange(&mock);

    Symbol received_symbol = 999;
    engine.set_position_callback([&received_symbol](Symbol s) {
        received_symbol = s;
        return 1.0; // Return some position
    });

    Signal sell_signal = Signal::sell(SignalStrength::Strong, 0.5, "test");

    MarketSnapshot market;
    market.bid = 50000;
    market.ask = 50010;

    Symbol expected_symbol = 42;
    engine.execute(expected_symbol, sell_signal, market, MarketRegime::Ranging);

    ASSERT_EQ(received_symbol, expected_symbol);
}

TEST(fractional_position_handled_correctly) {
    // Test with fractional crypto quantities
    MockExchangeAdapter mock;
    ExecutionEngine engine;
    engine.set_exchange(&mock);

    // Position: 0.03 BTC
    engine.set_position_callback([](Symbol) { return 0.03; });

    Signal sell_signal = Signal::sell(SignalStrength::Strong, 0.05, "test"); // Try to sell more

    MarketSnapshot market;
    market.bid = 50000;
    market.ask = 50010;

    uint64_t order_id = engine.execute(0, sell_signal, market, MarketRegime::Ranging);

    // Should be limited to 0.03
    ASSERT_TRUE(order_id > 0);
    ASSERT_DOUBLE_NEAR(mock.orders[0].qty, 0.03, 0.0001);
}

TEST(very_small_position_prevents_sell) {
    // Test that positions below minimum threshold are treated as 0
    MockExchangeAdapter mock;
    ExecutionEngine engine;
    engine.set_exchange(&mock);

    // Position: tiny amount (dust)
    engine.set_position_callback([](Symbol) { return 0.00001; });

    Signal sell_signal = Signal::sell(SignalStrength::Strong, 1.0, "test");

    MarketSnapshot market;
    market.bid = 50000;
    market.ask = 50010;

    uint64_t order_id = engine.execute(0, sell_signal, market, MarketRegime::Ranging);

    // Dust position - order rejected
    ASSERT_EQ(order_id, 0u);
    ASSERT_EQ(mock.orders.size(), 0u);
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "\n=== ExecutionEngine Overselling Prevention Tests ===\n\n";

    RUN_TEST(buy_order_executes_without_position_check);
    RUN_TEST(sell_order_rejected_when_no_position);
    RUN_TEST(sell_order_limited_to_available_position);
    RUN_TEST(sell_order_full_quantity_when_position_sufficient);
    RUN_TEST(no_position_callback_allows_sell_for_backwards_compat);
    RUN_TEST(position_callback_receives_correct_symbol);
    RUN_TEST(fractional_position_handled_correctly);
    RUN_TEST(very_small_position_prevents_sell);

    std::cout << "\n=== All tests PASSED! ===\n\n";
    return 0;
}
