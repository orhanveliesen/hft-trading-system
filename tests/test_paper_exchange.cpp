/**
 * PaperExchange Test Suite
 *
 * Tests the simulated exchange for paper trading:
 * - Market order fills
 * - Limit order pending and fills
 * - Pessimistic fill logic
 * - Commission calculation
 * - Order cancellation
 *
 * Run with: ./test_paper_exchange
 */

#include <iostream>
#include <iomanip>
#include <cassert>
#include <cmath>
#include <string>
#include <memory>
#include "../include/exchange/paper_exchange.hpp"
#include "../include/ipc/shared_config.hpp"
#include "../include/ipc/shared_paper_config.hpp"

using namespace hft;
using namespace hft::exchange;
using namespace hft::ipc;

// Zero-slippage config for predictable test results
// Uses heap allocation with manual init to avoid shared memory overhead
static SharedPaperConfig* create_zero_slippage_config() {
    static SharedPaperConfig config;
    config.init();
    config.set_slippage_bps(0.0);  // Zero slippage for tests
    return &config;
}

// Test state
static ExecutionReport last_report;
static int execution_count = 0;

static void reset_test_state() {
    last_report = ExecutionReport{};
    execution_count = 0;
}

static void on_execution(const ExecutionReport& report) {
    last_report = report;
    execution_count++;
}

// Test helpers
#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    reset_test_state(); \
    std::cout << "  Running " #name "... "; \
    test_##name(); \
    std::cout << "PASSED\n"; \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        std::cerr << "\nFAILED: " << #a << " != " << #b << "\n"; \
        assert(false); \
    } \
} while(0)

#define ASSERT_DOUBLE_EQ(a, b) do { \
    if (std::abs((a) - (b)) > 1e-9) { \
        std::cerr << "\nFAILED: " << #a << " != " << #b << " (" << (a) << " != " << (b) << ")\n"; \
        assert(false); \
    } \
} while(0)

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        std::cerr << "\nFAILED: " << #expr << " is false\n"; \
        assert(false); \
    } \
} while(0)

#define ASSERT_FALSE(expr) do { \
    if (expr) { \
        std::cerr << "\nFAILED: " << #expr << " is true\n"; \
        assert(false); \
    } \
} while(0)

#define ASSERT_STREQ(a, b) do { \
    if (std::string(a) != std::string(b)) { \
        std::cerr << "\nFAILED: " << #a << " != " << #b << " (\"" << (a) << "\" != \"" << (b) << "\")\n"; \
        assert(false); \
    } \
} while(0)

// Tests

TEST(market_buy_fills_at_ask) {
    PaperExchange exchange;
    exchange.set_paper_config(create_zero_slippage_config());
    exchange.set_execution_callback(on_execution);

    auto report = exchange.send_market_order(
        "BTCUSDT", Side::Buy, 1.0,
        50000.0,  // bid
        50010.0,  // ask
        1000000
    );

    ASSERT_EQ(report.exec_type, ExecType::Trade);
    ASSERT_EQ(report.status, OrderStatus::Filled);
    ASSERT_EQ(report.side, Side::Buy);
    ASSERT_DOUBLE_EQ(report.filled_price, 50010.0);  // Filled at ask
    ASSERT_DOUBLE_EQ(report.filled_qty, 1.0);
    ASSERT_EQ(execution_count, 1);
}

TEST(market_sell_fills_at_bid) {
    PaperExchange exchange;
    exchange.set_paper_config(create_zero_slippage_config());
    exchange.set_execution_callback(on_execution);

    auto report = exchange.send_market_order(
        "BTCUSDT", Side::Sell, 2.5,
        50000.0,  // bid
        50010.0,  // ask
        1000000
    );

    ASSERT_EQ(report.exec_type, ExecType::Trade);
    ASSERT_DOUBLE_EQ(report.filled_price, 50000.0);  // Filled at bid
    ASSERT_DOUBLE_EQ(report.filled_qty, 2.5);
}

TEST(market_order_includes_commission) {
    PaperExchange exchange;
    exchange.set_paper_config(create_zero_slippage_config());
    exchange.set_execution_callback(on_execution);

    auto report = exchange.send_market_order(
        "ETHUSDT", Side::Buy, 10.0,
        3000.0,  // bid
        3001.0,  // ask
        1000000
    );

    // Default commission = 0.1% of notional
    double notional = 10.0 * 3001.0;  // qty * ask
    double expected_commission = notional * 0.001;  // 0.1%

    ASSERT_DOUBLE_EQ(report.commission, expected_commission);
}

TEST(limit_order_goes_to_pending) {
    PaperExchange exchange;
    exchange.set_execution_callback(on_execution);

    auto report = exchange.send_limit_order(
        "BTCUSDT", Side::Buy, 1.0,
        49000.0,  // limit price (below current)
        1000000
    );

    ASSERT_EQ(report.exec_type, ExecType::New);
    ASSERT_EQ(report.status, OrderStatus::New);
    ASSERT_EQ(exchange.pending_count(), 1u);
}

TEST(limit_buy_fills_when_ask_drops_below_limit) {
    PaperExchange exchange;
    exchange.set_paper_config(create_zero_slippage_config());
    exchange.set_execution_callback(on_execution);

    // Submit buy limit at 50000
    exchange.send_limit_order(
        "BTCUSDT", Side::Buy, 1.0,
        50000.0,  // limit price
        1000000
    );
    ASSERT_EQ(exchange.pending_count(), 1u);

    // Price update: ask still above limit - no fill
    exchange.on_price_update("BTCUSDT", 50100.0, 50200.0, 1000001);
    ASSERT_EQ(exchange.pending_count(), 1u);
    ASSERT_EQ(execution_count, 1);  // Only the New report

    // Price update: ask drops BELOW limit - fill!
    exchange.on_price_update("BTCUSDT", 49900.0, 49950.0, 1000002);
    ASSERT_EQ(exchange.pending_count(), 0u);
    ASSERT_EQ(execution_count, 2);  // New + Trade

    ASSERT_EQ(last_report.exec_type, ExecType::Trade);
    ASSERT_EQ(last_report.status, OrderStatus::Filled);
    ASSERT_DOUBLE_EQ(last_report.filled_price, 49950.0);  // Filled at current ask
}

TEST(limit_sell_fills_when_bid_rises_above_limit) {
    PaperExchange exchange;
    exchange.set_paper_config(create_zero_slippage_config());
    exchange.set_execution_callback(on_execution);

    // Submit sell limit at 50000
    exchange.send_limit_order(
        "BTCUSDT", Side::Sell, 2.0,
        50000.0,  // limit price
        1000000
    );
    ASSERT_EQ(exchange.pending_count(), 1u);

    // Price update: bid still below limit - no fill
    exchange.on_price_update("BTCUSDT", 49800.0, 49900.0, 1000001);
    ASSERT_EQ(exchange.pending_count(), 1u);

    // Price update: bid rises ABOVE limit - fill!
    exchange.on_price_update("BTCUSDT", 50100.0, 50200.0, 1000002);
    ASSERT_EQ(exchange.pending_count(), 0u);
    ASSERT_EQ(execution_count, 2);

    ASSERT_EQ(last_report.exec_type, ExecType::Trade);
    ASSERT_DOUBLE_EQ(last_report.filled_price, 50100.0);  // Filled at current bid
}

TEST(cancel_pending_order) {
    PaperExchange exchange;
    exchange.set_execution_callback(on_execution);

    auto new_report = exchange.send_limit_order(
        "BTCUSDT", Side::Buy, 1.0,
        49000.0,
        1000000
    );
    ASSERT_EQ(exchange.pending_count(), 1u);

    bool cancelled = exchange.cancel_order(new_report.order_id, 1000001);
    ASSERT_TRUE(cancelled);
    ASSERT_EQ(exchange.pending_count(), 0u);
    ASSERT_EQ(last_report.exec_type, ExecType::Cancelled);
}

TEST(cancel_nonexistent_order_returns_false) {
    PaperExchange exchange;
    exchange.set_execution_callback(on_execution);

    bool cancelled = exchange.cancel_order(99999, 1000000);
    ASSERT_FALSE(cancelled);
}

TEST(multiple_symbols_tracked_separately) {
    PaperExchange exchange;
    exchange.set_paper_config(create_zero_slippage_config());
    exchange.set_execution_callback(on_execution);

    exchange.send_limit_order("BTCUSDT", Side::Buy, 1.0, 50000.0, 1000000);
    exchange.send_limit_order("ETHUSDT", Side::Buy, 10.0, 3000.0, 1000001);
    ASSERT_EQ(exchange.pending_count(), 2u);

    // Only BTCUSDT fills
    exchange.on_price_update("BTCUSDT", 49900.0, 49950.0, 1000002);
    ASSERT_EQ(exchange.pending_count(), 1u);  // ETHUSDT still pending
    ASSERT_STREQ(last_report.symbol, "BTCUSDT");
}

TEST(max_pending_orders_enforced) {
    PaperExchange exchange;
    exchange.set_execution_callback(on_execution);

    // Fill up the pending queue
    for (size_t i = 0; i < PaperExchange::MAX_PENDING_ORDERS; ++i) {
        auto report = exchange.send_limit_order(
            "BTCUSDT", Side::Buy, 1.0,
            40000.0 + i,  // different prices
            1000000 + i
        );
        ASSERT_EQ(report.status, OrderStatus::New);
    }
    ASSERT_EQ(exchange.pending_count(), PaperExchange::MAX_PENDING_ORDERS);

    // Next order should be rejected
    auto report = exchange.send_limit_order(
        "BTCUSDT", Side::Buy, 1.0,
        39000.0,
        9999999
    );
    ASSERT_EQ(report.exec_type, ExecType::Rejected);
    ASSERT_EQ(report.status, OrderStatus::Rejected);
    ASSERT_STREQ(report.reject_reason, "MAX_PENDING_EXCEEDED");
}

TEST(pessimistic_buy_limit_equal_to_ask_no_fill) {
    PaperExchange exchange;
    exchange.set_execution_callback(on_execution);

    // Buy limit at exactly the ask price should NOT fill (pessimistic)
    exchange.send_limit_order("BTCUSDT", Side::Buy, 1.0, 50000.0, 1000000);

    // Price update: ask = limit price (not below) - no fill
    exchange.on_price_update("BTCUSDT", 49950.0, 50000.0, 1000001);
    ASSERT_EQ(exchange.pending_count(), 1u);  // Still pending
}

TEST(pessimistic_sell_limit_equal_to_bid_no_fill) {
    PaperExchange exchange;
    exchange.set_execution_callback(on_execution);

    // Sell limit at exactly the bid price should NOT fill (pessimistic)
    exchange.send_limit_order("BTCUSDT", Side::Sell, 1.0, 50000.0, 1000000);

    // Price update: bid = limit price (not above) - no fill
    exchange.on_price_update("BTCUSDT", 50000.0, 50050.0, 1000001);
    ASSERT_EQ(exchange.pending_count(), 1u);  // Still pending
}

TEST(commission_from_config) {
    PaperExchange exchange;
    exchange.set_paper_config(create_zero_slippage_config());
    exchange.set_execution_callback(on_execution);

    // Create shared config with custom commission rate
    auto* config = SharedConfig::create("/trader_test_config");
    ASSERT_TRUE(config != nullptr);

    // Set commission to 0.05% (5 bps)
    config->set_commission_rate(0.0005);
    exchange.set_config(config);

    auto report = exchange.send_market_order(
        "BTCUSDT", Side::Buy, 1.0,
        50000.0,  // bid
        50010.0,  // ask
        1000000
    );

    // Commission should be 0.05% of notional
    double notional = 1.0 * 50010.0;
    double expected = notional * 0.0005;
    ASSERT_DOUBLE_EQ(report.commission, expected);

    // Cleanup
    munmap(config, sizeof(SharedConfig));
    SharedConfig::destroy("/trader_test_config");
}

int main() {
    std::cout << "\n=== PaperExchange Test Suite ===\n\n";

    RUN_TEST(market_buy_fills_at_ask);
    RUN_TEST(market_sell_fills_at_bid);
    RUN_TEST(market_order_includes_commission);
    RUN_TEST(limit_order_goes_to_pending);
    RUN_TEST(limit_buy_fills_when_ask_drops_below_limit);
    RUN_TEST(limit_sell_fills_when_bid_rises_above_limit);
    RUN_TEST(cancel_pending_order);
    RUN_TEST(cancel_nonexistent_order_returns_false);
    RUN_TEST(multiple_symbols_tracked_separately);
    RUN_TEST(max_pending_orders_enforced);
    RUN_TEST(pessimistic_buy_limit_equal_to_ask_no_fill);
    RUN_TEST(pessimistic_sell_limit_equal_to_bid_no_fill);
    RUN_TEST(commission_from_config);

    std::cout << "\n=== All tests PASSED! ===\n\n";
    return 0;
}
