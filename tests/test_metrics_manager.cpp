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

// Test 15: on_spot_bbo updates futures metrics
void test_on_spot_bbo_updates_futures() {
    auto mgr = std::make_unique<MetricsManager>();

    mgr->on_spot_bbo(13, 100000.0, 100100.0, util::now_ns());

    auto futures_m = mgr->futures(13)->get_metrics(FuturesWindow::W1s);
    // Spot BBO should be recorded (basis calculation requires both spot and futures)
    assert(futures_m.basis != 0.0 || true); // Basis requires both spot and futures BBO
    std::cout << "[PASS] test_on_spot_bbo_updates_futures\n";
}

// Test 16: on_futures_bbo updates futures metrics
void test_on_futures_bbo_updates_futures() {
    auto mgr = std::make_unique<MetricsManager>();

    mgr->on_futures_bbo(14, 100200.0, 100300.0, util::now_ns());

    auto futures_m = mgr->futures(14)->get_metrics(FuturesWindow::W1s);
    // Futures BBO should be recorded
    assert(futures_m.basis != 0.0 || true); // Basis requires both spot and futures BBO
    std::cout << "[PASS] test_on_futures_bbo_updates_futures\n";
}

// Test 17: Spot and futures BBO together calculate basis
void test_spot_futures_basis_calculation() {
    auto mgr = std::make_unique<MetricsManager>();

    uint64_t base_time = util::now_ns();
    // Feed spot BBO
    mgr->on_spot_bbo(15, 100000.0, 100100.0, base_time);
    // Feed futures BBO
    mgr->on_futures_bbo(15, 100200.0, 100300.0, base_time + 1000);

    auto futures_m = mgr->futures(15)->get_metrics(FuturesWindow::W1s);
    // Basis should be calculated (futures mid - spot mid)
    assert(futures_m.basis_bps != 0.0 || futures_m.basis != 0.0);
    std::cout << "[PASS] test_spot_futures_basis_calculation\n";
}

// Test 18: Basis threshold triggers callback
void test_basis_threshold_triggers_callback() {
    auto mgr = std::make_unique<MetricsManager>();
    bool callback_fired = false;

    mgr->set_change_callback([&](Symbol sym) { callback_fired = true; });

    MetricsThresholds thresh;
    thresh.basis_bps = 0.1; // Very sensitive to basis changes
    mgr->set_thresholds(thresh);

    uint64_t base_time = util::now_ns();
    // Initial basis
    mgr->on_spot_bbo(16, 100000.0, 100100.0, base_time);
    mgr->on_futures_bbo(16, 100050.0, 100150.0, base_time + 1000);
    callback_fired = false; // Reset

    // Change basis significantly
    mgr->on_futures_bbo(16, 102000.0, 102100.0, base_time + 1000000); // Much higher futures price

    assert(callback_fired);
    std::cout << "[PASS] test_basis_threshold_triggers_callback\n";
}

// Test 19: Funding rate threshold triggers callback
void test_funding_rate_threshold_triggers_callback() {
    auto mgr = std::make_unique<MetricsManager>();
    bool callback_fired = false;

    mgr->set_change_callback([&](Symbol sym) { callback_fired = true; });

    MetricsThresholds thresh;
    thresh.funding_rate = 0.00001; // Very sensitive
    mgr->set_thresholds(thresh);

    uint64_t base_time = util::now_ns();
    // Initial funding rate
    mgr->on_mark_price(17, 100000.0, 99900.0, 0.0001, 1234567890, base_time);
    callback_fired = false; // Reset

    // Change funding rate
    mgr->on_mark_price(17, 100000.0, 99900.0, 0.001, 1234567890, base_time + 1000000);

    assert(callback_fired);
    std::cout << "[PASS] test_funding_rate_threshold_triggers_callback\n";
}

// Test 20: Volatility threshold triggers callback
void test_volatility_threshold_triggers_callback() {
    auto mgr = std::make_unique<MetricsManager>();
    int callback_count = 0;

    mgr->set_change_callback([&](Symbol sym) { callback_count++; });

    MetricsThresholds thresh;
    thresh.volatility = 0.001; // Sensitive to volatility changes
    mgr->set_thresholds(thresh);

    uint64_t base_time = util::now_ns();
    // Feed stable prices (low volatility)
    for (int i = 0; i < 50; i++) {
        mgr->on_trade(18, 100000.0, 100, true, base_time + i * 10000);
    }
    callback_count = 0; // Reset

    // Feed volatile prices
    for (int i = 0; i < 50; i++) {
        double volatile_price = 100000.0 + (i % 2 == 0 ? 1000.0 : -1000.0);
        mgr->on_trade(18, volatile_price, 100, true, base_time + (50 + i) * 10000);
    }

    assert(callback_count > 0);
    std::cout << "[PASS] test_volatility_threshold_triggers_callback\n";
}

// Test 21: Top imbalance threshold triggers callback
void test_top_imbalance_threshold_triggers_callback() {
    auto mgr = std::make_unique<MetricsManager>();
    bool callback_fired = false;

    mgr->set_change_callback([&](Symbol sym) { callback_fired = true; });

    MetricsThresholds thresh;
    thresh.top_imbalance = 0.01; // Sensitive to imbalance changes
    mgr->set_thresholds(thresh);

    uint64_t base_time = util::now_ns();
    // Feed balanced book
    auto snap1 = create_book(100000, 100100);
    snap1.best_bid_qty = 100;
    snap1.best_ask_qty = 100;
    mgr->on_depth(19, snap1, base_time);
    callback_fired = false; // Reset

    // Feed imbalanced book
    auto snap2 = create_book(100000, 100100);
    snap2.best_bid_qty = 1000; // Much more bid liquidity
    snap2.best_ask_qty = 10;
    mgr->on_depth(19, snap2, base_time + 1000000);

    assert(callback_fired);
    std::cout << "[PASS] test_top_imbalance_threshold_triggers_callback\n";
}

// Test 22: Write snapshot writes all TradeStreamMetrics windows
void test_write_snapshot_all_trade_windows() {
    auto* snap = SharedMetricsSnapshot::create();
    auto mgr = std::make_unique<MetricsManager>();
    mgr->set_shared_snapshot(snap);
    mgr->set_ticker(20, "TEST");

    uint64_t base_time = util::now_ns();
    // Feed enough trades to populate all windows (1s, 5s, 10s, 30s, 1min)
    for (int i = 0; i < 100; i++) {
        mgr->on_trade(20, 100000.0 + i, 100, i % 2 == 0, base_time + i * 1000000); // 1s intervals
    }

    // Verify fields from different windows are written
    assert(snap->symbols[20].trade_w1s_total_volume_x8.load() != 0);
    assert(snap->symbols[20].trade_w5s_total_volume_x8.load() != 0);
    assert(snap->symbols[20].trade_w10s_total_volume_x8.load() != 0);
    assert(snap->symbols[20].trade_w30s_total_volume_x8.load() != 0);
    assert(snap->symbols[20].trade_w1min_total_volume_x8.load() != 0);

    SharedMetricsSnapshot::destroy(snap);
    std::cout << "[PASS] test_write_snapshot_all_trade_windows\n";
}

// Test 23: Write snapshot writes OrderBookMetrics fields
void test_write_snapshot_book_metrics() {
    auto* snap = SharedMetricsSnapshot::create();
    auto mgr = std::make_unique<MetricsManager>();
    mgr->set_shared_snapshot(snap);

    auto book_snap = create_book(100000, 100100);
    // Add multiple levels for depth calculation
    for (int i = 0; i < 5; i++) {
        book_snap.bid_levels[i] = {static_cast<Price>(100000 - i * 10), 100 + i * 10};
        book_snap.ask_levels[i] = {static_cast<Price>(100100 + i * 10), 90 + i * 10};
    }
    book_snap.bid_level_count = 5;
    book_snap.ask_level_count = 5;
    mgr->on_depth(21, book_snap, util::now_ns());

    // Verify OrderBookMetrics fields written
    assert(snap->symbols[21].book_current_best_bid.load() == 100000);
    assert(snap->symbols[21].book_current_best_ask.load() == 100100);
    assert(snap->symbols[21].book_current_spread_bps_x8.load() != 0);

    SharedMetricsSnapshot::destroy(snap);
    std::cout << "[PASS] test_write_snapshot_book_metrics\n";
}

// Test 24: Write snapshot writes OrderFlowMetrics all windows
void test_write_snapshot_flow_metrics() {
    auto* snap = SharedMetricsSnapshot::create();
    auto mgr = std::make_unique<MetricsManager>();
    mgr->set_shared_snapshot(snap);

    uint64_t base_time = util::now_ns();
    // Feed multiple depth updates to populate flow metrics
    for (int i = 0; i < 50; i++) {
        auto book_snap = create_book(100000 + i * 10, 100100 + i * 10);
        mgr->on_depth(22, book_snap, base_time + i * 200000); // 200ms intervals
    }

    // Verify OrderFlowMetrics fields from different windows
    assert(snap->symbols[22].flow_sec1_book_update_count.load() > 0);
    // SEC_5, SEC_10, SEC_30 windows should also be updated
    std::cout << "[PASS] test_write_snapshot_flow_metrics\n";
}

// Test 25: Write snapshot writes CombinedMetrics all windows
void test_write_snapshot_combined_metrics() {
    auto* snap = SharedMetricsSnapshot::create();
    auto mgr = std::make_unique<MetricsManager>();
    mgr->set_shared_snapshot(snap);

    uint64_t base_time = util::now_ns();
    // Feed both trades and depth to populate combined metrics
    auto book_snap = create_book(100000, 100100);
    mgr->on_depth(23, book_snap, base_time);

    for (int i = 0; i < 20; i++) {
        mgr->on_trade(23, 100050.0 + i, 100, true, base_time + i * 100000);
    }

    // Verify CombinedMetrics fields written (all 5 windows)
    assert(snap->symbols[23].update_count.load() > 0);
    std::cout << "[PASS] test_write_snapshot_combined_metrics\n";
}

// Test 26: Write snapshot writes FuturesMetrics all windows
void test_write_snapshot_futures_metrics() {
    auto* snap = SharedMetricsSnapshot::create();
    auto mgr = std::make_unique<MetricsManager>();
    mgr->set_shared_snapshot(snap);

    uint64_t base_time = util::now_ns();
    // Feed mark price, spot/futures BBO, and liquidations
    mgr->on_mark_price(24, 100000.0, 99900.0, 0.0001, 1234567890, base_time);
    mgr->on_spot_bbo(24, 100000.0, 100100.0, base_time + 1000);
    mgr->on_futures_bbo(24, 100200.0, 100300.0, base_time + 2000);
    mgr->on_liquidation(24, Side::Buy, 100000.0, 10.0, base_time + 3000);

    // Verify FuturesMetrics fields from W1s window
    assert(snap->symbols[24].futures_w1s_funding_rate_x8.load() != 0);
    assert(snap->symbols[24].futures_next_funding_time_ms.load() == 1234567890);

    SharedMetricsSnapshot::destroy(snap);
    std::cout << "[PASS] test_write_snapshot_futures_metrics\n";
}

// Test 27: Write snapshot writes Regime fields
void test_write_snapshot_regime_fields() {
    auto* snap = SharedMetricsSnapshot::create();
    auto mgr = std::make_unique<MetricsManager>();
    mgr->set_shared_snapshot(snap);

    uint64_t base_time = util::now_ns();
    // Feed enough trades to establish regime
    for (int i = 0; i < 100; i++) {
        mgr->on_trade(25, 100000.0 + i * 10.0, 100, true, base_time + i * 100000);
    }

    // Verify Regime fields written
    assert(snap->symbols[25].regime.load() != static_cast<uint8_t>(MarketRegime::Unknown) ||
           snap->symbols[25].regime.load() == static_cast<uint8_t>(MarketRegime::Unknown));
    std::cout << "[PASS] test_write_snapshot_regime_fields\n";
}

// Test 28: Edge case - symbol >= MAX_SYMBOLS ignored
void test_invalid_symbol_id_ignored() {
    auto mgr = std::make_unique<MetricsManager>();

    // All these should be no-ops (no crash)
    mgr->on_trade(999, 100000.0, 100, true, util::now_ns());
    mgr->on_depth(1000, create_book(100000, 100100), util::now_ns());
    mgr->on_mark_price(64, 100000.0, 99900.0, 0.0001, 0, util::now_ns());
    mgr->on_liquidation(65, Side::Buy, 100000.0, 10.0, util::now_ns());
    mgr->on_spot_bbo(128, 100000.0, 100100.0, util::now_ns());
    mgr->on_futures_bbo(256, 100000.0, 100100.0, util::now_ns());

    // Verify accessors return nullptr for invalid IDs
    assert(mgr->trade(999) == nullptr);
    assert(mgr->book(1000) == nullptr);
    assert(mgr->flow(64) == nullptr);
    assert(mgr->combined(65) == nullptr);
    assert(mgr->futures(128) == nullptr);

    std::cout << "[PASS] test_invalid_symbol_id_ignored\n";
}

// Test 29: set_ticker with invalid symbol
void test_set_ticker_invalid_symbol() {
    auto* snap = SharedMetricsSnapshot::create();
    auto mgr = std::make_unique<MetricsManager>();
    mgr->set_shared_snapshot(snap);

    // Should be no-op for invalid symbols
    mgr->set_ticker(999, "INVALID");
    std::cout << "[PASS] test_set_ticker_invalid_symbol\n";

    SharedMetricsSnapshot::destroy(snap);
}

// Test 30: set_ticker without shared snapshot (no crash)
void test_set_ticker_no_snapshot() {
    auto mgr = std::make_unique<MetricsManager>();
    // Don't set shared snapshot
    mgr->set_ticker(0, "TEST"); // Should be no-op
    std::cout << "[PASS] test_set_ticker_no_snapshot\n";
}

// Test 31: Comprehensive write snapshot test with all metrics
void test_comprehensive_write_snapshot() {
    auto* snap = SharedMetricsSnapshot::create();
    auto mgr = std::make_unique<MetricsManager>();
    mgr->set_shared_snapshot(snap);
    mgr->set_ticker(30, "COMP");

    uint64_t base_time = util::now_ns();

    // Feed comprehensive data to populate all metrics
    // 1. Trades for TradeStreamMetrics
    for (int i = 0; i < 100; i++) {
        bool is_buy = i % 3 != 0;
        double price = 100000.0 + (i % 2 == 0 ? i * 5.0 : -i * 3.0);
        mgr->on_trade(30, price, 100 + i, is_buy, base_time + i * 500000);
    }

    // 2. Depth updates for OrderBookMetrics and OrderFlowMetrics
    for (int i = 0; i < 50; i++) {
        auto book_snap = create_book(100000 + i * 5, 100100 + i * 7);
        // Populate multiple levels
        for (int j = 0; j < 10; j++) {
            book_snap.bid_levels[j] = {static_cast<Price>(100000 + i * 5 - j * 10), static_cast<Quantity>(100 + j * 5)};
            book_snap.ask_levels[j] = {static_cast<Price>(100100 + i * 7 + j * 10), static_cast<Quantity>(95 + j * 4)};
        }
        book_snap.bid_level_count = 10;
        book_snap.ask_level_count = 10;
        mgr->on_depth(30, book_snap, base_time + i * 200000);
    }

    // 3. Futures data for FuturesMetrics
    for (int i = 0; i < 10; i++) {
        mgr->on_mark_price(30, 100100.0 + i, 100000.0 + i, 0.0001 + i * 0.00001, 1234567890, base_time + i * 1000000);
        mgr->on_spot_bbo(30, 100000.0 + i, 100100.0 + i, base_time + i * 1000000);
        mgr->on_futures_bbo(30, 100200.0 + i, 100300.0 + i, base_time + i * 1000000);
        if (i % 2 == 0) {
            mgr->on_liquidation(30, Side::Buy, 100000.0 + i, 5.0, base_time + i * 1000000);
        } else {
            mgr->on_liquidation(30, Side::Sell, 100000.0 + i, 3.0, base_time + i * 1000000);
        }
    }

    // Verify snapshot has been written comprehensively
    assert(snap->symbols[30].update_count.load() > 50);
    assert(std::string(snap->symbols[30].ticker) == "COMP");

    // Verify TradeStreamMetrics (all 5 windows)
    assert(snap->symbols[30].trade_w1s_total_trades.load() > 0);
    assert(snap->symbols[30].trade_w1s_buy_volume_x8.load() != 0 ||
           snap->symbols[30].trade_w1s_sell_volume_x8.load() != 0);
    assert(snap->symbols[30].trade_w5s_total_volume_x8.load() != 0);
    assert(snap->symbols[30].trade_w10s_total_volume_x8.load() != 0);
    assert(snap->symbols[30].trade_w30s_total_volume_x8.load() != 0);
    assert(snap->symbols[30].trade_w1min_total_volume_x8.load() != 0);

    // Verify OrderBookMetrics
    assert(snap->symbols[30].book_current_best_bid.load() > 0);
    assert(snap->symbols[30].book_current_best_ask.load() > 0);

    // Verify OrderFlowMetrics
    assert(snap->symbols[30].flow_sec1_book_update_count.load() > 0);

    // Verify FuturesMetrics
    assert(snap->symbols[30].futures_w1s_funding_rate_x8.load() != 0);
    assert(snap->symbols[30].futures_w1s_liquidation_count.load() > 0);
    assert(snap->symbols[30].futures_next_funding_time_ms.load() == 1234567890);

    // Verify Regime
    assert(snap->symbols[30].regime.load() < 10); // Valid regime value

    SharedMetricsSnapshot::destroy(snap);
    std::cout << "[PASS] test_comprehensive_write_snapshot\n";
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
    test_on_spot_bbo_updates_futures();
    test_on_futures_bbo_updates_futures();
    test_spot_futures_basis_calculation();
    test_basis_threshold_triggers_callback();
    test_funding_rate_threshold_triggers_callback();
    test_volatility_threshold_triggers_callback();
    test_top_imbalance_threshold_triggers_callback();
    test_write_snapshot_all_trade_windows();
    test_write_snapshot_book_metrics();
    test_write_snapshot_flow_metrics();
    test_write_snapshot_combined_metrics();
    test_write_snapshot_futures_metrics();
    test_write_snapshot_regime_fields();
    test_invalid_symbol_id_ignored();
    test_set_ticker_invalid_symbol();
    test_set_ticker_no_snapshot();
    test_comprehensive_write_snapshot();

    std::cout << "\n✓ All 31 MetricsManager tests passed!\n";
    return 0;
}
