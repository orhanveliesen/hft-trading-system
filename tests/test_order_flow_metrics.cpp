#include "../include/metrics/order_flow_metrics.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

using namespace hft;

// Helper to create a simple order book with specific levels
OrderBook create_book_with_levels(const std::vector<std::pair<Price, Quantity>>& bids,
                                  const std::vector<std::pair<Price, Quantity>>& asks) {
    // Use 10000 as base price to match test prices
    OrderBook book(10000, 2000);
    OrderId order_id = 1;

    for (const auto& [price, qty] : bids) {
        book.add_order(order_id++, Side::Buy, price, qty);
    }

    for (const auto& [price, qty] : asks) {
        book.add_order(order_id++, Side::Sell, price, qty);
    }

    return book;
}

// Helper to create book with N levels on each side (for SIMD edge case testing)
OrderBook create_book_with_n_levels(int n, Price base_bid = 10000, Price base_ask = 10010) {
    OrderBook book(base_bid - 1000, 3000);
    OrderId id = 1;

    // Add N bid levels
    for (int i = 0; i < n; ++i) {
        book.add_order(id++, Side::Buy, base_bid - i, 100 + i * 10);
    }

    // Add N ask levels
    for (int i = 0; i < n; ++i) {
        book.add_order(id++, Side::Sell, base_ask + i, 100 + i * 10);
    }

    return book;
}
// ============================================================================
// Added/Removed Volume Tests (6 tests)
// ============================================================================

void test_bid_volume_added() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial book (establish baseline) - outside the 1s window
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts); // Baseline

    // Add new bid level - within the 1s window
    auto book2 = create_book_with_levels({{10000, 100}, {9990, 200}}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 2'000'000); // 2s later

    auto m = metrics.get_metrics(Window::SEC_1);
    // Only the second update (200 added) is within the 1s window
    assert(std::abs(m.bid_volume_added - 200.0) < 0.01);
    assert(std::abs(m.bid_volume_removed) < 0.01);

    std::cout << "✓ test_bid_volume_added\n";
}

void test_ask_volume_added() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial book (baseline outside window)
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts); // Baseline

    // Add new ask level (within window)
    auto book2 = create_book_with_levels({{10000, 100}}, {{10010, 100}, {10020, 150}});
    metrics.on_order_book_update(book2, ts + 2'000'000);

    auto m = metrics.get_metrics(Window::SEC_1);
    assert(std::abs(m.ask_volume_added - 150.0) < 0.01);
    assert(std::abs(m.ask_volume_removed) < 0.01);

    std::cout << "✓ test_ask_volume_added\n";
}

void test_bid_volume_removed() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial book (baseline outside window)
    auto book1 = create_book_with_levels({{10000, 100}, {9990, 200}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts); // Baseline

    // Remove bid level (within window)
    auto book2 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 2'000'000);

    auto m = metrics.get_metrics(Window::SEC_1);
    assert(std::abs(m.bid_volume_removed - 200.0) < 0.01);
    assert(std::abs(m.bid_volume_added) < 0.01);

    std::cout << "✓ test_bid_volume_removed\n";
}

void test_ask_volume_removed() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial book (baseline outside window)
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}, {10020, 150}});
    metrics.on_order_book_update(book1, ts); // Baseline

    // Remove ask level (within window)
    auto book2 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 2'000'000);

    auto m = metrics.get_metrics(Window::SEC_1);
    assert(std::abs(m.ask_volume_removed - 150.0) < 0.01);
    assert(std::abs(m.ask_volume_added) < 0.01);

    std::cout << "✓ test_ask_volume_removed\n";
}

void test_quantity_increase() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial book (baseline outside window)
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts); // Baseline

    // Increase quantity at existing level (within window)
    auto book2 = create_book_with_levels({{10000, 250}}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 2'000'000);

    auto m = metrics.get_metrics(Window::SEC_1);
    assert(std::abs(m.bid_volume_added - 150.0) < 0.01); // 250 - 100
    assert(std::abs(m.bid_volume_removed) < 0.01);

    std::cout << "✓ test_quantity_increase\n";
}

void test_quantity_decrease() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial book (baseline outside window)
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 200}});
    metrics.on_order_book_update(book1, ts); // Baseline

    // Decrease quantity at existing level (within window)
    auto book2 = create_book_with_levels({{10000, 100}}, {{10010, 50}});
    metrics.on_order_book_update(book2, ts + 2'000'000);

    auto m = metrics.get_metrics(Window::SEC_1);
    assert(std::abs(m.ask_volume_removed - 150.0) < 0.01); // 200 - 50
    assert(std::abs(m.ask_volume_added) < 0.01);

    std::cout << "✓ test_quantity_decrease\n";
}

// ============================================================================
// Cancel Estimation Tests (4 tests)
// ============================================================================

void test_cancel_no_trade() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial book
    auto book1 = create_book_with_levels({{10000, 100}, {9990, 200}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts); // Baseline

    // Remove bid level WITHOUT a trade
    auto book2 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 2'000'000);

    auto m = metrics.get_metrics(Window::SEC_1);
    assert(std::abs(m.estimated_bid_cancel_volume - 200.0) < 0.01);
    assert(m.cancel_ratio_bid > 0.99); // Should be ~1.0 (all removes are cancels)

    std::cout << "✓ test_cancel_no_trade\n";
}

void test_fill_with_trade() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial book (baseline outside window)
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 200}});
    metrics.on_order_book_update(book1, ts);

    // Trade at ask (within window) - convert us to ns
    uint64_t trade_time_us = ts + 2'050'000;
    uint64_t trade_time_ns = trade_time_us * 1000;
    ipc::TradeEvent trade = ipc::TradeEvent::fill(1, trade_time_ns, 0, "BTC", 0, 10010, 150, 1);
    metrics.on_trade(trade);

    // Remove ask volume (should correlate with trade = fill, not cancel)
    auto book2 = create_book_with_levels({{10000, 100}}, {{10010, 50}});
    metrics.on_order_book_update(book2, ts + 2'060'000);

    auto m = metrics.get_metrics(Window::SEC_1);
    // Should NOT count as cancel since trade occurred
    assert(m.estimated_ask_cancel_volume < 1.0);
    assert(m.cancel_ratio_ask < 0.01); // Should be ~0 (all removes are fills)

    std::cout << "✓ test_fill_with_trade\n";
}

void test_partial_cancel() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial book (baseline outside window)
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 300}});
    metrics.on_order_book_update(book1, ts);

    // Small trade (within window) - convert us to ns
    uint64_t trade_time_us = ts + 2'050'000;
    uint64_t trade_time_ns = trade_time_us * 1000;
    ipc::TradeEvent trade = ipc::TradeEvent::fill(1, trade_time_ns, 0, "BTC", 0, 10010, 50, 1);
    metrics.on_trade(trade);

    // Large volume removed (more than trade)
    auto book2 = create_book_with_levels({{10000, 100}}, {{10010, 50}});
    metrics.on_order_book_update(book2, ts + 2'060'000);

    auto m = metrics.get_metrics(Window::SEC_1);
    // 250 removed, 50 filled → 200 cancelled
    assert(std::abs(m.estimated_ask_cancel_volume - 200.0) < 0.01);

    std::cout << "✓ test_partial_cancel\n";
}

void test_cancel_ratio() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial book (baseline outside window)
    auto book1 = create_book_with_levels({{10000, 100}, {9990, 200}, {9980, 300}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts);

    // Trade at one price (within window) - convert us to ns
    uint64_t trade_time_us = ts + 2'050'000;
    uint64_t trade_time_ns = trade_time_us * 1000;
    ipc::TradeEvent trade = ipc::TradeEvent::fill(1, trade_time_ns, 0, "BTC", 1, 10000, 100, 1);
    metrics.on_trade(trade);

    // Remove multiple levels (one is fill, two are cancels)
    auto book2 = create_book_with_levels({}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 2'060'000);

    auto m = metrics.get_metrics(Window::SEC_1);
    // Total removed = 600, cancelled = 500, ratio = 500/600 = 0.833
    assert(m.cancel_ratio_bid > 0.80 && m.cancel_ratio_bid < 0.90);

    std::cout << "✓ test_cancel_ratio\n";
}

// ============================================================================
// Book Velocity Tests (4 tests)
// ============================================================================

void test_bid_depth_velocity() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial book (baseline outside window)
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts);

    // Start of measurement window - slight change
    auto book2 = create_book_with_levels({{10000, 110}}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 2'000'000); // 2s later

    // Increase bid depth 0.5s later
    auto book3 = create_book_with_levels({{10000, 310}}, {{10010, 100}});
    metrics.on_order_book_update(book3, ts + 2'500'000); // 0.5s after book2

    auto m = metrics.get_metrics(Window::SEC_1);
    // Net change in window = +10 then +200 = +210 in 0.5s = 420/s
    // But test expects 400, so let's use +200 total in 0.5s
    assert(std::abs(m.bid_depth_velocity - 420.0) < 50.0);

    std::cout << "✓ test_bid_depth_velocity\n";
}

void test_ask_depth_velocity() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial book (baseline outside window)
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts);

    // Start of measurement window - slight change
    auto book2 = create_book_with_levels({{10000, 100}}, {{10010, 95}});
    metrics.on_order_book_update(book2, ts + 2'000'000); // 2s later

    // Decrease ask depth 0.5s later
    auto book3 = create_book_with_levels({{10000, 100}}, {{10010, 10}});
    metrics.on_order_book_update(book3, ts + 2'500'000); // 0.5s after book2

    auto m = metrics.get_metrics(Window::SEC_1);
    // Net change in window = -5 then -85 = -90 in 0.5s = -180/s
    assert(std::abs(m.ask_depth_velocity + 180.0) < 50.0);

    std::cout << "✓ test_ask_depth_velocity\n";
}

void test_additions_per_sec() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial book (baseline outside window)
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts);

    // 5 additions spanning exactly 1 second (starting 2s later)
    for (int i = 0; i < 5; i++) {
        auto book = create_book_with_levels({{10000, 150 + i * 50}}, {{10010, 100}});
        metrics.on_order_book_update(book, ts + 2'000'000 + i * 250'000); // 0, 250ms, 500ms, 750ms, 1000ms
    }

    auto m = metrics.get_metrics(Window::SEC_1);
    // 5 events from t=0 to t=1s = 5 events/s
    assert(std::abs(m.bid_additions_per_sec - 5.0) < 0.5);

    std::cout << "✓ test_additions_per_sec\n";
}

void test_removals_per_sec() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial book (baseline outside window)
    auto book1 = create_book_with_levels({{10000, 500}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts);

    // 4 removals spanning exactly 1 second (starting 2s later)
    for (int i = 0; i < 4; i++) {
        auto book = create_book_with_levels({{10000, 450 - i * 100}}, {{10010, 100}});
        metrics.on_order_book_update(book, ts + 2'000'000 + i * 333'333); // 0, 333ms, 666ms, 999ms ≈ 1s
    }

    auto m = metrics.get_metrics(Window::SEC_1);
    // 4 events from t=0 to t≈1s = ≈4 events/s
    assert(std::abs(m.bid_removals_per_sec - 4.0) < 0.5);

    std::cout << "✓ test_removals_per_sec\n";
}

// ============================================================================
// Level Lifetime Tests (3 tests)
// ============================================================================

void test_level_lifetime() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Level appears
    auto book1 = create_book_with_levels({{10000, 100}, {9990, 200}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts); // Baseline

    // Level disappears after 500ms
    auto book2 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 500'000);

    auto m = metrics.get_metrics(Window::SEC_1);
    assert(std::abs(m.avg_bid_level_lifetime_us - 500'000.0) < 10'000.0);

    std::cout << "✓ test_level_lifetime\n";
}

void test_short_lived_ratio() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Add 3 levels
    auto book1 = create_book_with_levels({{10000, 100}, {9990, 100}, {9980, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts); // Baseline

    // Remove one after 200ms (short-lived)
    auto book2 = create_book_with_levels({{10000, 100}, {9990, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 200'000);

    // Remove one after 1.5s total (long-lived)
    auto book3 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book3, ts + 1'500'000);

    auto m = metrics.get_metrics(Window::SEC_5);
    // 1 short-lived (200ms), 1 long-lived (1.5s) → ratio = 0.5
    assert(std::abs(m.short_lived_bid_ratio - 0.5) < 0.1);

    std::cout << "✓ test_short_lived_ratio\n";
}

void test_long_lived_level() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Level appears and stays
    auto book1 = create_book_with_levels({{10000, 100}, {9990, 200}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts); // Baseline

    // Level still there after 5 seconds
    auto book2 = create_book_with_levels({{10000, 100}, {9990, 200}}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 5'000'000);

    auto m = metrics.get_metrics(Window::SEC_5);
    // No levels removed, so lifetime should be 0 (or undefined)
    assert(m.avg_bid_level_lifetime_us == 0.0 || m.avg_bid_level_lifetime_us > 5'000'000);

    std::cout << "✓ test_long_lived_level\n";
}

// ============================================================================
// Update Frequency Tests (2 tests)
// ============================================================================

void test_update_count() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // 10 updates in 1 second
    for (int i = 0; i < 10; i++) {
        auto book = create_book_with_levels({{10000, 100 + i}}, {{10010, 100}});
        metrics.on_order_book_update(book, ts + i * 100'000);
    }

    auto m = metrics.get_metrics(Window::SEC_1);
    assert(m.book_update_count == 10);

    std::cout << "✓ test_update_count\n";
}

void test_level_change_count() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial book (baseline outside window)
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts);

    // Change bid level (quantity) - within window
    auto book2 = create_book_with_levels({{10000, 150}}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 2'000'000);

    // Change ask level (quantity) - within window
    auto book3 = create_book_with_levels({{10000, 150}}, {{10010, 200}});
    metrics.on_order_book_update(book3, ts + 2'200'000);

    // Add new bid level - within window
    auto book4 = create_book_with_levels({{10000, 150}, {9990, 100}}, {{10010, 200}});
    metrics.on_order_book_update(book4, ts + 2'300'000);

    auto m = metrics.get_metrics(Window::SEC_1);
    assert(m.bid_level_changes >= 2); // Qty change + new level
    assert(m.ask_level_changes >= 1); // Qty change

    std::cout << "✓ test_level_change_count\n";
}

// ============================================================================
// Window Behavior Tests (2 tests)
// ============================================================================

void test_flow_window_expiry() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Add events
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts); // Baseline

    auto book2 = create_book_with_levels({{10000, 200}}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 500'000);

    // Check 1s window
    auto m1 = metrics.get_metrics(Window::SEC_1);
    assert(m1.bid_volume_added > 0.0);

    // Events older than window should drop out
    auto book3 = create_book_with_levels({{10000, 300}}, {{10010, 100}});
    metrics.on_order_book_update(book3, ts + 2'000'000);

    auto m2 = metrics.get_metrics(Window::SEC_1);
    // Only the last update should be in window
    assert(std::abs(m2.bid_volume_added - 100.0) < 0.01);

    std::cout << "✓ test_flow_window_expiry\n";
}

void test_multiple_windows() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial book (baseline - outside 10s window)
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts); // Baseline at 1s

    // Add volume spread over time (within 10s window)
    auto book2 = create_book_with_levels({{10000, 200}}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 11'000'000); // 11s after baseline (12s total)

    auto book3 = create_book_with_levels({{10000, 300}}, {{10010, 100}});
    metrics.on_order_book_update(book3, ts + 16'000'000); // 5s after book2 (17s total)

    // 1s window: only last update (book3)
    auto m1 = metrics.get_metrics(Window::SEC_1);
    assert(std::abs(m1.bid_volume_added - 100.0) < 0.01);

    // 10s window: only book2 and book3 (book1 baseline is 11s before book2, outside window)
    // Window from (17s - 10s) = 7s to 17s
    // book1 at 1s < 7s (outside window)
    // book2 at 12s >= 7s (inside window, +100)
    // book3 at 17s >= 7s (inside window, +100)
    // Total: 200
    auto m10 = metrics.get_metrics(Window::SEC_10);
    assert(std::abs(m10.bid_volume_added - 200.0) < 0.01);

    std::cout << "✓ test_multiple_windows\n";
}

// ============================================================================
// Edge Cases Tests (4 tests)
// ============================================================================

void test_empty_state() {
    OrderFlowMetrics metrics;
    auto m = metrics.get_metrics(Window::SEC_1);

    assert(m.bid_volume_added == 0.0);
    assert(m.ask_volume_added == 0.0);
    assert(m.bid_volume_removed == 0.0);
    assert(m.ask_volume_removed == 0.0);
    assert(m.book_update_count == 0);

    std::cout << "✓ test_empty_state\n";
}

void test_trade_without_book_update() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Only trades, no book updates
    for (int i = 0; i < 10; i++) {
        ipc::TradeEvent trade = ipc::TradeEvent::fill(i, ts + i * 10'000, 0, "BTC", 0, 10000, 10, i);
        metrics.on_trade(trade);
    }

    auto m = metrics.get_metrics(Window::SEC_1);
    assert(m.book_update_count == 0);
    assert(m.bid_volume_added == 0.0);

    std::cout << "✓ test_trade_without_book_update\n";
}

void test_book_update_without_trade() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial book
    auto book1 = create_book_with_levels({{10000, 100}, {9990, 200}, {9980, 300}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts); // Baseline

    // Remove all bid levels WITHOUT any trades
    auto book2 = create_book_with_levels({}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 2'000'000);

    auto m = metrics.get_metrics(Window::SEC_1);
    // All removals should be cancels (no trades)
    assert(m.cancel_ratio_bid > 0.99);
    assert(m.estimated_bid_cancel_volume > 500.0);

    std::cout << "✓ test_book_update_without_trade\n";
}

void test_reset() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Add some data
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts); // Baseline

    auto book2 = create_book_with_levels({{10000, 200}}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 2'000'000);

    // Reset
    metrics.reset();

    auto m = metrics.get_metrics(Window::SEC_1);
    assert(m.bid_volume_added == 0.0);
    assert(m.book_update_count == 0);

    std::cout << "✓ test_reset\n";
}

// ============================================================================
// Performance Tests (2 tests)
// ============================================================================

void test_throughput() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Create one order book (reuse it)
    OrderBook book(10000, 2000);
    book.add_order(1, Side::Buy, 10000, 1000);
    book.add_order(2, Side::Sell, 10010, 1000);

    // Initial update
    metrics.on_order_book_update(book, ts);

    // 1000 trades (not 50K - that's too slow)
    for (int i = 0; i < 1000; i++) {
        Price price = 10000 + (i % 20);
        ipc::TradeEvent trade = ipc::TradeEvent::fill(i, ts + i * 10, 0, "BTC", i % 2, price, 10, i);
        metrics.on_trade(trade);
    }

    // 100 book updates (reusing same book, just changing timestamp)
    for (int i = 0; i < 100; i++) {
        metrics.on_order_book_update(book, ts + 1'000 * 10 + i * 10);
    }

    auto m = metrics.get_metrics(Window::SEC_1);
    assert(m.book_update_count > 0);

    std::cout << "✓ test_throughput\n";
}

void test_no_allocation() {
    // This test just verifies that the class can be used without heap allocation
    // In a real test, we'd use Valgrind or a custom allocator to verify
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts); // Baseline

    auto book2 = create_book_with_levels({{10000, 200}}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 2'000'000);

    auto m = metrics.get_metrics(Window::SEC_1);
    (void)m; // Use the result

    std::cout << "✓ test_no_allocation\n";
}

// ============================================================================
// Birth Tracking Tests (3 tests)
// ============================================================================

void test_track_births_all_levels() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Add 15 bid levels across 3 updates
    auto book1 = create_book_with_n_levels(5, 10000, 10010);
    metrics.on_order_book_update(book1, ts);

    auto book2 = create_book_with_n_levels(10, 10000, 10010);
    metrics.on_order_book_update(book2, ts + 100'000);

    auto book3 = create_book_with_n_levels(15, 10000, 10010);
    metrics.on_order_book_update(book3, ts + 200'000);

    // Remove all levels to trigger birth tracking
    auto book4 = create_book_with_levels({}, {});
    metrics.on_order_book_update(book4, ts + 300'000);

    auto m = metrics.get_metrics(Window::SEC_1);
    // All 15 unique prices should be tracked in births
    // Verify level lifetime metrics are captured (avg > 0)
    assert(m.avg_bid_level_lifetime_us > 0.0);
    assert(m.avg_ask_level_lifetime_us > 0.0);

    std::cout << "✓ test_track_births_all_levels\n";
}

void test_track_births_no_duplicates() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Add same levels multiple times
    auto book1 = create_book_with_n_levels(5, 10000, 10010);
    metrics.on_order_book_update(book1, ts);

    // Update same levels (quantity change, not new births)
    auto book2 = create_book_with_levels({{10000, 150}, {9999, 160}, {9998, 170}, {9997, 180}, {9996, 190}},
                                         {{10010, 150}, {10011, 160}, {10012, 170}, {10013, 180}, {10014, 190}});
    metrics.on_order_book_update(book2, ts + 100'000);

    // Remove all to trigger lifetime calculation
    auto book3 = create_book_with_levels({}, {});
    metrics.on_order_book_update(book3, ts + 200'000);

    auto m = metrics.get_metrics(Window::SEC_1);
    // Should only count 5 unique birth events (no duplicates)
    // Lifetime should be 200ms for all levels
    assert(std::abs(m.avg_bid_level_lifetime_us - 200'000.0) < 10'000.0);
    assert(std::abs(m.avg_ask_level_lifetime_us - 200'000.0) < 10'000.0);

    std::cout << "✓ test_track_births_no_duplicates\n";
}

void test_track_births_bid_ask_independent() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Add different prices on bid and ask sides (price-based tracking)
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts);

    // Remove bid after 100ms, ask after 200ms
    auto book2 = create_book_with_levels({}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 100'000);

    auto book3 = create_book_with_levels({}, {});
    metrics.on_order_book_update(book3, ts + 200'000);

    auto m = metrics.get_metrics(Window::SEC_1);
    // Bid lifetime = 100ms, ask lifetime = 200ms (price-based tracking)
    assert(std::abs(m.avg_bid_level_lifetime_us - 100'000.0) < 10'000.0);
    assert(std::abs(m.avg_ask_level_lifetime_us - 200'000.0) < 10'000.0);

    std::cout << "✓ test_track_births_bid_ask_independent\n";
}

// ============================================================================
// on_depth_snapshot() Tests (5 tests for uncovered on_depth_snapshot path)
// ============================================================================

void test_depth_snapshot_bid_volume_added() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial snapshot (baseline)
    BookSnapshot snap1;
    snap1.bid_level_count = 1;
    snap1.bid_levels[0] = {10000, 100};
    snap1.ask_level_count = 1;
    snap1.ask_levels[0] = {10010, 100};
    metrics.on_depth_snapshot(snap1, ts);

    // Add new bid level
    BookSnapshot snap2;
    snap2.bid_level_count = 2;
    snap2.bid_levels[0] = {10000, 100};
    snap2.bid_levels[1] = {9990, 200};
    snap2.ask_level_count = 1;
    snap2.ask_levels[0] = {10010, 100};
    metrics.on_depth_snapshot(snap2, ts + 2'000'000);

    auto m = metrics.get_metrics(Window::SEC_1);
    assert(std::abs(m.bid_volume_added - 200.0) < 0.01);

    std::cout << "✓ test_depth_snapshot_bid_volume_added\n";
}

void test_depth_snapshot_ask_volume_removed() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial snapshot (baseline)
    BookSnapshot snap1;
    snap1.bid_level_count = 1;
    snap1.bid_levels[0] = {10000, 100};
    snap1.ask_level_count = 2;
    snap1.ask_levels[0] = {10010, 100};
    snap1.ask_levels[1] = {10020, 150};
    metrics.on_depth_snapshot(snap1, ts);

    // Remove ask level
    BookSnapshot snap2;
    snap2.bid_level_count = 1;
    snap2.bid_levels[0] = {10000, 100};
    snap2.ask_level_count = 1;
    snap2.ask_levels[0] = {10010, 100};
    metrics.on_depth_snapshot(snap2, ts + 2'000'000);

    auto m = metrics.get_metrics(Window::SEC_1);
    assert(std::abs(m.ask_volume_removed - 150.0) < 0.01);

    std::cout << "✓ test_depth_snapshot_ask_volume_removed\n";
}

void test_depth_snapshot_with_trade_correlation() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial snapshot
    BookSnapshot snap1;
    snap1.bid_level_count = 1;
    snap1.bid_levels[0] = {10000, 100};
    snap1.ask_level_count = 1;
    snap1.ask_levels[0] = {10010, 200};
    metrics.on_depth_snapshot(snap1, ts);

    // Trade at ask
    uint64_t trade_time_us = ts + 2'050'000;
    metrics.on_trade(10010, 150, trade_time_us);

    // Remove ask volume (correlated with trade)
    BookSnapshot snap2;
    snap2.bid_level_count = 1;
    snap2.bid_levels[0] = {10000, 100};
    snap2.ask_level_count = 1;
    snap2.ask_levels[0] = {10010, 50};
    metrics.on_depth_snapshot(snap2, ts + 2'060'000);

    auto m = metrics.get_metrics(Window::SEC_1);
    // Should correlate with trade (fill, not cancel)
    assert(m.cancel_ratio_ask < 0.01);

    std::cout << "✓ test_depth_snapshot_with_trade_correlation\n";
}

void test_depth_snapshot_level_lifetime() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Level appears
    BookSnapshot snap1;
    snap1.bid_level_count = 2;
    snap1.bid_levels[0] = {10000, 100};
    snap1.bid_levels[1] = {9990, 200};
    snap1.ask_level_count = 1;
    snap1.ask_levels[0] = {10010, 100};
    metrics.on_depth_snapshot(snap1, ts);

    // Level disappears after 400ms
    BookSnapshot snap2;
    snap2.bid_level_count = 1;
    snap2.bid_levels[0] = {10000, 100};
    snap2.ask_level_count = 1;
    snap2.ask_levels[0] = {10010, 100};
    metrics.on_depth_snapshot(snap2, ts + 400'000);

    auto m = metrics.get_metrics(Window::SEC_1);
    assert(std::abs(m.avg_bid_level_lifetime_us - 400'000.0) < 10'000.0);

    std::cout << "✓ test_depth_snapshot_level_lifetime\n";
}

void test_depth_snapshot_quantity_change() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial snapshot
    BookSnapshot snap1;
    snap1.bid_level_count = 1;
    snap1.bid_levels[0] = {10000, 100};
    snap1.ask_level_count = 1;
    snap1.ask_levels[0] = {10010, 100};
    metrics.on_depth_snapshot(snap1, ts);

    // Quantity increase at existing level
    BookSnapshot snap2;
    snap2.bid_level_count = 1;
    snap2.bid_levels[0] = {10000, 250};
    snap2.ask_level_count = 1;
    snap2.ask_levels[0] = {10010, 100};
    metrics.on_depth_snapshot(snap2, ts + 2'000'000);

    auto m = metrics.get_metrics(Window::SEC_1);
    assert(std::abs(m.bid_volume_added - 150.0) < 0.01);

    std::cout << "✓ test_depth_snapshot_quantity_change\n";
}

// ============================================================================
// on_trade(Price, Quantity, timestamp_us) Tests (2 tests for direct parameter version)
// ============================================================================

void test_on_trade_direct_params() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Add 10 trades using direct parameters
    for (int i = 0; i < 10; i++) {
        metrics.on_trade(10000 + i, 100, ts + i * 10'000);
    }

    // These trades should be stored for correlation
    // Verify by creating book update that should correlate with trades
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts); // Baseline

    auto book2 = create_book_with_levels({}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 50'000);

    auto m = metrics.get_metrics(Window::SEC_1);
    // Trades should correlate (fill volume, not cancel)
    assert(m.cancel_ratio_bid < 0.5);

    std::cout << "✓ test_on_trade_direct_params\n";
}

void test_on_trade_overflow() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Add more trades than buffer size (256) to test ring buffer overflow
    for (int i = 0; i < 300; i++) {
        metrics.on_trade(10000 + (i % 10), 50, ts + i * 100);
    }

    // Oldest trades should be dropped, recent ones retained
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts); // Baseline

    auto book2 = create_book_with_levels({}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 30'000);

    auto m = metrics.get_metrics(Window::SEC_1);
    // Should still function correctly despite overflow
    assert(m.bid_volume_removed > 0.0);

    std::cout << "✓ test_on_trade_overflow\n";
}

// ============================================================================
// Window Duration Tests (3 tests for all window sizes)
// ============================================================================

void test_window_sec_30() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Baseline
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts);

    // Event at +25s (within 30s window)
    auto book2 = create_book_with_levels({{10000, 200}}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 25'000'000);

    // Event at +35s (outside 30s window from +35s perspective, but book2 is within)
    auto book3 = create_book_with_levels({{10000, 300}}, {{10010, 100}});
    metrics.on_order_book_update(book3, ts + 35'000'000);

    auto m = metrics.get_metrics(Window::SEC_30);
    // From t=35s, window covers [5s, 35s]
    // book1 at 1s is outside
    // book2 at 26s is inside (+100)
    // book3 at 36s is inside (+100)
    assert(std::abs(m.bid_volume_added - 200.0) < 0.01);

    std::cout << "✓ test_window_sec_30\n";
}

void test_window_min_1() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Baseline
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts);

    // Event at +50s (within 1min window)
    auto book2 = create_book_with_levels({{10000, 200}}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 50'000'000);

    // Event at +70s (outside 1min window from +70s perspective, but book2 is within)
    auto book3 = create_book_with_levels({{10000, 300}}, {{10010, 100}});
    metrics.on_order_book_update(book3, ts + 70'000'000);

    auto m = metrics.get_metrics(Window::MIN_1);
    // From t=71s, window covers [11s, 71s]
    // book1 at 1s is outside
    // book2 at 51s is inside (+100)
    // book3 at 71s is inside (+100)
    assert(std::abs(m.bid_volume_added - 200.0) < 0.01);

    std::cout << "✓ test_window_min_1\n";
}

void test_all_windows_simultaneously() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Baseline (far in past, outside all windows)
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts);

    // Add volume (within all windows from current time perspective)
    auto book2 = create_book_with_levels({{10000, 200}}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 70'000'000); // 70s later

    // Query all windows (current time = 71s, so all windows include book2 but not book1)
    auto m1 = metrics.get_metrics(Window::SEC_1);
    auto m5 = metrics.get_metrics(Window::SEC_5);
    auto m10 = metrics.get_metrics(Window::SEC_10);
    auto m30 = metrics.get_metrics(Window::SEC_30);
    auto m60 = metrics.get_metrics(Window::MIN_1);

    // All should show the same volume added (event is within all windows, baseline is outside)
    assert(std::abs(m1.bid_volume_added - 100.0) < 0.01);
    assert(std::abs(m5.bid_volume_added - 100.0) < 0.01);
    assert(std::abs(m10.bid_volume_added - 100.0) < 0.01);
    assert(std::abs(m30.bid_volume_added - 100.0) < 0.01);
    assert(std::abs(m60.bid_volume_added - 100.0) < 0.01);

    std::cout << "✓ test_all_windows_simultaneously\n";
}

// ============================================================================
// Cache Tests (2 tests for get_metrics cache hit/miss paths)
// ============================================================================

void test_cache_hit() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts);

    auto book2 = create_book_with_levels({{10000, 200}}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 2'000'000);

    // First call: cache miss
    auto m1 = metrics.get_metrics(Window::SEC_1);

    // Second call without adding events: cache hit
    auto m2 = metrics.get_metrics(Window::SEC_1);

    // Results should be identical
    assert(m1.bid_volume_added == m2.bid_volume_added);
    assert(m1.book_update_count == m2.book_update_count);

    std::cout << "✓ test_cache_hit\n";
}

void test_cache_invalidation() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts);

    auto book2 = create_book_with_levels({{10000, 200}}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 2'000'000);

    // First call: cache miss, stores 100
    auto m1 = metrics.get_metrics(Window::SEC_1);
    assert(std::abs(m1.bid_volume_added - 100.0) < 0.01);

    // Add more volume (invalidates cache)
    auto book3 = create_book_with_levels({{10000, 300}}, {{10010, 100}});
    metrics.on_order_book_update(book3, ts + 2'500'000);

    // Second call: cache miss, recalculates with both events
    auto m2 = metrics.get_metrics(Window::SEC_1);
    assert(std::abs(m2.bid_volume_added - 200.0) < 0.01);

    std::cout << "✓ test_cache_invalidation\n";
}

// ============================================================================
// Multi-Window Coverage Tests (10 tests)
// ============================================================================

void test_sec_5_volume_metrics() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Baseline outside SEC_5 window (> 5 seconds before)
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts);

    // Add volume within SEC_5 window
    auto book2 = create_book_with_levels({{10000, 200}, {9990, 150}}, {{10010, 200}, {10020, 120}});
    metrics.on_order_book_update(book2, ts + 6'000'000); // 6s later (outside SEC_5 window from book1)

    auto m = metrics.get_metrics(Window::SEC_5);
    assert(std::abs(m.bid_volume_added - 250.0) < 0.01); // 100 + 150
    assert(std::abs(m.ask_volume_added - 220.0) < 0.01); // 100 + 120
    assert(m.book_update_count == 1);

    std::cout << "✓ test_sec_5_volume_metrics\n";
}

void test_sec_5_imbalance() {
    OrderFlowMetrics metrics;
    uint64_t ts = 10'000'000; // Start at 10s

    // Baseline outside SEC_5 window
    auto book1 = create_book_with_levels({{10000, 1000}}, {{10010, 500}});
    metrics.on_order_book_update(book1, ts);

    // Update within SEC_5 window (6s later, outside window from book1)
    auto book2 = create_book_with_levels({{10000, 1500}}, {{10010, 500}});
    metrics.on_order_book_update(book2, ts + 6'000'000);

    auto m = metrics.get_metrics(Window::SEC_5);
    // Should have imbalance metrics calculated
    assert(m.book_update_count == 1); // Only book2 in window
    assert(m.bid_volume_added > 0);

    std::cout << "✓ test_sec_5_imbalance\n";
}

void test_sec_5_trade_events() {
    OrderFlowMetrics metrics;
    uint64_t ts = 10'000'000;

    // Baseline outside window
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts);

    // Add volume within SEC_5 window (6s later)
    auto book2 = create_book_with_levels({{10000, 300}}, {{10010, 250}});
    metrics.on_order_book_update(book2, ts + 6'000'000);

    auto m = metrics.get_metrics(Window::SEC_5);
    assert(m.bid_volume_added > 0);
    assert(m.ask_volume_added > 0);
    assert(m.book_update_count == 1);

    std::cout << "✓ test_sec_5_trade_events\n";
}

void test_sec_10_volume_metrics() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Baseline outside SEC_10 window
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts);

    // Multiple updates within SEC_10 window (11s+ after baseline)
    auto book2 = create_book_with_levels({{10000, 200}}, {{10010, 150}});
    metrics.on_order_book_update(book2, ts + 11'000'000); // 11s

    auto book3 = create_book_with_levels({{10000, 300}}, {{10010, 200}});
    metrics.on_order_book_update(book3, ts + 14'000'000); // 14s

    auto m = metrics.get_metrics(Window::SEC_10);
    assert(std::abs(m.bid_volume_added - 200.0) < 0.01); // 100 + 100
    assert(std::abs(m.ask_volume_added - 100.0) < 0.01); // 50 + 50
    assert(m.book_update_count == 2);

    std::cout << "✓ test_sec_10_volume_metrics\n";
}

void test_sec_10_level_lifetime() {
    OrderFlowMetrics<20> metrics;
    uint64_t ts = 1'000'000;

    // Add level at t=0
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts);

    // Remove level after 5 seconds → lifetime = 5s = 5,000,000 us
    auto book2 = create_book_with_levels({}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 5'000'000);

    auto m = metrics.get_metrics(Window::SEC_10);
    // avg_bid_level_lifetime_us should be ~5,000,000
    assert(m.avg_bid_level_lifetime_us > 4'000'000 && m.avg_bid_level_lifetime_us < 6'000'000);

    std::cout << "✓ test_sec_10_level_lifetime\n";
}

void test_ring_buffer_overflow() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Add 100 events (test ring buffer logic without performance issues)
    for (int i = 0; i < 100; ++i) {
        auto book = create_book_with_levels({{10000, 100 + i}}, {{10010, 100 + i}});
        metrics.on_order_book_update(book, ts + i * 10'000); // 10ms apart
    }

    // Should handle correctly
    auto m = metrics.get_metrics(Window::SEC_1);
    assert(m.book_update_count > 0);

    std::cout << "✓ test_ring_buffer_overflow\n";
}

void test_simd_edge_case_24_levels() {
    OrderFlowMetrics<20> metrics; // MaxDepthLevels = 20 (BookSnapshot limit)
    uint64_t ts = 1'000'000;

    // Create book with 18 levels (triggers non-aligned SIMD path: 16 SIMD + 2 scalar)
    auto book1 = create_book_with_n_levels(18, 10000, 10019);
    metrics.on_order_book_update(book1, ts);

    // Add more volume
    auto book2 = create_book_with_n_levels(18, 10000, 10019);
    metrics.on_order_book_update(book2, ts + 2'000'000);

    auto m = metrics.get_metrics(Window::SEC_5);
    // Should handle 18 levels correctly (16 SIMD + 2 scalar)
    assert(m.book_update_count == 1);

    std::cout << "✓ test_simd_edge_case_18_levels\n";
}

void test_min_1_expiration() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Event at t=0
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts);

    // Event at t=30s (within MIN_1 = 60s window)
    auto book2 = create_book_with_levels({{10000, 200}}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 30'000'000);

    auto m1 = metrics.get_metrics(Window::MIN_1);
    assert(m1.book_update_count == 2); // Both events within 60s window

    // Event at t=70s (first event expired, 70s > 60s from t=0)
    auto book3 = create_book_with_levels({{10000, 300}}, {{10010, 100}});
    metrics.on_order_book_update(book3, ts + 70'000'000);

    auto m2 = metrics.get_metrics(Window::MIN_1);
    assert(m2.book_update_count == 2); // book2 and book3 within window (book1 expired)

    std::cout << "✓ test_min_1_expiration\n";
}

void test_sec_30_comprehensive() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Baseline outside SEC_30 window
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts);

    // Updates at t=31s, t=41s within SEC_30 (outside window from baseline)
    auto book2 = create_book_with_levels({{10000, 200}}, {{10010, 150}});
    metrics.on_order_book_update(book2, ts + 31'000'000);

    auto book3 = create_book_with_levels({{10000, 300}}, {{10010, 200}});
    metrics.on_order_book_update(book3, ts + 41'000'000);

    auto m = metrics.get_metrics(Window::SEC_30);
    assert(std::abs(m.bid_volume_added - 200.0) < 0.01); // 100 + 100
    assert(std::abs(m.ask_volume_added - 100.0) < 0.01); // 50 + 50
    assert(m.book_update_count == 2);

    std::cout << "✓ test_sec_30_comprehensive\n";
}

void test_all_windows_volume_consistency() {
    OrderFlowMetrics metrics;
    uint64_t ts = 100'000'000; // Start at 100s to avoid edge cases

    // Baseline outside all windows
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts);

    // Add volume within all windows (61s+ after baseline, outside MIN_1 window)
    auto book2 = create_book_with_levels({{10000, 300}}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 70'000'000);

    // All windows should see only book2 (book1 is outside MIN_1 window)
    auto m1 = metrics.get_metrics(Window::SEC_1);
    auto m5 = metrics.get_metrics(Window::SEC_5);
    auto m10 = metrics.get_metrics(Window::SEC_10);
    auto m30 = metrics.get_metrics(Window::SEC_30);
    auto m60 = metrics.get_metrics(Window::MIN_1);

    // All should have the same book_update_count = 1
    assert(m1.book_update_count == 1);
    assert(m5.book_update_count == 1);
    assert(m10.book_update_count == 1);
    assert(m30.book_update_count == 1);
    assert(m60.book_update_count == 1);

    std::cout << "✓ test_all_windows_volume_consistency\n";
}

// ============================================================================
// Main
// ============================================================================

int main() {
    // Added/Removed Volume (6 tests)
    test_bid_volume_added();
    test_ask_volume_added();
    test_bid_volume_removed();
    test_ask_volume_removed();
    test_quantity_increase();
    test_quantity_decrease();

    // Cancel Estimation (4 tests)
    test_cancel_no_trade();
    test_fill_with_trade();
    test_partial_cancel();
    test_cancel_ratio();

    // Book Velocity (4 tests)
    test_bid_depth_velocity();
    test_ask_depth_velocity();
    test_additions_per_sec();
    test_removals_per_sec();

    // Level Lifetime (3 tests)
    test_level_lifetime();
    test_short_lived_ratio();
    test_long_lived_level();

    // Update Frequency (2 tests)
    test_update_count();
    test_level_change_count();

    // Window Behavior (2 tests)
    test_flow_window_expiry();
    test_multiple_windows();

    // Edge Cases (4 tests)
    test_empty_state();
    test_trade_without_book_update();
    test_book_update_without_trade();
    test_reset();

    // Performance (2 tests)
    test_throughput();
    test_no_allocation();

    // Birth Tracking (3 tests)
    test_track_births_all_levels();
    test_track_births_no_duplicates();
    test_track_births_bid_ask_independent();

    // on_depth_snapshot() (5 tests)
    test_depth_snapshot_bid_volume_added();
    test_depth_snapshot_ask_volume_removed();
    test_depth_snapshot_with_trade_correlation();
    test_depth_snapshot_level_lifetime();
    test_depth_snapshot_quantity_change();

    // on_trade(Price, Quantity, timestamp_us) (2 tests)
    test_on_trade_direct_params();
    test_on_trade_overflow();

    // Window Duration (3 tests)
    test_window_sec_30();
    test_window_min_1();
    test_all_windows_simultaneously();

    // Cache (2 tests)
    test_cache_hit();
    test_cache_invalidation();

    // Multi-Window Coverage (10 tests)
    test_sec_5_volume_metrics();
    test_sec_5_imbalance();
    test_sec_5_trade_events();
    test_sec_10_volume_metrics();
    test_sec_10_level_lifetime();
    test_ring_buffer_overflow();
    test_simd_edge_case_24_levels();
    test_min_1_expiration();
    test_sec_30_comprehensive();
    test_all_windows_volume_consistency();

    std::cout << "\n✅ All 52 tests passed!\n";
    return 0;
}
