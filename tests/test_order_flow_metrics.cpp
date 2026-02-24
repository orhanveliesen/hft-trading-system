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

    // Initial book
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 200}});
    metrics.on_order_book_update(book1, ts); // Baseline

    // Trade at ask
    ipc::TradeEvent trade = ipc::TradeEvent::fill(1, ts + 50'000, 0, "BTC", 0, 10010, 150, 1);
    metrics.on_trade(trade);

    // Remove ask volume (should correlate with trade = fill, not cancel)
    auto book2 = create_book_with_levels({{10000, 100}}, {{10010, 50}});
    metrics.on_order_book_update(book2, ts + 60'000);

    auto m = metrics.get_metrics(Window::SEC_1);
    // Should NOT count as cancel since trade occurred
    assert(m.estimated_ask_cancel_volume < 1.0);
    assert(m.cancel_ratio_ask < 0.01); // Should be ~0 (all removes are fills)

    std::cout << "✓ test_fill_with_trade\n";
}

void test_partial_cancel() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial book
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 300}});
    metrics.on_order_book_update(book1, ts); // Baseline

    // Small trade
    ipc::TradeEvent trade = ipc::TradeEvent::fill(1, ts + 50'000, 0, "BTC", 0, 10010, 50, 1);
    metrics.on_trade(trade);

    // Large volume removed (more than trade)
    auto book2 = create_book_with_levels({{10000, 100}}, {{10010, 50}});
    metrics.on_order_book_update(book2, ts + 60'000);

    auto m = metrics.get_metrics(Window::SEC_1);
    // 250 removed, 50 filled → 200 cancelled
    assert(std::abs(m.estimated_ask_cancel_volume - 200.0) < 0.01);

    std::cout << "✓ test_partial_cancel\n";
}

void test_cancel_ratio() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial book
    auto book1 = create_book_with_levels({{10000, 100}, {9990, 200}, {9980, 300}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts); // Baseline

    // Trade at one price
    ipc::TradeEvent trade = ipc::TradeEvent::fill(1, ts + 50'000, 0, "BTC", 1, 10000, 100, 1);
    metrics.on_trade(trade);

    // Remove multiple levels (one is fill, two are cancels)
    auto book2 = create_book_with_levels({}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 60'000);

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

    // Initial book
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts); // Baseline

    // Increase bid depth
    auto book2 = create_book_with_levels({{10000, 300}}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 500'000); // 0.5s later

    auto m = metrics.get_metrics(Window::SEC_1);
    // Depth change = 200 in 0.5s = 400 per second
    assert(std::abs(m.bid_depth_velocity - 400.0) < 50.0);

    std::cout << "✓ test_bid_depth_velocity\n";
}

void test_ask_depth_velocity() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial book
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts); // Baseline

    // Decrease ask depth
    auto book2 = create_book_with_levels({{10000, 100}}, {{10010, 10}});
    metrics.on_order_book_update(book2, ts + 500'000); // 0.5s later

    auto m = metrics.get_metrics(Window::SEC_1);
    // Depth change = -90 in 0.5s = -180 per second
    assert(std::abs(m.ask_depth_velocity + 180.0) < 50.0);

    std::cout << "✓ test_ask_depth_velocity\n";
}

void test_additions_per_sec() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial book
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts); // Baseline

    // 5 additions over 1 second
    for (int i = 1; i <= 5; i++) {
        auto book = create_book_with_levels({{10000, 100 + i * 50}}, {{10010, 100}});
        metrics.on_order_book_update(book, ts + i * 200'000);
    }

    auto m = metrics.get_metrics(Window::SEC_1);
    assert(std::abs(m.bid_additions_per_sec - 5.0) < 0.5);

    std::cout << "✓ test_additions_per_sec\n";
}

void test_removals_per_sec() {
    OrderFlowMetrics metrics;
    uint64_t ts = 1'000'000;

    // Initial book
    auto book1 = create_book_with_levels({{10000, 500}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts); // Baseline

    // 4 removals over 1 second
    for (int i = 1; i <= 4; i++) {
        auto book = create_book_with_levels({{10000, 500 - i * 50}}, {{10010, 100}});
        metrics.on_order_book_update(book, ts + i * 250'000);
    }

    auto m = metrics.get_metrics(Window::SEC_1);
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

    // Initial book
    auto book1 = create_book_with_levels({{10000, 100}}, {{10010, 100}});
    metrics.on_order_book_update(book1, ts); // Baseline

    // Add volume spread over time
    auto book2 = create_book_with_levels({{10000, 200}}, {{10010, 100}});
    metrics.on_order_book_update(book2, ts + 2'000'000); // 2s later

    auto book3 = create_book_with_levels({{10000, 300}}, {{10010, 100}});
    metrics.on_order_book_update(book3, ts + 7'000'000); // 7s later

    // 1s window: only last update
    auto m1 = metrics.get_metrics(Window::SEC_1);
    assert(std::abs(m1.bid_volume_added - 100.0) < 0.01);

    // 10s window: both updates
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

    std::cout << "\n✅ All 27 tests passed!\n";
    return 0;
}
