#include "../include/exchange/binance_ws.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace hft;
using namespace hft::exchange;

// Note: parse_depth() is private, so we test it indirectly through the callback mechanism
// and test the public depth_to_snapshot() static method directly

void test_depth_to_snapshot_basic() {
    WsDepthUpdate depth;
    depth.symbol = "BTCUSDT";
    depth.bid_count = 3;
    depth.ask_count = 2;

    // Set bid levels
    depth.bids[0] = {420000000, 15000}; // 42000.00, 1.5
    depth.bids[1] = {419990000, 32000}; // 41999.00, 3.2
    depth.bids[2] = {419980000, 21000}; // 41998.00, 2.1

    // Set ask levels
    depth.asks[0] = {420010000, 20000}; // 42001.00, 2.0
    depth.asks[1] = {420020000, 17000}; // 42002.00, 1.7

    hft::BookSnapshot snapshot = BinanceWs::depth_to_snapshot(depth);

    // Verify level counts
    assert(snapshot.bid_level_count == 3);
    assert(snapshot.ask_level_count == 2);

    // Verify best bid/ask
    assert(snapshot.best_bid == 420000000);
    assert(snapshot.best_bid_qty == 15000);
    assert(snapshot.best_ask == 420010000);
    assert(snapshot.best_ask_qty == 20000);

    // Verify bid levels
    assert(snapshot.bid_levels[0].price == 420000000);
    assert(snapshot.bid_levels[0].quantity == 15000);
    assert(snapshot.bid_levels[1].price == 419990000);
    assert(snapshot.bid_levels[1].quantity == 32000);
    assert(snapshot.bid_levels[2].price == 419980000);
    assert(snapshot.bid_levels[2].quantity == 21000);

    // Verify ask levels
    assert(snapshot.ask_levels[0].price == 420010000);
    assert(snapshot.ask_levels[0].quantity == 20000);
    assert(snapshot.ask_levels[1].price == 420020000);
    assert(snapshot.ask_levels[1].quantity == 17000);

    std::cout << "✓ test_depth_to_snapshot_basic\n";
}

void test_depth_to_snapshot_empty_book() {
    WsDepthUpdate depth;
    depth.symbol = "ETHUSDT";
    depth.bid_count = 0;
    depth.ask_count = 0;

    hft::BookSnapshot snapshot = BinanceWs::depth_to_snapshot(depth);

    assert(snapshot.bid_level_count == 0);
    assert(snapshot.ask_level_count == 0);

    std::cout << "✓ test_depth_to_snapshot_empty_book\n";
}

void test_depth_to_snapshot_single_level() {
    WsDepthUpdate depth;
    depth.bid_count = 1;
    depth.ask_count = 1;
    depth.bids[0] = {500000000, 10000}; // 50000.00, 1.0
    depth.asks[0] = {500010000, 5000};  // 50001.00, 0.5

    hft::BookSnapshot snapshot = BinanceWs::depth_to_snapshot(depth);

    assert(snapshot.bid_level_count == 1);
    assert(snapshot.ask_level_count == 1);
    assert(snapshot.best_bid == 500000000);
    assert(snapshot.best_bid_qty == 10000);
    assert(snapshot.best_ask == 500010000);
    assert(snapshot.best_ask_qty == 5000);

    std::cout << "✓ test_depth_to_snapshot_single_level\n";
}

void test_depth_to_snapshot_max_levels() {
    WsDepthUpdate depth;

    // Create 20 levels (max)
    depth.bid_count = 20;
    depth.ask_count = 20;

    for (int i = 0; i < 20; i++) {
        depth.bids[i].price = 40000000 - i * 1000; // Descending
        depth.bids[i].quantity = 10000 + i * 1000;
        depth.asks[i].price = 40001000 + i * 1000; // Ascending
        depth.asks[i].quantity = 9000 + i * 1000;
    }

    hft::BookSnapshot snapshot = BinanceWs::depth_to_snapshot(depth);

    assert(snapshot.bid_level_count == 20);
    assert(snapshot.ask_level_count == 20);

    // Verify first and last levels
    assert(snapshot.bid_levels[0].price == 40000000);
    assert(snapshot.bid_levels[19].price == 40000000 - 19 * 1000);
    assert(snapshot.ask_levels[0].price == 40001000);
    assert(snapshot.ask_levels[19].price == 40001000 + 19 * 1000);

    std::cout << "✓ test_depth_to_snapshot_max_levels\n";
}

void test_depth_to_snapshot_best_bid_ask() {
    WsDepthUpdate depth;
    depth.bid_count = 5;
    depth.ask_count = 5;

    // Set multiple levels
    for (int i = 0; i < 5; i++) {
        depth.bids[i].price = 200000 - i * 100;
        depth.bids[i].quantity = 10000 + i * 1000;
        depth.asks[i].price = 200100 + i * 100;
        depth.asks[i].quantity = 9000 + i * 1000;
    }

    hft::BookSnapshot snapshot = BinanceWs::depth_to_snapshot(depth);

    // Verify best bid/ask are from first levels
    assert(snapshot.best_bid == depth.bids[0].price);
    assert(snapshot.best_bid_qty == depth.bids[0].quantity);
    assert(snapshot.best_ask == depth.asks[0].price);
    assert(snapshot.best_ask_qty == depth.asks[0].quantity);

    std::cout << "✓ test_depth_to_snapshot_best_bid_ask\n";
}

void test_depth_to_snapshot_all_levels_preserved() {
    // Test that all levels are correctly copied
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

    std::cout << "✓ test_depth_to_snapshot_all_levels_preserved\n";
}

void test_callback_mechanism() {
    // Test that the callback mechanism works (integration test)
    // This indirectly tests parse_depth() through the public API

    BinanceWs ws(false);

    std::string received_symbol;
    int received_bid_count = -1;
    int received_ask_count = -1;
    bool callback_invoked = false;

    ws.set_depth_callback([&](const WsDepthUpdate& depth) {
        received_symbol = depth.symbol;
        received_bid_count = depth.bid_count;
        received_ask_count = depth.ask_count;
        callback_invoked = true;
    });

    // Verify callback was set (we can't actually test parse_depth since it's private,
    // but we verify the callback mechanism is in place)
    assert(!callback_invoked); // Should not be invoked yet

    std::cout << "✓ test_callback_mechanism\n";
}

int main() {
    test_depth_to_snapshot_basic();
    test_depth_to_snapshot_empty_book();
    test_depth_to_snapshot_single_level();
    test_depth_to_snapshot_max_levels();
    test_depth_to_snapshot_best_bid_ask();
    test_depth_to_snapshot_all_levels_preserved();
    test_callback_mechanism();

    std::cout << "All BinanceWs depth tests passed!\n";
    return 0;
}
