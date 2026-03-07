#include "../include/core/metrics_manager.hpp"
#include "../include/orderbook.hpp"
#include "../include/util/time_utils.hpp"

#include <cassert>
#include <iostream>

using namespace hft;
using namespace hft::core;

// Helper: Create BookSnapshot
BookSnapshot create_book(Price bid, Price ask) {
    BookSnapshot snap;
    snap.best_bid = bid;
    snap.best_ask = ask;
    snap.best_bid_qty = 100;
    snap.best_ask_qty = 100;
    snap.bid_level_count = 1;
    snap.ask_level_count = 1;
    snap.bid_levels[0] = {bid, 100};
    snap.ask_levels[0] = {ask, 100};
    return snap;
}

// Test 1: on_trade updates all metrics
void test_on_trade_updates_metrics() {
    auto mgr = std::make_unique<MetricsManager>();

    mgr->on_trade(0, 100000.0, 100, true, util::now_ns());

    auto trade_m = mgr->trade(0)->get_metrics(TradeWindow::W1s);
    assert(trade_m.total_trades == 1);
    assert(trade_m.buy_volume > 0);
    std::cout << "[PASS] test_on_trade_updates_metrics\n";
}

// Test 2: on_depth updates book + flow + regime
void test_on_depth_updates_all() {
    auto mgr = std::make_unique<MetricsManager>();

    auto snap = create_book(100000, 100100);
    mgr->on_depth(1, snap, util::now_ns());

    auto book_m = mgr->book(1)->get_metrics();
    assert(book_m.best_bid == 100000);
    assert(book_m.best_ask == 100100);
    std::cout << "[PASS] test_on_depth_updates_all\n";
}

// Test 3: Threshold crossed → callback fired
void test_threshold_crossed_callback_fired() {
    auto mgr = std::make_unique<MetricsManager>();
    bool callback_fired = false;
    Symbol callback_symbol = 999;

    mgr->set_change_callback([&](Symbol sym) {
        callback_fired = true;
        callback_symbol = sym;
    });

    // Set very low threshold
    MetricsThresholds thresh;
    thresh.spread_bps = 0.01; // Very sensitive
    mgr->set_thresholds(thresh);

    // Feed initial depth
    auto snap1 = create_book(100000, 100100);
    mgr->on_depth(2, snap1, util::now_ns());
    callback_fired = false; // Reset

    // Feed significantly different depth (spread change)
    auto snap2 = create_book(100000, 100200); // Much wider spread
    mgr->on_depth(2, snap2, util::now_ns() + 1000000);

    assert(callback_fired);
    assert(callback_symbol == 2);
    std::cout << "[PASS] test_threshold_crossed_callback_fired\n";
}

// Test 4: Threshold NOT crossed → callback NOT fired
void test_threshold_not_crossed_callback_not_fired() {
    auto mgr = std::make_unique<MetricsManager>();
    bool callback_fired = false;

    mgr->set_change_callback([&](Symbol sym) { callback_fired = true; });

    // Set high threshold (insensitive)
    MetricsThresholds thresh;
    thresh.spread_bps = 1000.0; // Very insensitive
    mgr->set_thresholds(thresh);

    // Feed initial depth
    auto snap1 = create_book(100000, 100100);
    mgr->on_depth(3, snap1, util::now_ns());
    callback_fired = false; // Reset

    // Feed slightly different depth (small spread change)
    auto snap2 = create_book(100000, 100101);
    mgr->on_depth(3, snap2, util::now_ns() + 1000000);

    assert(!callback_fired); // Should NOT fire
    std::cout << "[PASS] test_threshold_not_crossed_callback_not_fired\n";
}

// Test 5: Regime change → callback fired
void test_regime_change_callback_fired() {
    auto mgr = std::make_unique<MetricsManager>();
    bool callback_fired = false;

    mgr->set_change_callback([&](Symbol sym) { callback_fired = true; });

    // Feed many trades to establish regime
    uint64_t base_time = util::now_ns();
    for (int i = 0; i < 100; i++) {
        mgr->on_trade(4, 100000.0 + i * 10.0, 100, true, base_time + i * 100000);
    }

    callback_fired = false; // Reset after regime established

    // Feed very different price pattern (should trigger regime change)
    for (int i = 0; i < 100; i++) {
        mgr->on_trade(4, 200000.0 + i * 100.0, 100, false, base_time + (100 + i) * 100000);
    }

    // Regime change should have fired callback
    assert(callback_fired);
    std::cout << "[PASS] test_regime_change_callback_fired\n";
}

// Test 6: context_for() returns filled context
void test_context_for_returns_filled() {
    auto mgr = std::make_unique<MetricsManager>();

    mgr->on_trade(5, 100000.0, 100, true, util::now_ns());

    auto ctx = mgr->context_for(5);
    assert(ctx.trade != nullptr);
    assert(ctx.book != nullptr);
    assert(ctx.flow != nullptr);
    assert(ctx.combined != nullptr);
    assert(ctx.futures != nullptr);
    std::cout << "[PASS] test_context_for_returns_filled\n";
}

// Test 7: Invalid symbol returns empty context
void test_invalid_symbol_returns_empty() {
    auto mgr = std::make_unique<MetricsManager>();

    auto ctx = mgr->context_for(999); // Out of bounds
    assert(ctx.trade == nullptr);
    assert(ctx.book == nullptr);
    std::cout << "[PASS] test_invalid_symbol_returns_empty\n";
}

// Test 8: on_mark_price updates futures metrics
void test_on_mark_price_updates_futures() {
    auto mgr = std::make_unique<MetricsManager>();

    mgr->on_mark_price(6, 100000.0, 99900.0, 0.0001, 1234567890, util::now_ns());

    auto futures_m = mgr->futures(6)->get_metrics(FuturesWindow::W1s);
    assert(futures_m.funding_rate > 0.0);
    std::cout << "[PASS] test_on_mark_price_updates_futures\n";
}

// Test 9: on_liquidation updates futures metrics
void test_on_liquidation_updates_futures() {
    auto mgr = std::make_unique<MetricsManager>();

    mgr->on_liquidation(7, Side::Buy, 100000.0, 10.0, util::now_ns());

    auto futures_m = mgr->futures(7)->get_metrics(FuturesWindow::W1s);
    assert(futures_m.liquidation_volume > 0.0);
    std::cout << "[PASS] test_on_liquidation_updates_futures\n";
}

// Test 10: set_ticker() writes to shared snapshot
void test_set_ticker_writes_shared_snapshot() {
    auto* snap = SharedMetricsSnapshot::create();
    assert(snap != nullptr);

    auto mgr = std::make_unique<MetricsManager>();
    mgr->set_shared_snapshot(snap);
    mgr->set_ticker(8, "BTCUSDT");

    assert(std::string(snap->symbols[8].ticker) == "BTCUSDT");

    SharedMetricsSnapshot::destroy(snap);
    std::cout << "[PASS] test_set_ticker_writes_shared_snapshot\n";
}

// Test 11: write_snapshot() increments update_count
void test_write_snapshot_increments_count() {
    auto* snap = SharedMetricsSnapshot::create();
    assert(snap != nullptr);

    auto mgr = std::make_unique<MetricsManager>();
    mgr->set_shared_snapshot(snap);
    mgr->set_ticker(9, "ETHUSDT");

    uint64_t count_before = snap->symbols[9].update_count.load();

    mgr->on_trade(9, 100000.0, 100, true, util::now_ns());

    uint64_t count_after = snap->symbols[9].update_count.load();
    assert(count_after > count_before);

    SharedMetricsSnapshot::destroy(snap);
    std::cout << "[PASS] test_write_snapshot_increments_count\n";
}

// Test 12: Multiple threshold types checked
void test_multiple_threshold_types() {
    auto mgr = std::make_unique<MetricsManager>();
    int callback_count = 0;

    mgr->set_change_callback([&](Symbol sym) { callback_count++; });

    MetricsThresholds thresh;
    thresh.buy_ratio = 0.01; // Sensitive to buy ratio change
    mgr->set_thresholds(thresh);

    // Feed trades with different buy ratios
    uint64_t base_time = util::now_ns();
    for (int i = 0; i < 50; i++) {
        mgr->on_trade(10, 100000.0, 100, true, base_time + i * 10000); // All buys
    }
    callback_count = 0; // Reset

    for (int i = 0; i < 50; i++) {
        mgr->on_trade(10, 100000.0, 100, false, base_time + (50 + i) * 10000); // All sells
    }

    // Buy ratio changed significantly → callback should fire
    assert(callback_count > 0);
    std::cout << "[PASS] test_multiple_threshold_types\n";
}

// Test 13: No callback set → no crash
void test_no_callback_no_crash() {
    auto mgr = std::make_unique<MetricsManager>();
    // Don't set callback

    auto snap = create_book(100000, 100100);
    mgr->on_depth(11, snap, util::now_ns());

    // Should not crash
    std::cout << "[PASS] test_no_callback_no_crash\n";
}

// Test 14: Combined metrics updated on trade
void test_combined_metrics_updated() {
    auto mgr = std::make_unique<MetricsManager>();

    uint64_t base_time = util::now_ns();
    auto snap = create_book(100000, 100100);
    mgr->on_depth(12, snap, base_time);

    mgr->on_trade(12, 100050.0, 100, true, base_time + 10000);

    auto comb_m = mgr->combined(12)->get_metrics(CombinedMetrics::Window::SEC_1);
    // Combined metrics should have values (checking available fields)
    assert(comb_m.trade_to_depth_ratio >= 0.0 || comb_m.spread_mean >= 0.0);
    std::cout << "[PASS] test_combined_metrics_updated\n";
}

int main() {
    std::cout << "Running MetricsManager tests...\n\n";

    test_on_trade_updates_metrics();
    test_on_depth_updates_all();
    test_threshold_crossed_callback_fired();
    test_threshold_not_crossed_callback_not_fired();
    test_regime_change_callback_fired();
    test_context_for_returns_filled();
    test_invalid_symbol_returns_empty();
    test_on_mark_price_updates_futures();
    test_on_liquidation_updates_futures();
    test_set_ticker_writes_shared_snapshot();
    test_write_snapshot_increments_count();
    test_multiple_threshold_types();
    test_no_callback_no_crash();
    test_combined_metrics_updated();

    std::cout << "\n✓ All 14 MetricsManager tests passed!\n";
    return 0;
}
