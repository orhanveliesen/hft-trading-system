#include "../include/exchange/binance_futures_ws.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>

using namespace hft;
using namespace hft::exchange;

// Mock BinanceFuturesWs for testing (inheritance-based pattern)
class MockBinanceFuturesWs : public BinanceFuturesWs {
public:
    explicit MockBinanceFuturesWs(bool use_testnet = false) : BinanceFuturesWs(use_testnet) {}

    bool connect() override {
        if (!validate_streams()) {
            return false;
        }
        running_ = true;
        connected_ = true;
        trigger_connect(true);
        return true;
    }

    void disconnect() override {
        running_ = false;
        connected_ = false;
        trigger_connect(false);
    }

    // Test helpers - expose protected methods
    void parse_for_test(const std::string& json) { parse_message(json); }

    void simulate_error_for_test(const std::string& error) { trigger_error(error); }
};

// ============================================================================
// Stream Path Building Tests
// ============================================================================

void test_single_stream_path() {
    std::vector<std::string> streams = {"btcusdt@markPrice@1s"};
    std::string path = BinanceFuturesWs::build_stream_path(streams);
    assert(path == "/ws/btcusdt@markPrice@1s");

    std::cout << "✓ test_single_stream_path\n";
}

void test_combined_stream_path() {
    std::vector<std::string> streams = {"btcusdt@markPrice@1s", "ethusdt@forceOrder", "bnbusdt@bookTicker"};
    std::string path = BinanceFuturesWs::build_stream_path(streams);
    assert(path.find("/stream?streams=") == 0);
    assert(path.find("btcusdt@markPrice@1s") != std::string::npos);
    assert(path.find("ethusdt@forceOrder") != std::string::npos);
    assert(path.find("bnbusdt@bookTicker") != std::string::npos);

    std::cout << "✓ test_combined_stream_path\n";
}

// ============================================================================
// JSON Parsing Tests (Static Methods)
// ============================================================================

void test_parse_mark_price_update() {
    std::string json = R"({
        "e":"markPriceUpdate",
        "E":1700000000000,
        "s":"BTCUSDT",
        "p":"42000.50",
        "P":"41950.00",
        "i":"",
        "r":"0.00010000",
        "T":1700028800000
    })";

    MarkPriceUpdate mp = BinanceFuturesWs::parse_mark_price_update(json);

    assert(mp.symbol == "BTCUSDT");
    assert(std::abs(mp.mark_price - 42000.50) < 0.01);
    assert(std::abs(mp.index_price - 41950.00) < 0.01);
    assert(std::abs(mp.funding_rate - 0.0001) < 1e-10);
    assert(mp.next_funding_time == 1700028800000);
    assert(mp.event_time == 1700000000000);

    std::cout << "✓ test_parse_mark_price_update\n";
}

void test_parse_liquidation_order() {
    std::string json = R"({
        "e":"forceOrder",
        "E":1700000000000,
        "o":{
            "s":"BTCUSDT",
            "S":"SELL",
            "o":"LIMIT",
            "f":"IOC",
            "q":"0.010",
            "p":"42000.00",
            "ap":"42100.00",
            "X":"FILLED",
            "l":"0.010",
            "z":"0.010",
            "T":1700000000000
        }
    })";

    LiquidationOrder lo = BinanceFuturesWs::parse_liquidation_order(json);

    assert(lo.symbol == "BTCUSDT");
    assert(lo.side == Side::Sell);
    assert(std::abs(lo.price - 42000.00) < 0.01);
    assert(std::abs(lo.quantity - 0.010) < 0.001);
    assert(std::abs(lo.avg_price - 42100.00) < 0.01);
    assert(lo.order_status == "FILLED");
    assert(lo.trade_time == 1700000000000);
    assert(lo.event_time == 1700000000000);

    std::cout << "✓ test_parse_liquidation_order\n";
}

void test_parse_futures_book_ticker() {
    std::string json = R"({
        "e":"bookTicker",
        "u":123,
        "s":"BTCUSDT",
        "b":"42000.50",
        "B":"1.5",
        "a":"42001.00",
        "A":"2.0",
        "T":1700000000000,
        "E":1700000001000
    })";

    FuturesBookTicker fbt = BinanceFuturesWs::parse_futures_book_ticker(json);

    assert(fbt.symbol == "BTCUSDT");
    assert(std::abs(fbt.bid_price - 42000.50) < 0.01);
    assert(std::abs(fbt.bid_qty - 1.5) < 0.01);
    assert(std::abs(fbt.ask_price - 42001.00) < 0.01);
    assert(std::abs(fbt.ask_qty - 2.0) < 0.01);
    assert(fbt.transaction_time == 1700000000000);
    assert(fbt.event_time == 1700000001000);

    std::cout << "✓ test_parse_futures_book_ticker\n";
}

void test_parse_agg_trade() {
    std::string json = R"({
        "e":"aggTrade",
        "E":1700000000000,
        "s":"BTCUSDT",
        "a":123456,
        "p":"42000.50",
        "q":"0.100",
        "f":100,
        "l":105,
        "T":1700000000000,
        "m":true
    })";

    WsAggTrade trade = BinanceFuturesWs::parse_agg_trade(json);

    assert(trade.symbol == "BTCUSDT");
    assert(trade.agg_trade_id == 123456);
    assert(std::abs(trade.price - 42000.50) < 0.01);
    assert(std::abs(trade.quantity - 0.100) < 0.001);
    assert(trade.first_trade_id == 100);
    assert(trade.last_trade_id == 105);
    assert(trade.time == 1700000000000);
    assert(trade.is_buyer_maker == true);

    std::cout << "✓ test_parse_agg_trade\n";
}

void test_parse_kline() {
    std::string json = R"({
        "e":"kline",
        "E":1700000000000,
        "s":"BTCUSDT",
        "k":{
            "t":1700000000000,
            "T":1700000060000,
            "s":"BTCUSDT",
            "i":"1m",
            "o":"42.00",
            "c":"42.10",
            "h":"42.15",
            "l":"41.99",
            "v":"10.5",
            "n":100,
            "x":true
        }
    })";

    WsKline kline = BinanceFuturesWs::parse_kline(json);

    assert(kline.symbol == "BTCUSDT");
    assert(kline.open_time == 1700000000000);
    assert(kline.close_time == 1700000060000);
    assert(kline.open == 420000);  // 42.00 * 10000
    assert(kline.close == 421000); // 42.10 * 10000
    assert(kline.high == 421500);  // 42.15 * 10000
    assert(kline.low == 419900);   // 41.99 * 10000
    assert(std::abs(kline.volume - 10.5) < 0.01);
    assert(kline.trades == 100);
    assert(kline.is_closed == true);

    std::cout << "✓ test_parse_kline\n";
}

// Test invalid JSON handling (should not crash)
void test_parse_invalid_json() {
    std::string invalid_json = "not a json";

    // These should return empty/zero structs without crashing
    MarkPriceUpdate mp = BinanceFuturesWs::parse_mark_price_update(invalid_json);
    assert(mp.symbol.empty());

    LiquidationOrder lo = BinanceFuturesWs::parse_liquidation_order(invalid_json);
    assert(lo.symbol.empty());

    FuturesBookTicker fbt = BinanceFuturesWs::parse_futures_book_ticker(invalid_json);
    assert(fbt.symbol.empty());

    WsAggTrade trade = BinanceFuturesWs::parse_agg_trade(invalid_json);
    assert(trade.symbol.empty());

    WsKline kline = BinanceFuturesWs::parse_kline(invalid_json);
    assert(kline.symbol.empty());

    std::cout << "✓ test_parse_invalid_json\n";
}

// Test missing fields (should return defaults)
void test_parse_missing_fields() {
    std::string json = R"({"e":"markPriceUpdate"})";

    MarkPriceUpdate mp = BinanceFuturesWs::parse_mark_price_update(json);
    assert(mp.symbol.empty());
    assert(mp.mark_price == 0.0);

    std::cout << "✓ test_parse_missing_fields\n";
}

// ============================================================================
// Subscribe Validation Tests
// ============================================================================

void test_subscribe_before_connect() {
    MockBinanceFuturesWs ws;

    // Should succeed - subscribing before connect is allowed
    ws.subscribe_mark_price("BTCUSDT");
    ws.subscribe_liquidation("ETHUSDT");

    std::cout << "✓ test_subscribe_before_connect\n";
}

void test_connect_without_subscriptions() {
    MockBinanceFuturesWs ws;

    // Should fail - no streams subscribed
    bool result = ws.connect();
    assert(result == false);

    std::cout << "✓ test_connect_without_subscriptions\n";
}

// ============================================================================
// Endpoint Selection Tests
// ============================================================================

void test_mainnet_endpoint() {
    BinanceFuturesWs ws(false); // mainnet

    std::string host = ws.get_host();
    int port = ws.get_port();

    assert(host == "fstream.binance.com");
    assert(port == 443);

    std::cout << "✓ test_mainnet_endpoint\n";
}

void test_testnet_endpoint() {
    BinanceFuturesWs ws(true); // testnet

    std::string host = ws.get_host();
    int port = ws.get_port();

    assert(host == "testnet.binancefuture.com");
    assert(port == 443);

    std::cout << "✓ test_testnet_endpoint\n";
}

// ============================================================================
// Subscription Method Tests
// ============================================================================

void test_subscribe_methods() {
    MockBinanceFuturesWs ws;

    // Test all subscribe methods
    ws.subscribe_mark_price("BTCUSDT", "1s");
    ws.subscribe_liquidation("ETHUSDT");
    ws.subscribe_book_ticker("SOLUSDT");
    ws.subscribe_agg_trade("XRPUSDT");
    ws.subscribe_kline("ADAUSDT", "1m");

    // Verify streams were added correctly
    const auto& streams = ws.get_streams();
    assert(streams.size() == 5);
    assert(streams[0] == "btcusdt@markPrice@1s");
    assert(streams[1] == "ethusdt@forceOrder");
    assert(streams[2] == "solusdt@bookTicker");
    assert(streams[3] == "xrpusdt@aggTrade");
    assert(streams[4] == "adausdt@kline_1m");

    std::cout << "✓ test_subscribe_methods\n";
}

// ============================================================================
// Callback Tests
// ============================================================================

void test_callback_setters() {
    MockBinanceFuturesWs ws;

    bool mark_price_called = false;
    bool liquidation_called = false;
    bool book_ticker_called = false;
    bool agg_trade_called = false;
    bool kline_called = false;
    bool error_called = false;
    bool connect_called = false;

    ws.set_mark_price_callback([&](const MarkPriceUpdate&) { mark_price_called = true; });
    ws.set_liquidation_callback([&](const LiquidationOrder&) { liquidation_called = true; });
    ws.set_book_ticker_callback([&](const FuturesBookTicker&) { book_ticker_called = true; });
    ws.set_agg_trade_callback([&](const WsAggTrade&) { agg_trade_called = true; });
    ws.set_kline_callback([&](const WsKline&) { kline_called = true; });
    ws.set_error_callback([&](const std::string&) { error_called = true; });
    ws.set_connect_callback([&](bool) { connect_called = true; });

    // Callbacks set successfully (would be invoked by websocket events)
    assert(!mark_price_called); // Not yet invoked
    assert(!liquidation_called);
    assert(!book_ticker_called);
    assert(!agg_trade_called);
    assert(!kline_called);
    assert(!error_called);
    assert(!connect_called);

    std::cout << "✓ test_callback_setters\n";
}

void test_error_callback_invocation() {
    MockBinanceFuturesWs ws;

    std::string captured_error;
    ws.set_error_callback([&](const std::string& err) { captured_error = err; });

    // Trigger error by connecting without subscriptions
    bool result = ws.connect();

    assert(result == false);
    assert(captured_error == "No streams subscribed");

    std::cout << "✓ test_error_callback_invocation\n";
}

// ============================================================================
// State Tests
// ============================================================================

void test_initial_state() {
    MockBinanceFuturesWs ws;

    assert(!ws.is_connected());
    assert(!ws.is_running());
    assert(ws.get_streams().empty());

    std::cout << "✓ test_initial_state\n";
}

void test_symbol_lowercase_conversion() {
    MockBinanceFuturesWs ws;

    ws.subscribe_mark_price("BTCUSDT");
    ws.subscribe_liquidation("EthUSDT");
    ws.subscribe_book_ticker("solUSDT");

    const auto& streams = ws.get_streams();
    assert(streams[0] == "btcusdt@markPrice@1s");
    assert(streams[1] == "ethusdt@forceOrder");
    assert(streams[2] == "solusdt@bookTicker");

    std::cout << "✓ test_symbol_lowercase_conversion\n";
}

// ============================================================================
// Message Routing Tests (parse_message)
// ============================================================================

void test_parse_message_routes_mark_price() {
    MockBinanceFuturesWs ws;

    bool callback_invoked = false;
    MarkPriceUpdate captured_update;

    ws.set_mark_price_callback([&](const MarkPriceUpdate& mp) {
        callback_invoked = true;
        captured_update = mp;
    });

    std::string json =
        R"({"e":"markPriceUpdate","E":1700000000000,"s":"BTCUSDT","p":"42000.50","P":"41950.00","r":"0.00010000","T":1700028800000})";
    ws.parse_for_test(json);

    assert(callback_invoked);
    assert(captured_update.symbol == "BTCUSDT");
    assert(std::abs(captured_update.mark_price - 42000.50) < 0.01);

    std::cout << "✓ test_parse_message_routes_mark_price\n";
}

void test_parse_message_routes_liquidation() {
    MockBinanceFuturesWs ws;

    bool callback_invoked = false;
    LiquidationOrder captured_order;

    ws.set_liquidation_callback([&](const LiquidationOrder& lo) {
        callback_invoked = true;
        captured_order = lo;
    });

    std::string json =
        R"({"e":"forceOrder","E":1700000000000,"o":{"s":"BTCUSDT","S":"SELL","p":"42000.00","q":"0.010","ap":"42100.00","X":"FILLED","T":1700000000000}})";
    ws.parse_for_test(json);

    assert(callback_invoked);
    assert(captured_order.symbol == "BTCUSDT");
    assert(captured_order.side == Side::Sell);

    std::cout << "✓ test_parse_message_routes_liquidation\n";
}

void test_parse_message_routes_book_ticker() {
    MockBinanceFuturesWs ws;

    bool callback_invoked = false;
    FuturesBookTicker captured_ticker;

    ws.set_book_ticker_callback([&](const FuturesBookTicker& fbt) {
        callback_invoked = true;
        captured_ticker = fbt;
    });

    std::string json =
        R"({"e":"bookTicker","s":"BTCUSDT","b":"42000.50","B":"1.5","a":"42001.00","A":"2.0","T":1700000000000,"E":1700000001000})";
    ws.parse_for_test(json);

    assert(callback_invoked);
    assert(captured_ticker.symbol == "BTCUSDT");
    assert(std::abs(captured_ticker.bid_price - 42000.50) < 0.01);

    std::cout << "✓ test_parse_message_routes_book_ticker\n";
}

void test_parse_message_routes_agg_trade() {
    MockBinanceFuturesWs ws;

    bool callback_invoked = false;
    WsAggTrade captured_trade;

    ws.set_agg_trade_callback([&](const WsAggTrade& trade) {
        callback_invoked = true;
        captured_trade = trade;
    });

    std::string json =
        R"({"e":"aggTrade","E":1700000000000,"s":"BTCUSDT","a":123456,"p":"42000.50","q":"0.100","f":100,"l":105,"T":1700000000000,"m":true})";
    ws.parse_for_test(json);

    assert(callback_invoked);
    assert(captured_trade.symbol == "BTCUSDT");
    assert(captured_trade.agg_trade_id == 123456);

    std::cout << "✓ test_parse_message_routes_agg_trade\n";
}

void test_parse_message_routes_kline() {
    MockBinanceFuturesWs ws;

    bool callback_invoked = false;
    WsKline captured_kline;

    ws.set_kline_callback([&](const WsKline& kline) {
        callback_invoked = true;
        captured_kline = kline;
    });

    std::string json =
        R"({"e":"kline","E":1700000000000,"s":"BTCUSDT","k":{"t":1700000000000,"T":1700000060000,"o":"42.00","c":"42.10","h":"42.15","l":"41.99","v":"10.5","n":100,"x":true}})";
    ws.parse_for_test(json);

    assert(callback_invoked);
    assert(captured_kline.symbol == "BTCUSDT");
    assert(captured_kline.open == 420000);

    std::cout << "✓ test_parse_message_routes_kline\n";
}

void test_parse_message_combined_stream() {
    MockBinanceFuturesWs ws;

    bool callback_invoked = false;
    MarkPriceUpdate captured_update;

    ws.set_mark_price_callback([&](const MarkPriceUpdate& mp) {
        callback_invoked = true;
        captured_update = mp;
    });

    // Combined stream format (multi-stream WebSocket)
    std::string json =
        R"({"stream":"btcusdt@markPrice@1s","data":{"e":"markPriceUpdate","E":1700000000000,"s":"BTCUSDT","p":"42000.50","P":"41950.00","r":"0.00010000","T":1700028800000}})";
    ws.parse_for_test(json);

    assert(callback_invoked);
    assert(captured_update.symbol == "BTCUSDT");

    std::cout << "✓ test_parse_message_combined_stream\n";
}

void test_parse_message_no_callback_set() {
    MockBinanceFuturesWs ws;

    // No callbacks set - should not crash
    std::string json =
        R"({"e":"markPriceUpdate","E":1700000000000,"s":"BTCUSDT","p":"42000.50","P":"41950.00","r":"0.00010000","T":1700028800000})";
    ws.parse_for_test(json);

    std::cout << "✓ test_parse_message_no_callback_set\n";
}

// ============================================================================
// JSON Extractor Edge Cases
// ============================================================================

void test_extract_double_quoted_vs_unquoted() {
    // Test quoted number
    std::string json1 = R"({"price":"42000.50"})";
    MarkPriceUpdate mp1 = BinanceFuturesWs::parse_mark_price_update(R"({"p":"42000.50","E":0,"T":0})");
    assert(std::abs(mp1.mark_price - 42000.50) < 0.01);

    // Test unquoted number
    MarkPriceUpdate mp2 = BinanceFuturesWs::parse_mark_price_update(R"({"p":42000.50,"E":0,"T":0})");
    assert(std::abs(mp2.mark_price - 42000.50) < 0.01);

    std::cout << "✓ test_extract_double_quoted_vs_unquoted\n";
}

void test_extract_bool_values() {
    // Test true
    WsAggTrade trade1 =
        BinanceFuturesWs::parse_agg_trade(R"({"s":"BTCUSDT","a":1,"p":"0","q":"0","f":0,"l":0,"T":0,"m":true})");
    assert(trade1.is_buyer_maker == true);

    // Test false
    WsAggTrade trade2 =
        BinanceFuturesWs::parse_agg_trade(R"({"s":"BTCUSDT","a":1,"p":"0","q":"0","f":0,"l":0,"T":0,"m":false})");
    assert(trade2.is_buyer_maker == false);

    std::cout << "✓ test_extract_bool_values\n";
}

void test_extract_uint64_large_values() {
    // Test large timestamp
    MarkPriceUpdate mp = BinanceFuturesWs::parse_mark_price_update(
        R"({"s":"BTC","p":"0","P":"0","r":"0","E":1700000000000,"T":9999999999999})");
    assert(mp.event_time == 1700000000000);
    assert(mp.next_funding_time == 9999999999999);

    std::cout << "✓ test_extract_uint64_large_values\n";
}

// ============================================================================
// Connection/Disconnection Tests (Mock Mode)
// ============================================================================

void test_connect_without_streams() {
    MockBinanceFuturesWs ws(false);

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
    MockBinanceFuturesWs ws(false);
    ws.subscribe_mark_price("BTCUSDT");

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
    MockBinanceFuturesWs ws(false);
    ws.subscribe_mark_price("BTCUSDT");

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
    MockBinanceFuturesWs ws(false);

    bool error_received = false;
    std::string error_msg;
    ws.set_error_callback([&](const std::string& err) {
        error_received = true;
        error_msg = err;
    });

    ws.simulate_error_for_test("Connection timeout");

    assert(error_received == true);
    assert(error_msg == "Connection timeout");

    std::cout << "✓ test_error_callback\n";
}

void test_is_connected_is_running() {
    MockBinanceFuturesWs ws(false);
    ws.subscribe_liquidation("BTCUSDT");

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

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "Running Binance futures WebSocket tests...\n\n";

    // Stream path building
    test_single_stream_path();
    test_combined_stream_path();

    // JSON parsing
    test_parse_mark_price_update();
    test_parse_liquidation_order();
    test_parse_futures_book_ticker();
    test_parse_agg_trade();
    test_parse_kline();
    test_parse_invalid_json();
    test_parse_missing_fields();

    // Subscribe validation
    test_subscribe_before_connect();
    test_connect_without_subscriptions();

    // Endpoint selection
    test_mainnet_endpoint();
    test_testnet_endpoint();

    // Subscription methods
    test_subscribe_methods();

    // Callback tests
    test_callback_setters();
    test_error_callback_invocation();

    // State tests
    test_initial_state();
    test_symbol_lowercase_conversion();

    // Message routing tests
    test_parse_message_routes_mark_price();
    test_parse_message_routes_liquidation();
    test_parse_message_routes_book_ticker();
    test_parse_message_routes_agg_trade();
    test_parse_message_routes_kline();
    test_parse_message_combined_stream();
    test_parse_message_no_callback_set();

    // JSON extractor edge cases
    test_extract_double_quoted_vs_unquoted();
    test_extract_bool_values();
    test_extract_uint64_large_values();

    // Connection/disconnection tests (mock mode)
    test_connect_without_streams();
    test_connect_with_streams();
    test_disconnect_triggers_callback();
    test_error_callback();
    test_is_connected_is_running();

    std::cout << "\n✅ All Binance futures WebSocket tests passed!\n";
    return 0;
}
