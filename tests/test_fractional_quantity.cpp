/**
 * Fractional Quantity Test Suite
 *
 * Tests that fractional quantities (common in crypto trading) are correctly
 * passed through the entire fill callback chain:
 * - PaperExchange → PaperExchangeAdapter → FillCallback
 *
 * This test exists because Quantity was originally uint32_t, which truncated
 * fractional values like 0.01 BTC to 0.
 *
 * Run with: ./test_fractional_quantity
 */

#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <cmath>
#include <string>
#include "../include/exchange/paper_exchange_adapter.hpp"
#include "../include/ipc/shared_config.hpp"

using namespace hft;
using namespace hft::exchange;
using namespace hft::ipc;

// Test state for callback verification
static double received_qty = 0.0;
static double received_price = 0.0;
static std::string received_symbol;
static Side received_side = Side::Buy;
static int callback_count = 0;

static void reset_test_state() {
    received_qty = 0.0;
    received_price = 0.0;
    received_symbol.clear();
    received_side = Side::Buy;
    callback_count = 0;
}

// Test helpers
#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    reset_test_state(); \
    std::cout << "  Running " #name "... "; \
    test_##name(); \
    std::cout << "PASSED\n"; \
} while(0)

// Use abort() instead of assert() to ensure failure even in release mode
#define TEST_FAIL() do { std::cerr << std::flush; std::abort(); } while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        std::cerr << "\nFAILED: " << #a << " != " << #b << "\n"; \
        TEST_FAIL(); \
    } \
} while(0)

#define ASSERT_SIZE_EQ(a, b) do { \
    if ((a) != (b)) { \
        std::cerr << "\nFAILED: " << #a << " != " << #b << "\n"; \
        std::cerr << "  Actual: " << (a) << ", Expected: " << (b) << "\n"; \
        TEST_FAIL(); \
    } \
} while(0)

#define ASSERT_DOUBLE_EQ(a, b) do { \
    if (std::abs((a) - (b)) > 1e-9) { \
        std::cerr << "\nFAILED: " << #a << " != " << #b << "\n"; \
        std::cerr << "  Actual: " << std::setprecision(10) << (a) << ", Expected: " << (b) << "\n"; \
        TEST_FAIL(); \
    } \
} while(0)

#define ASSERT_DOUBLE_GT(a, b) do { \
    if ((a) <= (b)) { \
        std::cerr << "\nFAILED: " << #a << " > " << #b << "\n"; \
        std::cerr << "  Actual: " << std::setprecision(10) << (a) << " <= " << (b) << "\n"; \
        TEST_FAIL(); \
    } \
} while(0)

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        std::cerr << "\nFAILED: " << #expr << " is false\n"; \
        TEST_FAIL(); \
    } \
} while(0)

#define ASSERT_STREQ(a, b) do { \
    if (std::string(a) != std::string(b)) { \
        std::cerr << "\nFAILED: " << #a << " != " << #b << "\n"; \
        std::cerr << "  Actual: \"" << (a) << "\", Expected: \"" << (b) << "\"\n"; \
        TEST_FAIL(); \
    } \
} while(0)

/**
 * TEST: Fractional BTC quantity (0.01 BTC) through adapter callback
 *
 * This is the critical test - it verifies that when we buy 0.01 BTC,
 * the callback receives 0.01, not 0 (which would happen with uint32_t truncation).
 */
TEST(fractional_btc_quantity_through_callback) {
    PaperExchangeAdapter adapter;

    // Register symbol
    Symbol sym_id = adapter.register_symbol("BTCUSDT");

    // Set up fill callback to capture the quantity
    // NOTE: qty is now double (not Quantity/uint32_t) to support fractional crypto quantities
    adapter.set_fill_callback([](
        uint64_t order_id,
        const char* symbol_name,
        Side side,
        double qty,
        Price fill_price,
        double commission
    ) {
        received_qty = qty;  // Now receives double directly
        received_symbol = symbol_name;
        received_side = side;
        received_price = static_cast<double>(fill_price);
        callback_count++;
    });

    // Send limit order for 0.01 BTC at price 87000
    double fractional_qty = 0.01;  // 0.01 BTC
    double limit_price = 87000.0;
    Price limit_price_scaled = adapter.double_to_price(limit_price);

    uint64_t order_id = adapter.send_limit_order(
        sym_id, Side::Buy, fractional_qty, limit_price_scaled
    );
    ASSERT_TRUE(order_id > 0);
    ASSERT_SIZE_EQ(adapter.pending_order_count(), 1u);

    // Price update: ask drops below limit - should fill
    Price bid = adapter.double_to_price(86900.0);
    Price ask = adapter.double_to_price(86950.0);  // Below 87000 limit

    adapter.on_price_update(sym_id, bid, ask, 1000000);

    // Verify fill callback was called
    ASSERT_SIZE_EQ(callback_count, 1);
    ASSERT_SIZE_EQ(adapter.pending_order_count(), 0u);

    // THE CRITICAL ASSERTION: quantity should be 0.01, not 0
    // If this fails, the bug exists (uint32_t truncation)
    ASSERT_DOUBLE_GT(received_qty, 0.0);  // Must be > 0
    ASSERT_DOUBLE_EQ(received_qty, fractional_qty);  // Must be exactly 0.01

    ASSERT_STREQ(received_symbol.c_str(), "BTCUSDT");
    ASSERT_EQ(received_side, Side::Buy);
}

/**
 * TEST: Very small ETH quantity (0.001 ETH)
 */
TEST(very_small_eth_quantity) {
    PaperExchangeAdapter adapter;
    Symbol sym_id = adapter.register_symbol("ETHUSDT");

    adapter.set_fill_callback([](
        uint64_t, const char* symbol, Side side,
        double qty, Price, double
    ) {
        received_qty = qty;
        received_symbol = symbol;
        callback_count++;
    });

    double tiny_qty = 0.001;  // 0.001 ETH
    double limit_price = 2900.0;

    adapter.send_limit_order(
        sym_id, Side::Buy, tiny_qty,
        adapter.double_to_price(limit_price)
    );

    // Trigger fill
    adapter.on_price_update(
        sym_id,
        adapter.double_to_price(2895.0),
        adapter.double_to_price(2898.0),
        1000000
    );

    ASSERT_SIZE_EQ(callback_count, 1);
    ASSERT_DOUBLE_GT(received_qty, 0.0);
    ASSERT_DOUBLE_EQ(received_qty, tiny_qty);
}

/**
 * TEST: Typical crypto trade size (0.0115 BTC ≈ $1000 at $87000)
 */
TEST(typical_thousand_dollar_btc_trade) {
    PaperExchangeAdapter adapter;
    Symbol sym_id = adapter.register_symbol("BTCUSDT");

    adapter.set_fill_callback([](
        uint64_t, const char*, Side,
        double qty, Price, double
    ) {
        received_qty = qty;
        callback_count++;
    });

    // $1000 worth of BTC at $87000 = 0.01149... BTC
    double btc_price = 87000.0;
    double usd_value = 1000.0;
    double btc_qty = usd_value / btc_price;  // ~0.011494

    adapter.send_limit_order(
        sym_id, Side::Buy, btc_qty,
        adapter.double_to_price(btc_price)
    );

    // Trigger fill
    adapter.on_price_update(
        sym_id,
        adapter.double_to_price(86900.0),
        adapter.double_to_price(86950.0),
        1000000
    );

    ASSERT_SIZE_EQ(callback_count, 1);
    ASSERT_DOUBLE_GT(received_qty, 0.0);
    ASSERT_DOUBLE_EQ(received_qty, btc_qty);

    // Verify the USD value is approximately correct
    double received_usd_value = received_qty * btc_price;
    ASSERT_TRUE(std::abs(received_usd_value - usd_value) < 1.0);
}

/**
 * TEST: Market order also preserves fractional quantity
 */
TEST(market_order_fractional_quantity) {
    PaperExchangeAdapter adapter;
    Symbol sym_id = adapter.register_symbol("BTCUSDT");

    adapter.set_fill_callback([](
        uint64_t, const char*, Side,
        double qty, Price, double
    ) {
        received_qty = qty;
        callback_count++;
    });

    double fractional_qty = 0.05;  // 0.05 BTC
    Price expected_price = adapter.double_to_price(87000.0);

    adapter.send_market_order(sym_id, Side::Buy, fractional_qty, expected_price);

    ASSERT_SIZE_EQ(callback_count, 1);
    ASSERT_DOUBLE_GT(received_qty, 0.0);
    ASSERT_DOUBLE_EQ(received_qty, fractional_qty);
}

/**
 * TEST: Sell order with fractional quantity
 */
TEST(sell_order_fractional_quantity) {
    PaperExchangeAdapter adapter;
    Symbol sym_id = adapter.register_symbol("BTCUSDT");

    adapter.set_fill_callback([](
        uint64_t, const char*, Side side,
        double qty, Price, double
    ) {
        received_qty = qty;
        received_side = side;
        callback_count++;
    });

    double fractional_qty = 0.02;  // 0.02 BTC

    adapter.send_limit_order(
        sym_id, Side::Sell, fractional_qty,
        adapter.double_to_price(88000.0)  // Sell at 88000
    );

    // Bid rises above limit - fill
    adapter.on_price_update(
        sym_id,
        adapter.double_to_price(88100.0),
        adapter.double_to_price(88150.0),
        1000000
    );

    ASSERT_SIZE_EQ(callback_count, 1);
    ASSERT_EQ(received_side, Side::Sell);
    ASSERT_DOUBLE_GT(received_qty, 0.0);
    ASSERT_DOUBLE_EQ(received_qty, fractional_qty);
}

int main() {
    std::cout << "\n=== Fractional Quantity Test Suite ===\n\n";
    std::cout << "Testing that fractional crypto quantities are preserved\n";
    std::cout << "through the PaperExchangeAdapter callback chain.\n\n";

    RUN_TEST(fractional_btc_quantity_through_callback);
    RUN_TEST(very_small_eth_quantity);
    RUN_TEST(typical_thousand_dollar_btc_trade);
    RUN_TEST(market_order_fractional_quantity);
    RUN_TEST(sell_order_fractional_quantity);

    std::cout << "\n=== All tests PASSED! ===\n\n";
    return 0;
}
