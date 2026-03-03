#include "../include/exchange/binance_futures_ws.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>

using namespace hft;
using namespace hft::exchange;

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
    BinanceFuturesWs ws;

    // Should succeed - subscribing before connect is allowed
    ws.subscribe_mark_price("BTCUSDT");
    ws.subscribe_liquidation("ETHUSDT");

    std::cout << "✓ test_subscribe_before_connect\n";
}

void test_connect_without_subscriptions() {
    BinanceFuturesWs ws;

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
    BinanceFuturesWs ws;

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

    std::cout << "\n✅ All Binance futures WebSocket tests passed!\n";
    return 0;
}
