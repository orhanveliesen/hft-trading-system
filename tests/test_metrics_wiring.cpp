#include "../include/config/defaults.hpp"
#include "../include/exchange/binance_ws.hpp"
#include "../include/metrics/combined_metrics.hpp"
#include "../include/metrics/order_book_metrics.hpp"
#include "../include/metrics/order_flow_metrics.hpp"
#include "../include/metrics/trade_stream_metrics.hpp"

#include <cassert>
#include <iostream>

using namespace hft;
using namespace hft::exchange;

// Note: Window enums are different for each metrics class
// TradeStreamMetrics uses TradeWindow
// OrderFlowMetrics uses Window
// CombinedMetrics uses its own Window enum

void test_wiring_compilation() {
    // WsTrade → TradeStreamMetrics
    TradeStreamMetrics tsm;
    WsTrade t;
    t.price = 420000; // Already Price type
    t.quantity = 1.5;
    t.is_buyer_maker = false;
    t.time = 1000;

    Quantity qty = static_cast<Quantity>(t.quantity * config::scaling::QUANTITY_SCALE);
    bool is_buy = !t.is_buyer_maker;
    uint64_t ts_us = t.time * 1000;

    tsm.on_trade(t.price, qty, is_buy, ts_us);

    // Verify metrics can be retrieved
    auto tsm_metrics = tsm.get_metrics(TradeWindow::W1s);
    assert(tsm_metrics.trade_count > 0);

    // WsDepthUpdate → BookSnapshot → OrderBookMetrics
    OrderBookMetrics obm;
    WsDepthUpdate d;
    d.symbol = "BTCUSDT";
    d.bid_count = 1;
    d.bids[0] = {420000, 15000};
    d.ask_count = 1;
    d.asks[0] = {420100, 12000};

    auto snap = BinanceWs::depth_to_snapshot(d);
    obm.on_depth_snapshot(snap, 1000000);

    // Verify snapshot conversion
    assert(snap.best_bid == 420000);
    assert(snap.best_ask == 420100);
    assert(snap.best_bid_qty == 15000);
    assert(snap.best_ask_qty == 12000);

    // Verify metrics
    auto obm_metrics = obm.get_metrics();
    assert(obm_metrics.best_bid == 420000);
    assert(obm_metrics.best_ask == 420100);

    // OrderFlowMetrics both overloads
    OrderFlowMetrics<20> ofm;
    ofm.on_trade(420000, 15000, 1000000);
    ofm.on_depth_snapshot(snap, 1000000);

    // Verify metrics can be retrieved
    auto ofm_metrics = ofm.get_metrics(Window::SEC_1);

    // CombinedMetrics from refs
    CombinedMetrics cm(tsm, obm);
    cm.update(1000000);
    auto m = cm.get_metrics(CombinedMetrics::Window::SEC_1);

    std::cout << "✓ test_wiring_compilation\n";
}

void test_depth_to_snapshot_conversion() {
    // Test with multiple levels
    WsDepthUpdate depth;
    depth.symbol = "ETHUSDT";
    depth.bid_count = 5;
    depth.ask_count = 5;

    // Set bid levels
    for (int i = 0; i < 5; i++) {
        depth.bids[i].price = 200000 - i * 100; // Descending prices
        depth.bids[i].quantity = 10000 + i * 1000;
    }

    // Set ask levels
    for (int i = 0; i < 5; i++) {
        depth.asks[i].price = 200100 + i * 100; // Ascending prices
        depth.asks[i].quantity = 9000 + i * 1000;
    }

    hft::BookSnapshot snapshot = BinanceWs::depth_to_snapshot(depth);

    // Verify level counts
    assert(snapshot.bid_level_count == 5);
    assert(snapshot.ask_level_count == 5);

    // Verify all bid levels preserved
    for (int i = 0; i < 5; i++) {
        assert(snapshot.bid_levels[i].price == depth.bids[i].price);
        assert(snapshot.bid_levels[i].quantity == depth.bids[i].quantity);
    }

    // Verify all ask levels preserved
    for (int i = 0; i < 5; i++) {
        assert(snapshot.ask_levels[i].price == depth.asks[i].price);
        assert(snapshot.ask_levels[i].quantity == depth.asks[i].quantity);
    }

    std::cout << "✓ test_depth_to_snapshot_conversion\n";
}

void test_trade_metrics_integration() {
    TradeStreamMetrics tsm;

    // Simulate multiple trades
    for (int i = 0; i < 10; i++) {
        Price price = 50000 + i * 10;
        Quantity qty = 1000 + i * 100;
        bool is_buy = (i % 2 == 0);
        uint64_t ts = 1000000 + i * 100000; // 100ms apart

        tsm.on_trade(price, qty, is_buy, ts);
    }

    auto metrics = tsm.get_metrics(TradeWindow::W1s);

    // Should have 10 trades
    assert(metrics.trade_count == 10);

    // Should have both buy and sell volume
    assert(metrics.buy_volume > 0);
    assert(metrics.sell_volume > 0);

    std::cout << "✓ test_trade_metrics_integration\n";
}

void test_order_flow_metrics_integration() {
    OrderFlowMetrics<20> ofm;

    // Create initial snapshot
    WsDepthUpdate depth1;
    depth1.bid_count = 2;
    depth1.bids[0] = {100000, 50000};
    depth1.bids[1] = {99900, 30000};
    depth1.ask_count = 2;
    depth1.asks[0] = {100100, 40000};
    depth1.asks[1] = {100200, 25000};

    auto snap1 = BinanceWs::depth_to_snapshot(depth1);
    ofm.on_depth_snapshot(snap1, 1000000);

    // Simulate a trade
    ofm.on_trade(100000, 10000, 1100000);

    // Create updated snapshot (bid reduced after trade)
    WsDepthUpdate depth2;
    depth2.bid_count = 2;
    depth2.bids[0] = {100000, 40000}; // Reduced from 50000
    depth2.bids[1] = {99900, 30000};
    depth2.ask_count = 2;
    depth2.asks[0] = {100100, 40000};
    depth2.asks[1] = {100200, 25000};

    auto snap2 = BinanceWs::depth_to_snapshot(depth2);
    ofm.on_depth_snapshot(snap2, 1200000);

    auto metrics = ofm.get_metrics(Window::SEC_1);

    // Should detect volume removed on bid side
    assert(metrics.bid_volume_removed > 0);

    std::cout << "✓ test_order_flow_metrics_integration\n";
}

void test_combined_metrics_integration() {
    TradeStreamMetrics tsm;
    OrderBookMetrics obm;
    CombinedMetrics cm(tsm, obm);

    // Add some trade data
    tsm.on_trade(50000, 10000, true, 1000000);
    tsm.on_trade(50010, 15000, false, 1100000);

    // Add book snapshot
    WsDepthUpdate depth;
    depth.bid_count = 1;
    depth.bids[0] = {50000, 50000};
    depth.ask_count = 1;
    depth.asks[0] = {50010, 45000};

    auto snap = BinanceWs::depth_to_snapshot(depth);
    obm.on_depth_snapshot(snap, 1100000);

    // Update combined metrics
    cm.update(1100000);

    auto metrics = cm.get_metrics(CombinedMetrics::Window::SEC_1);

    // Should have combined data from both sources
    assert(metrics.trade_count > 0);
    assert(metrics.spread > 0);

    std::cout << "✓ test_combined_metrics_integration\n";
}

int main() {
    test_wiring_compilation();
    test_depth_to_snapshot_conversion();
    test_trade_metrics_integration();
    test_order_flow_metrics_integration();
    test_combined_metrics_integration();

    std::cout << "All metrics wiring tests passed!\n";
    return 0;
}
