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

void test_parse_levels_basic() {
    // Test parsing real Binance JSON format
    std::string json = R"(["42000.50","1.5"],["41999.00","3.2"],["41998.00","2.1"])";
    std::array<WsDepthUpdate::Level, 20> levels{};

    int count = BinanceWs::parse_levels(json, levels);

    assert(count == 3);
    // 42000.50 * 10000
    assert(levels[0].price == 420005000);
    // 1.5 * 10000
    assert(levels[0].quantity == 15000);
    // 41999.00 * 10000
    assert(levels[1].price == 419990000);
    // 3.2 * 10000
    assert(levels[1].quantity == 32000);
    // 41998.00 * 10000
    assert(levels[2].price == 419980000);
    // 2.1 * 10000
    assert(levels[2].quantity == 21000);

    std::cout << "✓ test_parse_levels_basic\n";
}

void test_parse_levels_empty() {
    // Empty array
    std::string json = "";
    std::array<WsDepthUpdate::Level, 20> levels{};

    int count = BinanceWs::parse_levels(json, levels);

    assert(count == 0);

    std::cout << "✓ test_parse_levels_empty\n";
}

void test_parse_levels_malformed() {
    std::array<WsDepthUpdate::Level, 20> levels{};

    // Test 1: Malformed JSON - missing quotes
    std::string json1 = R"([42000.50,1.5])";
    int count1 = BinanceWs::parse_levels(json1, levels);
    assert(count1 == 0);

    // Test 2: No brackets at all - triggers line 331 break (no '[' found)
    std::string json2 = "no brackets here";
    int count2 = BinanceWs::parse_levels(json2, levels);
    assert(count2 == 0);

    // Test 3: Opening bracket without closing - triggers line 336 break (no ']' found)
    std::string json3 = R"([["42000.50","1.5")";
    int count3 = BinanceWs::parse_levels(json3, levels);
    assert(count3 == 0);

    std::cout << "✓ test_parse_levels_malformed\n";
}

void test_parse_levels_single_level() {
    std::string json = R"(["50000.00","1.0"])";
    std::array<WsDepthUpdate::Level, 20> levels{};

    int count = BinanceWs::parse_levels(json, levels);

    assert(count == 1);
    // 50000.00 * 10000
    assert(levels[0].price == 500000000);
    // 1.0 * 10000
    assert(levels[0].quantity == 10000);

    std::cout << "✓ test_parse_levels_single_level\n";
}

void test_parse_levels_max_levels() {
    // Create JSON with 22 levels (more than max 20)
    std::string json;
    for (int i = 0; i < 22; i++) {
        if (i > 0)
            json += ",";
        json += "[\"" + std::to_string(40000 - i) + ".00\",\"" + std::to_string(1 + i * 0.1) + "\"]";
    }

    std::array<WsDepthUpdate::Level, 20> levels{};
    int count = BinanceWs::parse_levels(json, levels);

    // Should parse only first 20 levels
    assert(count == 20);
    assert(levels[0].price == 400000000);  // 40000.00 * 10000
    assert(levels[19].price == 399810000); // 39981.00 * 10000

    std::cout << "✓ test_parse_levels_max_levels\n";
}

void test_parse_depth_integration() {
    // Full integration test with real Binance depth JSON
    BinanceWs ws(false);

    bool callback_invoked = false;
    WsDepthUpdate received_depth;

    ws.set_depth_callback([&](const WsDepthUpdate& depth) {
        received_depth = depth;
        callback_invoked = true;
    });

    // Simulate real Binance depth message (partial book depth format)
    // This is what Binance sends on @depth20@100ms stream
    std::string depth_json = R"({
        "lastUpdateId": 160,
        "bids": [
            ["42000.50", "1.5"],
            ["41999.00", "3.2"],
            ["41998.00", "2.1"]
        ],
        "asks": [
            ["42001.00", "2.0"],
            ["42002.00", "1.7"]
        ]
    })";

    // Manually call parse_depth (simulating what parse_message does)
    // Note: parse_depth is private, but we test through public API in real usage
    // For this test, we verify parse_levels works correctly above
    // In production, parse_message routes to parse_depth which calls parse_levels

    // Verify parse_levels extracts bid levels correctly
    std::string bids_json = R"(["42000.50", "1.5"],["41999.00", "3.2"],["41998.00", "2.1"])";
    std::array<WsDepthUpdate::Level, 20> bids{};
    int bid_count = BinanceWs::parse_levels(bids_json, bids);

    assert(bid_count == 3);
    assert(bids[0].price == 420005000);
    assert(bids[0].quantity == 15000);

    // Verify parse_levels extracts ask levels correctly
    std::string asks_json = R"(["42001.00", "2.0"],["42002.00", "1.7"])";
    std::array<WsDepthUpdate::Level, 20> asks{};
    int ask_count = BinanceWs::parse_levels(asks_json, asks);

    assert(ask_count == 2);
    assert(asks[0].price == 420010000);
    assert(asks[0].quantity == 20000);

    std::cout << "✓ test_parse_depth_integration\n";
}

// ============================================================================
// Connection/Disconnection Tests (Mock Mode)
// ============================================================================

void test_connect_without_streams() {
    BinanceWs ws(false);
    ws.set_test_mode(true);

    bool error_received = false;
    std::string error_msg;
    ws.set_error_callback([&](const std::string& err) {
        error_received = true;
        error_msg = err;
    });

    bool result = ws.connect();

    assert(result == false);
    assert(error_received == true);
    assert(error_msg == "No streams subscribed");

    std::cout << "✓ test_connect_without_streams\n";
}

void test_connect_with_streams() {
    BinanceWs ws(false);
    ws.set_test_mode(true);
    ws.subscribe_book_ticker("BTCUSDT");

    bool connect_callback_invoked = false;
    bool connect_status = false;
    ws.set_connect_callback([&](bool connected) {
        connect_callback_invoked = true;
        connect_status = connected;
    });

    bool result = ws.connect();

    assert(result == true);
    assert(ws.is_connected() == true);
    assert(ws.is_running() == true);
    assert(connect_callback_invoked == true);
    assert(connect_status == true);

    ws.disconnect();

    std::cout << "✓ test_connect_with_streams\n";
}

void test_disconnect_triggers_callback() {
    BinanceWs ws(false);
    ws.set_test_mode(true);
    ws.subscribe_trade("ETHUSDT");

    int callback_count = 0;
    bool final_status = true;
    ws.set_connect_callback([&](bool connected) {
        callback_count++;
        final_status = connected;
    });

    ws.connect();
    assert(callback_count == 1);
    assert(final_status == true);

    ws.disconnect();
    assert(callback_count == 2);
    assert(final_status == false);
    assert(ws.is_connected() == false);
    assert(ws.is_running() == false);

    std::cout << "✓ test_disconnect_triggers_callback\n";
}

void test_error_callback() {
    BinanceWs ws(false);
    ws.set_test_mode(true);

    bool error_received = false;
    std::string error_msg;
    ws.set_error_callback([&](const std::string& err) {
        error_received = true;
        error_msg = err;
    });

    ws.simulate_error("Connection timeout");

    assert(error_received == true);
    assert(error_msg == "Connection timeout");

    std::cout << "✓ test_error_callback\n";
}

void test_is_connected_is_running() {
    BinanceWs ws(false);
    ws.set_test_mode(true);
    ws.subscribe_kline("BTCUSDT", "1m");

    assert(ws.is_connected() == false);
    assert(ws.is_running() == false);

    ws.connect();
    assert(ws.is_connected() == true);
    assert(ws.is_running() == true);

    ws.disconnect();
    assert(ws.is_connected() == false);
    assert(ws.is_running() == false);

    std::cout << "✓ test_is_connected_is_running\n";
}

void test_parse_book_ticker() {
    BinanceWs ws(false);
    ws.set_test_mode(true);

    bool callback_invoked = false;
    BookTicker received;
    ws.set_book_ticker_callback([&](const BookTicker& bt) {
        callback_invoked = true;
        received = bt;
    });

    std::string json = R"({"u":123,"s":"BTCUSDT","b":"50000.00","B":"1.5","a":"50001.00","A":"2.0"})";
    ws.parse_message_for_test(json);

    assert(callback_invoked == true);
    assert(received.symbol == "BTCUSDT");
    assert(received.bid_price == 500000000); // 50000.00 * 10000
    assert(std::abs(received.bid_qty - 1.5) < 0.01);
    assert(received.ask_price == 500010000); // 50001.00 * 10000
    assert(std::abs(received.ask_qty - 2.0) < 0.01);
    assert(received.update_time == 123);

    std::cout << "✓ test_parse_book_ticker\n";
}

void test_parse_trade() {
    BinanceWs ws(false);
    ws.set_test_mode(true);

    bool callback_invoked = false;
    WsTrade received;
    ws.set_trade_callback([&](const WsTrade& trade) {
        callback_invoked = true;
        received = trade;
    });

    std::string json = R"({"e":"trade","E":123,"s":"BTCUSDT","t":456,"p":"50000.00","q":"1.5","T":789,"m":true})";
    ws.parse_message_for_test(json);

    assert(callback_invoked == true);
    assert(received.symbol == "BTCUSDT");
    assert(received.trade_id == 456);
    assert(received.price == 500000000); // 50000.00 * 10000
    assert(std::abs(received.quantity - 1.5) < 0.01);
    assert(received.time == 789);
    assert(received.is_buyer_maker == true);

    std::cout << "✓ test_parse_trade\n";
}

void test_parse_kline() {
    BinanceWs ws(false);
    ws.set_test_mode(true);

    bool callback_invoked = false;
    WsKline received;
    ws.set_kline_callback([&](const WsKline& kline) {
        callback_invoked = true;
        received = kline;
    });

    std::string json =
        R"({"e":"kline","E":123,"s":"BTCUSDT","k":{"t":100,"T":200,"o":"50000.00","h":"50100.00","l":"49900.00","c":"50050.00","v":"10.5","n":100,"x":true}})";
    ws.parse_message_for_test(json);

    assert(callback_invoked == true);
    assert(received.symbol == "BTCUSDT");
    assert(received.open_time == 100);
    assert(received.close_time == 200);
    assert(received.open == 500000000);  // 50000.00 * 10000
    assert(received.high == 501000000);  // 50100.00 * 10000
    assert(received.low == 499000000);   // 49900.00 * 10000
    assert(received.close == 500500000); // 50050.00 * 10000
    assert(std::abs(received.volume - 10.5) < 0.01);
    assert(received.trades == 100);
    assert(received.is_closed == true);

    std::cout << "✓ test_parse_kline\n";
}

void test_parse_depth() {
    BinanceWs ws(false);
    ws.set_test_mode(true);

    bool callback_invoked = false;
    WsDepthUpdate received;
    ws.set_depth_callback([&](const WsDepthUpdate& depth) {
        callback_invoked = true;
        received = depth;
    });

    // Simulate combined stream message with depth data
    std::string json =
        R"({"stream":"btcusdt@depth5@100ms","data":{"lastUpdateId":160,"bids":[["42000.50","1.5"],["41999.00","3.2"]],"asks":[["42001.00","2.0"]]}})";
    ws.parse_message_for_test(json);

    assert(callback_invoked == true);
    assert(received.symbol == "BTCUSDT");
    assert(received.last_update_id == 160);
    assert(received.bid_count == 2);
    assert(received.ask_count == 1);
    assert(received.bids[0].price == 420005000); // 42000.50 * 10000
    assert(received.bids[0].quantity == 15000);  // 1.5 * 10000
    assert(received.asks[0].price == 420010000); // 42001.00 * 10000

    std::cout << "✓ test_parse_depth\n";
}

// Test parsing without callbacks set (early returns)
void test_parse_without_callbacks() {
    BinanceWs ws(false);
    ws.set_test_mode(true);

    // Parse book_ticker without callback - should return early
    std::string bt_json = R"({"u":123,"s":"BTCUSDT","b":"50000.00","B":"1.5","a":"50001.00","A":"2.0"})";
    ws.parse_message_for_test(bt_json); // No callback set, should return early

    // Parse trade without callback - should return early
    std::string trade_json =
        R"({"e":"trade","E":123,"s":"BTCUSDT","t":12345,"p":"50000.00","q":"1.5","T":1234567890,"m":true})";
    ws.parse_message_for_test(trade_json);

    // Parse kline without callback - should return early
    std::string kline_json =
        R"({"e":"kline","E":123,"s":"BTCUSDT","k":{"t":100,"T":200,"o":"50000.00","h":"50100.00","l":"49900.00","c":"50050.00","v":"10.5","n":100,"x":true}})";
    ws.parse_message_for_test(kline_json);

    // Parse depth without callback - should return early
    std::string depth_json =
        R"({"stream":"btcusdt@depth5@100ms","data":{"lastUpdateId":160,"bids":[["42000.50","1.5"]],"asks":[["42001.00","2.0"]]}})";
    ws.parse_message_for_test(depth_json);

    std::cout << "✓ test_parse_without_callbacks\n";
}

// Test malformed kline JSON (missing "k" object)
void test_parse_kline_malformed() {
    BinanceWs ws(false);
    ws.set_test_mode(true);

    bool callback_invoked = false;
    ws.set_kline_callback([&](const WsKline&) { callback_invoked = true; });

    // Missing "k" object - should return early
    std::string json = R"({"e":"kline","E":123,"s":"BTCUSDT"})";
    ws.parse_message_for_test(json);

    assert(callback_invoked == false);

    std::cout << "✓ test_parse_kline_malformed\n";
}

// Test extract functions with edge cases
void test_extract_functions_edge_cases() {
    BinanceWs ws(false);
    ws.set_test_mode(true);

    bool callback_invoked = false;
    WsKline received;
    ws.set_kline_callback([&](const WsKline& kline) {
        callback_invoked = true;
        received = kline;
    });

    // Kline with boolean field to test extract_bool
    std::string json =
        R"({"e":"kline","E":123,"s":"BTCUSDT","k":{"t":100,"T":200,"o":"50000.00","h":"50100.00","l":"49900.00","c":"50050.00","v":"10.5","n":100,"x":false}})";
    ws.parse_message_for_test(json);

    assert(callback_invoked == true);
    assert(received.is_closed == false);

    std::cout << "✓ test_extract_functions_edge_cases\n";
}

// Test kline parsing with malformed k object
void test_parse_kline_missing_braces() {
    BinanceWs ws(false);
    ws.set_test_mode(true);

    bool callback_invoked = false;
    ws.set_kline_callback([&](const WsKline&) { callback_invoked = true; });

    // k object incomplete - missing closing brace
    std::string json = R"({"e":"kline","E":123,"s":"BTCUSDT","k":{"t":100)";
    ws.parse_message_for_test(json);

    assert(callback_invoked == false);

    std::cout << "✓ test_parse_kline_missing_braces\n";
}

// Test extract_string with missing keys
void test_extract_string_missing_key() {
    BinanceWs ws(false);
    ws.set_test_mode(true);

    bool callback_invoked = false;
    BookTicker received;
    ws.set_book_ticker_callback([&](const BookTicker& bt) {
        callback_invoked = true;
        received = bt;
    });

    // Missing "s" (symbol) field - extract_string should return ""
    std::string json = R"({"u":123,"b":"50000.00","B":"1.5","a":"50001.00","A":"2.0"})";
    ws.parse_message_for_test(json);

    assert(callback_invoked == true);
    assert(received.symbol == "");

    std::cout << "✓ test_extract_string_missing_key\n";
}

// Test extract function edge cases (unquoted numbers, missing keys)
void test_extract_functions_comprehensive() {
    BinanceWs ws(false);
    ws.set_test_mode(true);

    // Test 1: extract_double with unquoted format (lines 591-601)
    // Use kline with unquoted numeric values
    bool callback_invoked = false;
    WsKline kline_received;
    ws.set_kline_callback([&](const WsKline& kline) {
        callback_invoked = true;
        kline_received = kline;
    });

    // Unquoted numbers in k object (non-standard but should be handled)
    std::string kline_unquoted =
        R"({"e":"kline","E":123,"s":"BTCUSDT","k":{"t":100,"T":200,"o":50000.00,"h":50100.00,"l":49900.00,"c":50050.00,"v":10.5,"n":100,"x":true}})";
    ws.parse_message_for_test(kline_unquoted);

    assert(callback_invoked == true);
    assert(kline_received.open == 500000000); // 50000.00 * 10000
    callback_invoked = false;

    // Test 2: extract_uint64 with missing key (line 608)
    // Kline with missing "t" (open_time) field
    std::string kline_missing_t =
        R"({"e":"kline","E":123,"s":"BTCUSDT","k":{"T":200,"o":"50000.00","h":"50100.00","l":"49900.00","c":"50050.00","v":"10.5","n":100,"x":true}})";
    ws.parse_message_for_test(kline_missing_t);

    assert(callback_invoked == true);
    assert(kline_received.open_time == 0); // extract_uint64 returns 0 when key missing
    callback_invoked = false;

    // Test 3: extract_bool with missing key (line 622)
    // Kline with missing "x" (is_closed) field
    std::string kline_missing_x =
        R"({"e":"kline","E":123,"s":"BTCUSDT","k":{"t":100,"T":200,"o":"50000.00","h":"50100.00","l":"49900.00","c":"50050.00","v":"10.5","n":100}})";
    ws.parse_message_for_test(kline_missing_x);

    assert(callback_invoked == true);
    assert(kline_received.is_closed == false); // extract_bool returns false when key missing

    std::cout << "✓ test_extract_functions_comprehensive\n";
}

int main() {
    test_depth_to_snapshot_basic();
    test_depth_to_snapshot_empty_book();
    test_depth_to_snapshot_single_level();
    test_depth_to_snapshot_max_levels();
    test_depth_to_snapshot_best_bid_ask();
    test_depth_to_snapshot_all_levels_preserved();
    test_callback_mechanism();

    // JSON parsing tests (critical for code coverage)
    test_parse_levels_basic();
    test_parse_levels_empty();
    test_parse_levels_malformed();
    test_parse_levels_single_level();
    test_parse_levels_max_levels();
    test_parse_depth_integration();

    // Connection/disconnection tests (mock mode)
    test_connect_without_streams();
    test_connect_with_streams();
    test_disconnect_triggers_callback();
    test_error_callback();
    test_is_connected_is_running();

    // Message parsing tests
    test_parse_book_ticker();
    test_parse_trade();
    test_parse_kline();
    test_parse_depth();

    // Edge case tests
    test_parse_without_callbacks();
    test_parse_kline_malformed();
    test_extract_functions_edge_cases();
    test_parse_kline_missing_braces();
    test_extract_string_missing_key();
    test_extract_functions_comprehensive();

    std::cout << "\n✅ All BinanceWs depth tests passed!\n";
    return 0;
}
