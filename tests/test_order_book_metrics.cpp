#include "../include/metrics/order_book_metrics.hpp"
#include "../include/orderbook.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace hft;

// Helper to compare doubles with tolerance
bool approx_equal(double a, double b, double tol = 0.01) {
    return std::abs(a - b) < tol;
}

// Helper to create a simple order book
OrderBook create_test_book() {
    OrderBook book(90000, 200000);
    return book;
}

// === Basic Spread Tests (5) ===

void test_spread_calculation() {
    auto book = create_test_book();
    book.add_order(1, Side::Buy, 10000, 100);
    book.add_order(2, Side::Sell, 10005, 100);

    OrderBookMetrics metrics;
    metrics.on_order_book_update(book, 1'000'000);

    auto m = metrics.get_metrics();
    assert(approx_equal(m.spread, 5.0));
    std::cout << "✓ test_spread_calculation\n";
}

void test_spread_bps() {
    auto book = create_test_book();
    book.add_order(1, Side::Buy, 10000, 100);
    book.add_order(2, Side::Sell, 10010, 100);

    OrderBookMetrics metrics;
    metrics.on_order_book_update(book, 1'000'000);

    auto m = metrics.get_metrics();
    // spread = 10, mid = 10005, spread_bps = (10 / 10005) * 10000 = ~9.995
    assert(approx_equal(m.spread_bps, 9.995, 0.01));
    std::cout << "✓ test_spread_bps\n";
}

void test_mid_price() {
    auto book = create_test_book();
    book.add_order(1, Side::Buy, 10000, 100);
    book.add_order(2, Side::Sell, 10020, 100);

    OrderBookMetrics metrics;
    metrics.on_order_book_update(book, 1'000'000);

    auto m = metrics.get_metrics();
    assert(approx_equal(m.mid_price, 10010.0));
    std::cout << "✓ test_mid_price\n";
}

void test_empty_book() {
    auto book = create_test_book();

    OrderBookMetrics metrics;
    metrics.on_order_book_update(book, 1'000'000);

    auto m = metrics.get_metrics();
    assert(m.spread == 0.0);
    assert(m.spread_bps == 0.0);
    assert(m.mid_price == 0.0);
    assert(m.best_bid == INVALID_PRICE);
    assert(m.best_ask == INVALID_PRICE);
    std::cout << "✓ test_empty_book\n";
}

void test_one_sided_book() {
    auto book = create_test_book();
    book.add_order(1, Side::Buy, 10000, 100);
    book.add_order(2, Side::Buy, 9995, 50);

    OrderBookMetrics metrics;
    metrics.on_order_book_update(book, 1'000'000);

    auto m = metrics.get_metrics();
    assert(m.spread == 0.0);
    assert(m.spread_bps == 0.0);
    assert(m.mid_price == 0.0);
    assert(m.best_bid == 10000);
    assert(m.best_ask == INVALID_PRICE);
    std::cout << "✓ test_one_sided_book\n";
}

// === Depth Calculation Tests (6) ===

void test_bid_depth_5bps() {
    auto book = create_test_book();
    // Best bid = 10000, 5 bps = 5 points, so threshold = 9995
    book.add_order(1, Side::Buy, 10000, 100); // Within 5 bps
    book.add_order(2, Side::Buy, 9998, 50);   // Within 5 bps
    book.add_order(3, Side::Buy, 9995, 30);   // Exactly at threshold
    book.add_order(4, Side::Buy, 9994, 20);   // Outside 5 bps

    OrderBookMetrics metrics;
    metrics.on_order_book_update(book, 1'000'000);

    auto m = metrics.get_metrics();
    assert(approx_equal(m.bid_depth_5, 180.0)); // 100 + 50 + 30
    std::cout << "✓ test_bid_depth_5bps\n";
}

void test_bid_depth_10bps() {
    auto book = create_test_book();
    // Best bid = 10000, 10 bps = 10 points, so threshold = 9990
    book.add_order(1, Side::Buy, 10000, 100);
    book.add_order(2, Side::Buy, 9995, 50);
    book.add_order(3, Side::Buy, 9990, 30); // Exactly at threshold
    book.add_order(4, Side::Buy, 9989, 20); // Outside 10 bps

    OrderBookMetrics metrics;
    metrics.on_order_book_update(book, 1'000'000);

    auto m = metrics.get_metrics();
    assert(approx_equal(m.bid_depth_10, 180.0)); // 100 + 50 + 30
    std::cout << "✓ test_bid_depth_10bps\n";
}

void test_bid_depth_20bps() {
    auto book = create_test_book();
    // Best bid = 10000, 20 bps = 20 points, so threshold = 9980
    book.add_order(1, Side::Buy, 10000, 100);
    book.add_order(2, Side::Buy, 9995, 50);
    book.add_order(3, Side::Buy, 9990, 30);
    book.add_order(4, Side::Buy, 9980, 20); // Exactly at threshold
    book.add_order(5, Side::Buy, 9979, 10); // Outside 20 bps

    OrderBookMetrics metrics;
    metrics.on_order_book_update(book, 1'000'000);

    auto m = metrics.get_metrics();
    assert(approx_equal(m.bid_depth_20, 200.0)); // 100 + 50 + 30 + 20
    std::cout << "✓ test_bid_depth_20bps\n";
}

void test_ask_depth_5bps() {
    auto book = create_test_book();
    // Best ask = 10000, 5 bps = 5 points, so threshold = 10005
    book.add_order(1, Side::Sell, 10000, 100); // Within 5 bps
    book.add_order(2, Side::Sell, 10002, 50);  // Within 5 bps
    book.add_order(3, Side::Sell, 10005, 30);  // Exactly at threshold
    book.add_order(4, Side::Sell, 10006, 20);  // Outside 5 bps

    OrderBookMetrics metrics;
    metrics.on_order_book_update(book, 1'000'000);

    auto m = metrics.get_metrics();
    assert(approx_equal(m.ask_depth_5, 180.0)); // 100 + 50 + 30
    std::cout << "✓ test_ask_depth_5bps\n";
}

void test_ask_depth_10bps() {
    auto book = create_test_book();
    // Best ask = 10000, 10 bps = 10 points, so threshold = 10010
    book.add_order(1, Side::Sell, 10000, 100);
    book.add_order(2, Side::Sell, 10005, 50);
    book.add_order(3, Side::Sell, 10010, 30); // Exactly at threshold
    book.add_order(4, Side::Sell, 10011, 20); // Outside 10 bps

    OrderBookMetrics metrics;
    metrics.on_order_book_update(book, 1'000'000);

    auto m = metrics.get_metrics();
    assert(approx_equal(m.ask_depth_10, 180.0)); // 100 + 50 + 30
    std::cout << "✓ test_ask_depth_10bps\n";
}

void test_ask_depth_20bps() {
    auto book = create_test_book();
    // Best ask = 10000, 20 bps = 20 points, so threshold = 10020
    book.add_order(1, Side::Sell, 10000, 100);
    book.add_order(2, Side::Sell, 10005, 50);
    book.add_order(3, Side::Sell, 10010, 30);
    book.add_order(4, Side::Sell, 10020, 20); // Exactly at threshold
    book.add_order(5, Side::Sell, 10021, 10); // Outside 20 bps

    OrderBookMetrics metrics;
    metrics.on_order_book_update(book, 1'000'000);

    auto m = metrics.get_metrics();
    assert(approx_equal(m.ask_depth_20, 200.0)); // 100 + 50 + 30 + 20
    std::cout << "✓ test_ask_depth_20bps\n";
}

// === Imbalance Tests (5) ===

void test_imbalance_5bps() {
    auto book = create_test_book();
    // Bid depth (5 bps): 100 + 50 = 150
    // Ask depth (5 bps): 80
    book.add_order(1, Side::Buy, 10000, 100);
    book.add_order(2, Side::Buy, 9998, 50);
    book.add_order(3, Side::Sell, 10010, 80);

    OrderBookMetrics metrics;
    metrics.on_order_book_update(book, 1'000'000);

    auto m = metrics.get_metrics();
    // imbalance = (150 - 80) / (150 + 80) = 70 / 230 = ~0.304
    assert(approx_equal(m.imbalance_5, 0.304, 0.01));
    std::cout << "✓ test_imbalance_5bps\n";
}

void test_imbalance_10bps() {
    auto book = create_test_book();
    book.add_order(1, Side::Buy, 10000, 100);
    book.add_order(2, Side::Buy, 9995, 50);
    book.add_order(3, Side::Sell, 10010, 120);
    book.add_order(4, Side::Sell, 10015, 30);

    OrderBookMetrics metrics;
    metrics.on_order_book_update(book, 1'000'000);

    auto m = metrics.get_metrics();
    // imbalance = (150 - 150) / (150 + 150) = 0
    assert(approx_equal(m.imbalance_10, 0.0, 0.01));
    std::cout << "✓ test_imbalance_10bps\n";
}

void test_imbalance_20bps() {
    auto book = create_test_book();
    book.add_order(1, Side::Buy, 10000, 50);
    book.add_order(2, Side::Sell, 10010, 100);
    book.add_order(3, Side::Sell, 10015, 100);

    OrderBookMetrics metrics;
    metrics.on_order_book_update(book, 1'000'000);

    auto m = metrics.get_metrics();
    // imbalance = (50 - 200) / (50 + 200) = -150 / 250 = -0.6
    assert(approx_equal(m.imbalance_20, -0.6, 0.01));
    std::cout << "✓ test_imbalance_20bps\n";
}

void test_top_imbalance() {
    auto book = create_test_book();
    book.add_order(1, Side::Buy, 10000, 200);
    book.add_order(2, Side::Sell, 10010, 100);

    OrderBookMetrics metrics;
    metrics.on_order_book_update(book, 1'000'000);

    auto m = metrics.get_metrics();
    // top_imbalance = (200 - 100) / (200 + 100) = 100 / 300 = ~0.333
    assert(approx_equal(m.top_imbalance, 0.333, 0.01));
    std::cout << "✓ test_top_imbalance\n";
}

void test_imbalance_zero_depth() {
    auto book = create_test_book();
    // Empty book - no depth on either side

    OrderBookMetrics metrics;
    metrics.on_order_book_update(book, 1'000'000);

    auto m = metrics.get_metrics();
    assert(m.imbalance_5 == 0.0);
    assert(m.imbalance_10 == 0.0);
    assert(m.imbalance_20 == 0.0);
    assert(m.top_imbalance == 0.0);
    std::cout << "✓ test_imbalance_zero_depth\n";
}

// === Top of Book Tests (4) ===

void test_best_bid_ask() {
    auto book = create_test_book();
    book.add_order(1, Side::Buy, 10000, 100);
    book.add_order(2, Side::Buy, 9995, 50);
    book.add_order(3, Side::Sell, 10010, 80);
    book.add_order(4, Side::Sell, 10015, 30);

    OrderBookMetrics metrics;
    metrics.on_order_book_update(book, 1'000'000);

    auto m = metrics.get_metrics();
    assert(m.best_bid == 10000);
    assert(m.best_ask == 10010);
    std::cout << "✓ test_best_bid_ask\n";
}

void test_best_bid_ask_qty() {
    auto book = create_test_book();
    book.add_order(1, Side::Buy, 10000, 100);
    book.add_order(2, Side::Sell, 10010, 250);

    OrderBookMetrics metrics;
    metrics.on_order_book_update(book, 1'000'000);

    auto m = metrics.get_metrics();
    assert(m.best_bid_qty == 100);
    assert(m.best_ask_qty == 250);
    std::cout << "✓ test_best_bid_ask_qty\n";
}

void test_multiple_levels_at_top() {
    auto book = create_test_book();
    // Multiple orders at the same price
    book.add_order(1, Side::Buy, 10000, 100);
    book.add_order(2, Side::Buy, 10000, 50);
    book.add_order(3, Side::Sell, 10010, 80);
    book.add_order(4, Side::Sell, 10010, 20);

    OrderBookMetrics metrics;
    metrics.on_order_book_update(book, 1'000'000);

    auto m = metrics.get_metrics();
    assert(m.best_bid_qty == 150); // 100 + 50
    assert(m.best_ask_qty == 100); // 80 + 20
    std::cout << "✓ test_multiple_levels_at_top\n";
}

void test_top_of_book_update() {
    auto book = create_test_book();
    book.add_order(1, Side::Buy, 10000, 100);
    book.add_order(2, Side::Sell, 10010, 80);

    OrderBookMetrics metrics;
    metrics.on_order_book_update(book, 1'000'000);

    auto m1 = metrics.get_metrics();
    assert(m1.best_bid == 10000);

    // Cancel best bid
    book.cancel_order(1);
    metrics.on_order_book_update(book, 2'000'000);

    auto m2 = metrics.get_metrics();
    assert(m2.best_bid == INVALID_PRICE);
    assert(m2.best_ask == 10010);
    std::cout << "✓ test_top_of_book_update\n";
}

// === Edge Cases (5) ===

void test_crossed_book() {
    auto book = create_test_book();
    // Crossed book: bid > ask (should not happen in reality, but test handling)
    book.add_order(1, Side::Buy, 10020, 100);
    book.add_order(2, Side::Sell, 10010, 80);

    OrderBookMetrics metrics;
    metrics.on_order_book_update(book, 1'000'000);

    auto m = metrics.get_metrics();
    // spread = 10010 - 10020 = -10 (negative)
    assert(approx_equal(m.spread, -10.0));
    std::cout << "✓ test_crossed_book\n";
}

void test_wide_spread() {
    auto book = create_test_book();
    // Very wide spread (> 100 bps)
    book.add_order(1, Side::Buy, 10000, 100);
    book.add_order(2, Side::Sell, 10200, 80); // 200 point spread = 200 bps

    OrderBookMetrics metrics;
    metrics.on_order_book_update(book, 1'000'000);

    auto m = metrics.get_metrics();
    assert(approx_equal(m.spread, 200.0));
    // spread_bps = (200 / 10100) * 10000 = ~198
    assert(approx_equal(m.spread_bps, 198.0, 1.0));
    std::cout << "✓ test_wide_spread\n";
}

void test_single_level_each_side() {
    auto book = create_test_book();
    book.add_order(1, Side::Buy, 10000, 100);
    book.add_order(2, Side::Sell, 10010, 80);

    OrderBookMetrics metrics;
    metrics.on_order_book_update(book, 1'000'000);

    auto m = metrics.get_metrics();
    // All depth metrics should equal the single level quantity
    assert(approx_equal(m.bid_depth_5, 100.0));
    assert(approx_equal(m.ask_depth_5, 80.0));
    std::cout << "✓ test_single_level_each_side\n";
}

void test_deep_book() {
    auto book = create_test_book();
    // 20+ levels on each side
    for (int i = 0; i < 25; ++i) {
        book.add_order(100 + i, Side::Buy, 10000 - i, 10);
        book.add_order(200 + i, Side::Sell, 10010 + i, 10);
    }

    OrderBookMetrics metrics;
    metrics.on_order_book_update(book, 1'000'000);

    auto m = metrics.get_metrics();
    assert(m.best_bid == 10000);
    assert(m.best_ask == 10010);
    // Depth should include levels within thresholds
    assert(m.bid_depth_20 > 0.0);
    std::cout << "✓ test_deep_book\n";
}

void test_shallow_book() {
    auto book = create_test_book();
    // Less than 5 levels
    book.add_order(1, Side::Buy, 10000, 100);
    book.add_order(2, Side::Buy, 9990, 50);
    book.add_order(3, Side::Sell, 10010, 80);

    OrderBookMetrics metrics;
    metrics.on_order_book_update(book, 1'000'000);

    auto m = metrics.get_metrics();
    // Should use available levels only
    assert(approx_equal(m.bid_depth_10, 150.0)); // Both bid levels within 10 bps
    std::cout << "✓ test_shallow_book\n";
}

int main() {
    std::cout << "Running OrderBookMetrics tests...\n\n";

    // Basic Spread Tests (5)
    test_spread_calculation();
    test_spread_bps();
    test_mid_price();
    test_empty_book();
    test_one_sided_book();

    // Depth Calculation Tests (6)
    test_bid_depth_5bps();
    test_bid_depth_10bps();
    test_bid_depth_20bps();
    test_ask_depth_5bps();
    test_ask_depth_10bps();
    test_ask_depth_20bps();

    // Imbalance Tests (5)
    test_imbalance_5bps();
    test_imbalance_10bps();
    test_imbalance_20bps();
    test_top_imbalance();
    test_imbalance_zero_depth();

    // Top of Book Tests (4)
    test_best_bid_ask();
    test_best_bid_ask_qty();
    test_multiple_levels_at_top();
    test_top_of_book_update();

    // Edge Cases (5)
    test_crossed_book();
    test_wide_spread();
    test_single_level_each_side();
    test_deep_book();
    test_shallow_book();

    std::cout << "\n✅ All 25 OrderBookMetrics tests passed!\n";
    return 0;
}
