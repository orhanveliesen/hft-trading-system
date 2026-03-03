#include "../include/metrics/combined_metrics.hpp"
#include "../include/metrics/order_book_metrics.hpp"
#include "../include/metrics/trade_stream_metrics.hpp"
#include "../include/orderbook.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace hft;

// Helper to compare doubles with tolerance
bool approx_equal(double a, double b, double tol = 0.01) {
    return std::abs(a - b) < tol;
}

// Helper to create order book with bid/ask
OrderBook create_book_with_spread(Price bid, Quantity bid_qty, Price ask, Quantity ask_qty) {
    OrderBook book(9000, 200000);
    book.add_order(1, Side::Buy, bid, bid_qty);
    book.add_order(2, Side::Sell, ask, ask_qty);
    return book;
}

// === Trade vs Book Tests (2) ===

void test_trade_to_depth_low() {
    // Setup: Low trade volume, high depth → small ratio
    TradeStreamMetrics trade_metrics;
    OrderBookMetrics book_metrics;

    // Add small trades
    trade_metrics.on_trade(10000, 10, true, 1'000'000);
    trade_metrics.on_trade(10000, 10, false, 1'100'000);
    // Total volume = 20

    // Deep book
    auto book = create_book_with_spread(10000, 1000, 10005, 1000);
    book_metrics.on_order_book_update(book, 1'200'000);
    // Total depth = 2000

    CombinedMetrics combined(trade_metrics, book_metrics);
    combined.update(1'200'000);

    auto m = combined.get_metrics(CombinedMetrics::Window::SEC_1);
    // Ratio = 20 / 2000 = 0.01
    assert(approx_equal(m.trade_to_depth_ratio, 0.01, 0.001));
    std::cout << "✓ test_trade_to_depth_low\n";
}

void test_trade_to_depth_high() {
    // Setup: High trade volume, thin depth → large ratio
    TradeStreamMetrics trade_metrics;
    OrderBookMetrics book_metrics;

    // Add large trades
    trade_metrics.on_trade(10000, 500, true, 1'000'000);
    trade_metrics.on_trade(10000, 500, false, 1'100'000);
    // Total volume = 1000

    // Thin book
    auto book = create_book_with_spread(10000, 50, 10005, 50);
    book_metrics.on_order_book_update(book, 1'200'000);
    // Total depth = 100

    CombinedMetrics combined(trade_metrics, book_metrics);
    combined.update(1'200'000);

    auto m = combined.get_metrics(CombinedMetrics::Window::SEC_1);
    // Ratio = 1000 / 100 = 10.0
    assert(approx_equal(m.trade_to_depth_ratio, 10.0, 0.1));
    std::cout << "✓ test_trade_to_depth_high\n";
}

// === Absorption Tests (5) ===

void test_absorption_bid_strong() {
    // Setup: Heavy sells at best bid, bid holds → ratio > 1
    TradeStreamMetrics trade_metrics;
    OrderBookMetrics book_metrics;

    // Initial book state: bid=10000 qty=1000
    auto book = create_book_with_spread(10000, 1000, 10005, 500);
    book_metrics.on_order_book_update(book, 1'000'000);

    CombinedMetrics combined(trade_metrics, book_metrics);
    combined.update(1'000'000);

    // Large sell trades at best bid (aggressive sells hitting the bid)
    trade_metrics.on_trade(10000, 500, false, 1'050'000);
    trade_metrics.on_trade(10000, 300, false, 1'060'000);
    // Total sell volume at bid = 800

    // Book update: bid still at 10000 but quantity reduced to 300 (absorbed 700, but 800 traded)
    // This means hidden orders absorbed 100 qty (ratio = 800/700 > 1)
    book.add_order(3, Side::Buy, 10000, 300); // Simulate replenishment
    book.cancel_order(1);                     // Remove old bid
    book_metrics.on_order_book_update(book, 1'100'000);

    combined.update(1'100'000);

    auto m = combined.get_metrics(CombinedMetrics::Window::SEC_1);
    // Absorption ratio = sell_volume / bid_qty_decrease
    // sell_volume = 800, bid_qty_decrease = 1000 - 300 = 700
    // ratio = 800 / 700 ≈ 1.14 (> 1 indicates absorption)
    assert(m.absorption_ratio_bid > 1.0);
    std::cout << "✓ test_absorption_bid_strong\n";
}

void test_absorption_bid_weak() {
    // Setup: Sells at best bid, bid breaks → ratio < 1
    TradeStreamMetrics trade_metrics;
    OrderBookMetrics book_metrics;

    // Initial book state
    auto book = create_book_with_spread(10000, 500, 10005, 500);
    book_metrics.on_order_book_update(book, 1'000'000);

    CombinedMetrics combined(trade_metrics, book_metrics);
    combined.update(1'000'000);

    // Small sell trades
    trade_metrics.on_trade(10000, 200, false, 1'050'000);

    // Book update: bid qty decreased by more than traded (weak absorption)
    book.cancel_order(1);
    book.add_order(3, Side::Buy, 10000, 200); // Qty decreased by 300
    book_metrics.on_order_book_update(book, 1'100'000);

    combined.update(1'100'000);

    auto m = combined.get_metrics(CombinedMetrics::Window::SEC_1);
    // sell_volume = 200, bid_qty_decrease = 500 - 200 = 300
    // ratio = 200 / 300 ≈ 0.67 (< 1 indicates weak absorption)
    assert(m.absorption_ratio_bid < 1.0);
    std::cout << "✓ test_absorption_bid_weak\n";
}

void test_absorption_ask_strong() {
    // Setup: Heavy buys at best ask, ask holds → ratio > 1
    TradeStreamMetrics trade_metrics;
    OrderBookMetrics book_metrics;

    // Initial book state
    auto book = create_book_with_spread(10000, 500, 10005, 1000);
    book_metrics.on_order_book_update(book, 1'000'000);

    CombinedMetrics combined(trade_metrics, book_metrics);
    combined.update(1'000'000);

    // Large buy trades at best ask
    trade_metrics.on_trade(10005, 500, true, 1'050'000);
    trade_metrics.on_trade(10005, 300, true, 1'060'000);
    // Total buy volume at ask = 800

    // Book update: ask still at 10005 but quantity reduced to 300
    book.cancel_order(2);
    book.add_order(3, Side::Sell, 10005, 300);
    book_metrics.on_order_book_update(book, 1'100'000);

    combined.update(1'100'000);

    auto m = combined.get_metrics(CombinedMetrics::Window::SEC_1);
    // buy_volume = 800, ask_qty_decrease = 1000 - 300 = 700
    // ratio = 800 / 700 ≈ 1.14
    assert(m.absorption_ratio_ask > 1.0);
    std::cout << "✓ test_absorption_ask_strong\n";
}

void test_absorption_ask_weak() {
    // Setup: Buys at best ask, ask breaks → ratio < 1
    TradeStreamMetrics trade_metrics;
    OrderBookMetrics book_metrics;

    // Initial book state
    auto book = create_book_with_spread(10000, 500, 10005, 500);
    book_metrics.on_order_book_update(book, 1'000'000);

    CombinedMetrics combined(trade_metrics, book_metrics);
    combined.update(1'000'000);

    // Small buy trades
    trade_metrics.on_trade(10005, 200, true, 1'050'000);

    // Book update: ask qty decreased by more than traded
    book.cancel_order(2);
    book.add_order(3, Side::Sell, 10005, 200);
    book_metrics.on_order_book_update(book, 1'100'000);

    combined.update(1'100'000);

    auto m = combined.get_metrics(CombinedMetrics::Window::SEC_1);
    // buy_volume = 200, ask_qty_decrease = 500 - 200 = 300
    // ratio = 200 / 300 ≈ 0.67
    assert(m.absorption_ratio_ask < 1.0);
    std::cout << "✓ test_absorption_ask_weak\n";
}

void test_absorption_no_activity() {
    // Setup: No trades at best bid/ask → ratio 0
    TradeStreamMetrics trade_metrics;
    OrderBookMetrics book_metrics;

    auto book = create_book_with_spread(10000, 500, 10005, 500);
    book_metrics.on_order_book_update(book, 1'000'000);

    CombinedMetrics combined(trade_metrics, book_metrics);
    combined.update(1'000'000);

    // No trades
    auto m = combined.get_metrics(CombinedMetrics::Window::SEC_1);
    assert(approx_equal(m.absorption_ratio_bid, 0.0, 0.001));
    assert(approx_equal(m.absorption_ratio_ask, 0.0, 0.001));
    std::cout << "✓ test_absorption_no_activity\n";
}

// === Spread Dynamics Tests (4) ===

void test_spread_mean() {
    // Setup: Known spread values, verify average
    TradeStreamMetrics trade_metrics;
    OrderBookMetrics book_metrics;

    // Create combined metrics
    CombinedMetrics combined(trade_metrics, book_metrics);

    // Add spreads: 5, 10, 15 (mean = 10)
    auto book1 = create_book_with_spread(10000, 100, 10005, 100);
    book_metrics.on_order_book_update(book1, 1'000'000);
    combined.update(1'000'000);

    auto book2 = create_book_with_spread(10000, 100, 10010, 100);
    book_metrics.on_order_book_update(book2, 1'100'000);
    combined.update(1'100'000);

    auto book3 = create_book_with_spread(10000, 100, 10015, 100);
    book_metrics.on_order_book_update(book3, 1'200'000);
    combined.update(1'200'000);

    auto m = combined.get_metrics(CombinedMetrics::Window::SEC_1);
    assert(approx_equal(m.spread_mean, 10.0, 0.1));
    std::cout << "✓ test_spread_mean\n";
}

void test_spread_max_min() {
    // Setup: Verify extremes
    TradeStreamMetrics trade_metrics;
    OrderBookMetrics book_metrics;

    CombinedMetrics combined(trade_metrics, book_metrics);

    // Add spreads: 5, 20, 10 (min=5, max=20)
    auto book1 = create_book_with_spread(10000, 100, 10005, 100);
    book_metrics.on_order_book_update(book1, 1'000'000);
    combined.update(1'000'000);

    auto book2 = create_book_with_spread(10000, 100, 10020, 100);
    book_metrics.on_order_book_update(book2, 1'100'000);
    combined.update(1'100'000);

    auto book3 = create_book_with_spread(10000, 100, 10010, 100);
    book_metrics.on_order_book_update(book3, 1'200'000);
    combined.update(1'200'000);

    auto m = combined.get_metrics(CombinedMetrics::Window::SEC_1);
    assert(approx_equal(m.spread_max, 20.0, 0.1));
    assert(approx_equal(m.spread_min, 5.0, 0.1));
    std::cout << "✓ test_spread_max_min\n";
}

void test_spread_volatility() {
    // Setup: Varying spread, verify std dev
    TradeStreamMetrics trade_metrics;
    OrderBookMetrics book_metrics;

    CombinedMetrics combined(trade_metrics, book_metrics);

    // Add spreads with variation: 5, 15, 10 (mean=10, variance=16.67, std=4.08)
    auto book1 = create_book_with_spread(10000, 100, 10005, 100);
    book_metrics.on_order_book_update(book1, 1'000'000);
    combined.update(1'000'000);

    auto book2 = create_book_with_spread(10000, 100, 10015, 100);
    book_metrics.on_order_book_update(book2, 1'100'000);
    combined.update(1'100'000);

    auto book3 = create_book_with_spread(10000, 100, 10010, 100);
    book_metrics.on_order_book_update(book3, 1'200'000);
    combined.update(1'200'000);

    auto m = combined.get_metrics(CombinedMetrics::Window::SEC_1);
    // Values: 5, 15, 10 → mean=10, variance=25, std=5.0
    assert(approx_equal(m.spread_volatility, 5.0, 0.1));
    std::cout << "✓ test_spread_volatility\n";
}

void test_spread_stable() {
    // Setup: Constant spread, volatility ≈ 0
    TradeStreamMetrics trade_metrics;
    OrderBookMetrics book_metrics;

    CombinedMetrics combined(trade_metrics, book_metrics);

    // Add constant spread: 10, 10, 10
    auto book1 = create_book_with_spread(10000, 100, 10010, 100);
    book_metrics.on_order_book_update(book1, 1'000'000);
    combined.update(1'000'000);

    auto book2 = create_book_with_spread(10000, 100, 10010, 100);
    book_metrics.on_order_book_update(book2, 1'100'000);
    combined.update(1'100'000);

    auto book3 = create_book_with_spread(10000, 100, 10010, 100);
    book_metrics.on_order_book_update(book3, 1'200'000);
    combined.update(1'200'000);

    auto m = combined.get_metrics(CombinedMetrics::Window::SEC_1);
    assert(approx_equal(m.spread_volatility, 0.0, 0.01));
    std::cout << "✓ test_spread_stable\n";
}

// === Window Behavior Tests (2) ===

void test_window_expiry() {
    // Setup: Old spread values drop out
    TradeStreamMetrics trade_metrics;
    OrderBookMetrics book_metrics;

    CombinedMetrics combined(trade_metrics, book_metrics);

    // Add spread at t=0
    auto book1 = create_book_with_spread(10000, 100, 10020, 100);
    book_metrics.on_order_book_update(book1, 0);
    combined.update(0);

    // Add spread at t=2s (outside 1s window)
    auto book2 = create_book_with_spread(10000, 100, 10010, 100);
    book_metrics.on_order_book_update(book2, 2'000'000);
    combined.update(2'000'000);

    // Query 1s window (should only see spread=10)
    auto m = combined.get_metrics(CombinedMetrics::Window::SEC_1);
    assert(approx_equal(m.spread_mean, 10.0, 0.1));

    // Query 5s window (should see both spreads, mean=15)
    auto m5 = combined.get_metrics(CombinedMetrics::Window::SEC_5);
    assert(approx_equal(m5.spread_mean, 15.0, 0.1));

    std::cout << "✓ test_window_expiry\n";
}

void test_multiple_windows() {
    // Setup: Different windows different stats
    TradeStreamMetrics trade_metrics;
    OrderBookMetrics book_metrics;

    CombinedMetrics combined(trade_metrics, book_metrics);

    // Add spreads over time
    auto book1 = create_book_with_spread(10000, 100, 10005, 100); // t=0s, spread=5
    book_metrics.on_order_book_update(book1, 0);
    combined.update(0);

    auto book2 = create_book_with_spread(10000, 100, 10015, 100); // t=3s, spread=15
    book_metrics.on_order_book_update(book2, 3'000'000);
    combined.update(3'000'000);

    auto book3 = create_book_with_spread(10000, 100, 10010, 100); // t=7s, spread=10
    book_metrics.on_order_book_update(book3, 7'000'000);
    combined.update(7'000'000);

    // 1s window: only spread=10
    auto m1 = combined.get_metrics(CombinedMetrics::Window::SEC_1);
    assert(approx_equal(m1.spread_mean, 10.0, 0.1));

    // 5s window: spread=15 and spread=10, mean=12.5
    auto m5 = combined.get_metrics(CombinedMetrics::Window::SEC_5);
    assert(approx_equal(m5.spread_mean, 12.5, 0.1));

    // 10s window: all spreads, mean=10
    auto m10 = combined.get_metrics(CombinedMetrics::Window::SEC_10);
    assert(approx_equal(m10.spread_mean, 10.0, 0.1));

    std::cout << "✓ test_multiple_windows\n";
}

// === Edge Cases Tests (2) ===

void test_empty_state() {
    // Setup: No data, all zero
    TradeStreamMetrics trade_metrics;
    OrderBookMetrics book_metrics;

    CombinedMetrics combined(trade_metrics, book_metrics);

    auto m = combined.get_metrics(CombinedMetrics::Window::SEC_1);
    assert(approx_equal(m.trade_to_depth_ratio, 0.0, 0.001));
    assert(approx_equal(m.absorption_ratio_bid, 0.0, 0.001));
    assert(approx_equal(m.absorption_ratio_ask, 0.0, 0.001));
    assert(approx_equal(m.spread_mean, 0.0, 0.001));
    assert(approx_equal(m.spread_max, 0.0, 0.001));
    assert(approx_equal(m.spread_min, 0.0, 0.001));
    assert(approx_equal(m.spread_volatility, 0.0, 0.001));
    std::cout << "✓ test_empty_state\n";
}

void test_reset() {
    // Setup: Verify clean state
    TradeStreamMetrics trade_metrics;
    OrderBookMetrics book_metrics;

    CombinedMetrics combined(trade_metrics, book_metrics);

    // Add some data
    auto book = create_book_with_spread(10000, 100, 10010, 100);
    book_metrics.on_order_book_update(book, 1'000'000);
    combined.update(1'000'000);

    // Reset
    combined.reset();

    auto m = combined.get_metrics(CombinedMetrics::Window::SEC_1);
    assert(approx_equal(m.trade_to_depth_ratio, 0.0, 0.001));
    assert(approx_equal(m.spread_mean, 0.0, 0.001));
    std::cout << "✓ test_reset\n";
}

// === Performance Tests (2) - Placeholder (not run in CI) ===

int main() {
    // Trade vs Book (2)
    test_trade_to_depth_low();
    test_trade_to_depth_high();

    // Absorption (5)
    test_absorption_bid_strong();
    test_absorption_bid_weak();
    test_absorption_ask_strong();
    test_absorption_ask_weak();
    test_absorption_no_activity();

    // Spread Dynamics (4)
    test_spread_mean();
    test_spread_max_min();
    test_spread_volatility();
    test_spread_stable();

    // Window Behavior (2)
    test_window_expiry();
    test_multiple_windows();

    // Edge Cases (2)
    test_empty_state();
    test_reset();

    std::cout << "\n✅ All 15 tests passed!\n";
    return 0;
}
