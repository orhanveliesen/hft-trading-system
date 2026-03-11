#include "../include/exchange/binance_futures_rest.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <map>
#include <string>

using namespace hft::exchange;

// Mock HTTP client for testing (template policy pattern - zero overhead)
class MockHttpClient {
public:
    explicit MockHttpClient(const std::string& base_url) : base_url_(base_url) {}

    std::string get(const std::string& path) {
        // Return pre-canned responses based on path
        if (path.find("premiumIndex") != std::string::npos) {
            return R"({"symbol":"BTCUSDT","markPrice":"42000.50","indexPrice":"41950.00","lastFundingRate":"0.00010000","nextFundingTime":1700028800000,"time":1700000000000})";
        }
        if (path.find("fundingRate") != std::string::npos) {
            return R"([{"symbol":"BTCUSDT","fundingRate":"0.00010000","fundingTime":1700000000000}])";
        }
        if (path.find("openInterestHist") != std::string::npos) {
            return R"([{"symbol":"BTCUSDT","sumOpenInterest":"1000.50","sumOpenInterestValue":"42000000.00","timestamp":1700000000000}])";
        }
        if (path.find("openInterest") != std::string::npos) {
            return R"({"symbol":"BTCUSDT","openInterest":"1000.50","time":1700000000000})";
        }
        if (path.find("klines") != std::string::npos) {
            return R"([[1700000000000,"42000.00","42500.00","41800.00","42300.00","100.5",1700003599999,"4200000.00",1500,"50.25","2100000.00","0"]])";
        }
        return "{}";
    }

private:
    std::string base_url_;
};

// Mock that throws errors for testing error handling
class ErrorHttpClient {
public:
    explicit ErrorHttpClient(const std::string& base_url) : base_url_(base_url) {}

    std::string get(const std::string& /*path*/) { throw std::runtime_error("Network error"); }

private:
    std::string base_url_;
};

// ============================================================================
// URL Construction Tests
// ============================================================================

void test_build_funding_rate_url() {
    std::string url = BinanceFuturesRest<>::build_funding_rate_url("BTCUSDT");
    assert(url == "/fapi/v1/premiumIndex?symbol=BTCUSDT");

    std::cout << "✓ test_build_funding_rate_url\n";
}

void test_build_funding_rate_history_url() {
    std::string url =
        BinanceFuturesRest<>::build_funding_rate_history_url("BTCUSDT", 1700000000000, 1700086400000, 100);
    assert(url == "/fapi/v1/fundingRate?symbol=BTCUSDT&startTime=1700000000000&endTime=1700086400000&limit=100");

    // Test without optional params
    url = BinanceFuturesRest<>::build_funding_rate_history_url("ETHUSDT", 0, 0, 100);
    assert(url == "/fapi/v1/fundingRate?symbol=ETHUSDT&limit=100");

    std::cout << "✓ test_build_funding_rate_history_url\n";
}

void test_build_open_interest_url() {
    std::string url = BinanceFuturesRest<>::build_open_interest_url("BTCUSDT");
    assert(url == "/fapi/v1/openInterest?symbol=BTCUSDT");

    std::cout << "✓ test_build_open_interest_url\n";
}

void test_build_open_interest_history_url() {
    std::string url =
        BinanceFuturesRest<>::build_open_interest_history_url("BTCUSDT", "5m", 1700000000000, 1700086400000, 30);
    assert(url == "/futures/data/openInterestHist?symbol=BTCUSDT&period=5m&startTime=1700000000000&endTime="
                  "1700086400000&limit=30");

    // Test without optional params
    url = BinanceFuturesRest<>::build_open_interest_history_url("ETHUSDT", "1h", 0, 0, 30);
    assert(url == "/futures/data/openInterestHist?symbol=ETHUSDT&period=1h&limit=30");

    std::cout << "✓ test_build_open_interest_history_url\n";
}

void test_build_mark_price_url() {
    std::string url = BinanceFuturesRest<>::build_mark_price_url("BTCUSDT");
    assert(url == "/fapi/v1/premiumIndex?symbol=BTCUSDT");

    std::cout << "✓ test_build_mark_price_url\n";
}

void test_build_klines_url() {
    std::string url = BinanceFuturesRest<>::build_klines_url("BTCUSDT", "1m", 0, 0, 500);
    assert(url == "/fapi/v1/klines?symbol=BTCUSDT&interval=1m&limit=500");

    // With time range
    url = BinanceFuturesRest<>::build_klines_url("ETHUSDT", "5m", 1700000000000, 1700086400000, 1000);
    assert(url == "/fapi/v1/klines?symbol=ETHUSDT&interval=5m&startTime=1700000000000&endTime=1700086400000&"
                  "limit=1000");

    std::cout << "✓ test_build_klines_url\n";
}

// ============================================================================
// JSON Parsing Tests
// ============================================================================

void test_parse_funding_rate_json() {
    std::string json = R"({
        "symbol": "BTCUSDT",
        "markPrice": "42000.50000000",
        "indexPrice": "41950.00000000",
        "lastFundingRate": "0.00010000",
        "nextFundingTime": 1700028800000,
        "time": 1700000000000
    })";

    FundingRate fr = BinanceFuturesRest<>::parse_funding_rate_json(json);

    assert(fr.symbol == "BTCUSDT");
    assert(std::abs(fr.mark_price - 42000.50) < 0.01);
    assert(std::abs(fr.funding_rate - 0.0001) < 1e-10);
    assert(fr.funding_time == 1700028800000);
    assert(fr.event_time == 1700000000000);

    std::cout << "✓ test_parse_funding_rate_json\n";
}

void test_parse_funding_rate_history_json() {
    std::string json = R"([
        {
            "symbol": "BTCUSDT",
            "fundingRate": "0.00010000",
            "fundingTime": 1700000000000
        },
        {
            "symbol": "BTCUSDT",
            "fundingRate": "0.00015000",
            "fundingTime": 1700028800000
        }
    ])";

    std::vector<FundingRate> history = BinanceFuturesRest<>::parse_funding_rate_history_json(json);

    assert(history.size() == 2);
    assert(history[0].symbol == "BTCUSDT");
    assert(std::abs(history[0].funding_rate - 0.0001) < 1e-10);
    assert(history[0].funding_time == 1700000000000);
    assert(history[1].symbol == "BTCUSDT");
    assert(std::abs(history[1].funding_rate - 0.00015) < 1e-10);
    assert(history[1].funding_time == 1700028800000);

    std::cout << "✓ test_parse_funding_rate_history_json\n";
}

void test_parse_open_interest_json() {
    std::string json = R"({
        "openInterest": "50000.000",
        "symbol": "BTCUSDT",
        "time": 1700000000000
    })";

    OpenInterest oi = BinanceFuturesRest<>::parse_open_interest_json(json);

    assert(oi.symbol == "BTCUSDT");
    assert(std::abs(oi.open_interest - 50000.0) < 0.01);
    assert(oi.time == 1700000000000);
    // Note: open_interest_value not in this endpoint

    std::cout << "✓ test_parse_open_interest_json\n";
}

void test_parse_open_interest_history_json() {
    std::string json = R"([
        {
            "symbol": "BTCUSDT",
            "sumOpenInterest": "50000.000",
            "sumOpenInterestValue": "2100000000.00",
            "timestamp": 1700000000000
        },
        {
            "symbol": "BTCUSDT",
            "sumOpenInterest": "51000.000",
            "sumOpenInterestValue": "2142000000.00",
            "timestamp": 1700000300000
        }
    ])";

    std::vector<OpenInterest> history = BinanceFuturesRest<>::parse_open_interest_history_json(json);

    assert(history.size() == 2);
    assert(history[0].symbol == "BTCUSDT");
    assert(std::abs(history[0].open_interest - 50000.0) < 0.01);
    assert(std::abs(history[0].open_interest_value - 2100000000.0) < 1.0);
    assert(history[0].time == 1700000000000);
    assert(history[1].symbol == "BTCUSDT");
    assert(std::abs(history[1].open_interest - 51000.0) < 0.01);
    assert(history[1].time == 1700000300000);

    std::cout << "✓ test_parse_open_interest_history_json\n";
}

void test_parse_mark_price_json() {
    std::string json = R"({
        "symbol": "BTCUSDT",
        "markPrice": "42000.50000000",
        "indexPrice": "41950.00000000",
        "lastFundingRate": "0.00010000",
        "nextFundingTime": 1700028800000,
        "time": 1700000000000
    })";

    MarkPriceUpdate mp = BinanceFuturesRest<>::parse_mark_price_json(json);

    assert(mp.symbol == "BTCUSDT");
    assert(std::abs(mp.mark_price - 42000.50) < 0.01);
    assert(std::abs(mp.index_price - 41950.00) < 0.01);
    assert(std::abs(mp.funding_rate - 0.0001) < 1e-10);
    assert(mp.next_funding_time == 1700028800000);
    assert(mp.event_time == 1700000000000);

    std::cout << "✓ test_parse_mark_price_json\n";
}

void test_parse_klines_json() {
    std::string json = R"([
        [
            1700000000000,
            "42.00",
            "42.15",
            "41.99",
            "42.10",
            "10.5",
            1700000060000,
            "441.525",
            100,
            "5.2",
            "218.3725",
            "0"
        ],
        [
            1700000060000,
            "42.10",
            "42.20",
            "42.05",
            "42.18",
            "8.3",
            1700000120000,
            "350.094",
            75,
            "4.1",
            "172.938",
            "0"
        ]
    ])";

    std::vector<Kline> klines = BinanceFuturesRest<>::parse_klines_json(json);

    assert(klines.size() == 2);
    assert(klines[0].open_time == 1700000000000);
    assert(klines[0].open == 420000);  // 42.00 * 10000
    assert(klines[0].high == 421500);  // 42.15 * 10000
    assert(klines[0].low == 419900);   // 41.99 * 10000
    assert(klines[0].close == 421000); // 42.10 * 10000
    assert(std::abs(klines[0].volume - 10.5) < 0.01);
    assert(klines[0].close_time == 1700000060000);
    assert(klines[0].trades == 100);
    assert(std::abs(klines[0].taker_buy_volume - 5.2) < 0.01);

    assert(klines[1].open_time == 1700000060000);
    assert(klines[1].close == 421800); // 42.18 * 10000

    std::cout << "✓ test_parse_klines_json\n";
}

// ============================================================================
// Error Handling Tests
// ============================================================================

void test_parse_malformed_json() {
    std::string bad_json = "not a json";

    // Should not crash, return empty/default values
    FundingRate fr = BinanceFuturesRest<>::parse_funding_rate_json(bad_json);
    assert(fr.symbol.empty());

    std::vector<FundingRate> history = BinanceFuturesRest<>::parse_funding_rate_history_json(bad_json);
    assert(history.empty());

    OpenInterest oi = BinanceFuturesRest<>::parse_open_interest_json(bad_json);
    assert(oi.symbol.empty());

    std::vector<OpenInterest> oi_history = BinanceFuturesRest<>::parse_open_interest_history_json(bad_json);
    assert(oi_history.empty());

    MarkPriceUpdate mp = BinanceFuturesRest<>::parse_mark_price_json(bad_json);
    assert(mp.symbol.empty());

    std::vector<Kline> klines = BinanceFuturesRest<>::parse_klines_json(bad_json);
    assert(klines.empty());

    std::cout << "✓ test_parse_malformed_json\n";
}

void test_parse_missing_fields() {
    std::string json = R"({"symbol": "BTCUSDT"})";

    // Should return struct with symbol set, other fields default
    FundingRate fr = BinanceFuturesRest<>::parse_funding_rate_json(json);
    assert(fr.symbol == "BTCUSDT");
    assert(fr.funding_rate == 0.0);
    assert(fr.mark_price == 0.0);

    std::cout << "✓ test_parse_missing_fields\n";
}

void test_parse_type_mismatch() {
    // markPrice as number instead of string
    std::string json = R"({
        "symbol": "BTCUSDT",
        "markPrice": 42000.50
    })";

    FundingRate fr = BinanceFuturesRest<>::parse_funding_rate_json(json);
    assert(fr.symbol == "BTCUSDT");
    // Should handle number type gracefully
    assert(std::abs(fr.mark_price - 42000.50) < 0.01);

    std::cout << "✓ test_parse_type_mismatch\n";
}

void test_parse_empty_array() {
    std::string json = "[]";

    std::vector<FundingRate> history = BinanceFuturesRest<>::parse_funding_rate_history_json(json);
    assert(history.empty());

    std::vector<OpenInterest> oi_history = BinanceFuturesRest<>::parse_open_interest_history_json(json);
    assert(oi_history.empty());

    std::vector<Kline> klines = BinanceFuturesRest<>::parse_klines_json(json);
    assert(klines.empty());

    std::cout << "✓ test_parse_empty_array\n";
}

void test_parse_klines_malformed_array() {
    // Array with insufficient elements (need 11, has 5)
    std::string json = R"([
        [1700000000000, "42.00", "42.15", "41.99", "42.10"]
    ])";

    std::vector<Kline> klines = BinanceFuturesRest<>::parse_klines_json(json);
    assert(klines.empty()); // Should skip malformed entries

    std::cout << "✓ test_parse_klines_malformed_array\n";
}

void test_parse_invalid_string_to_double() {
    // Invalid number string
    std::string json = R"({
        "symbol": "BTCUSDT",
        "markPrice": "invalid_number"
    })";

    FundingRate fr = BinanceFuturesRest<>::parse_funding_rate_json(json);
    assert(fr.symbol == "BTCUSDT");
    assert(fr.mark_price == 0.0); // Should default to 0.0 on parse error

    std::cout << "✓ test_parse_invalid_string_to_double\n";
}

void test_parse_klines_numeric_fields() {
    // Test with numeric price fields instead of strings
    std::string json = R"([
        [
            1700000000000,
            42.00,
            42.15,
            41.99,
            42.10,
            10.5,
            1700000060000,
            441.525,
            100,
            5.2,
            218.3725,
            0
        ]
    ])";

    std::vector<Kline> klines = BinanceFuturesRest<>::parse_klines_json(json);
    assert(klines.size() == 1);
    assert(klines[0].open == 420000);
    assert(std::abs(klines[0].volume - 10.5) < 0.01);

    std::cout << "✓ test_parse_klines_numeric_fields\n";
}

// ============================================================================
// Fetch Method Tests (with MockHttpClient)
// ============================================================================

void test_fetch_funding_rate() {
    BinanceFuturesRest<MockHttpClient> rest(false);

    FundingRate fr = rest.fetch_funding_rate("BTCUSDT");
    assert(fr.symbol == "BTCUSDT");
    assert(std::abs(fr.mark_price - 42000.50) < 0.01);
    assert(std::abs(fr.funding_rate - 0.0001) < 1e-10);

    std::cout << "✓ test_fetch_funding_rate\n";
}

void test_fetch_funding_rate_history() {
    BinanceFuturesRest<MockHttpClient> rest(false);

    auto history = rest.fetch_funding_rate_history("BTCUSDT", 0, 0, 100);
    assert(history.size() == 1);
    assert(history[0].symbol == "BTCUSDT");
    assert(std::abs(history[0].funding_rate - 0.0001) < 1e-10);

    std::cout << "✓ test_fetch_funding_rate_history\n";
}

void test_fetch_open_interest() {
    BinanceFuturesRest<MockHttpClient> rest(false);

    OpenInterest oi = rest.fetch_open_interest("BTCUSDT");
    assert(oi.symbol == "BTCUSDT");
    assert(std::abs(oi.open_interest - 1000.50) < 0.01);

    std::cout << "✓ test_fetch_open_interest\n";
}

void test_fetch_open_interest_history() {
    BinanceFuturesRest<MockHttpClient> rest(false);

    auto history = rest.fetch_open_interest_history("BTCUSDT", "5m", 0, 0, 30);
    assert(history.size() == 1);
    assert(history[0].symbol == "BTCUSDT");
    assert(std::abs(history[0].open_interest - 1000.50) < 0.01);
    assert(std::abs(history[0].open_interest_value - 42000000.00) < 0.01);

    std::cout << "✓ test_fetch_open_interest_history\n";
}

void test_fetch_mark_price() {
    BinanceFuturesRest<MockHttpClient> rest(false);

    MarkPriceUpdate mp = rest.fetch_mark_price("BTCUSDT");
    assert(mp.symbol == "BTCUSDT");
    assert(std::abs(mp.mark_price - 42000.50) < 0.01);
    assert(std::abs(mp.index_price - 41950.00) < 0.01);

    std::cout << "✓ test_fetch_mark_price\n";
}

void test_fetch_klines() {
    BinanceFuturesRest<MockHttpClient> rest(false);

    auto klines = rest.fetch_klines("BTCUSDT", "1m", 0, 0, 500);
    assert(klines.size() == 1);
    assert(klines[0].open == 420000000);  // 42000.00 * 10000
    assert(klines[0].high == 425000000);  // 42500.00 * 10000
    assert(klines[0].low == 418000000);   // 41800.00 * 10000
    assert(klines[0].close == 423000000); // 42300.00 * 10000
    assert(std::abs(klines[0].volume - 100.5) < 0.01);

    std::cout << "✓ test_fetch_klines\n";
}

void test_fetch_with_network_error() {
    BinanceFuturesRest<ErrorHttpClient> rest(false);

    try {
        rest.fetch_funding_rate("BTCUSDT");
        assert(false); // Should throw
    } catch (const std::runtime_error& e) {
        assert(std::string(e.what()) == "Network error");
    }

    std::cout << "✓ test_fetch_with_network_error\n";
}

void test_constructor_destructor() {
    // Test constructor/destructor with MockHttpClient
    {
        BinanceFuturesRest<MockHttpClient> rest_mainnet(false);
        BinanceFuturesRest<MockHttpClient> rest_testnet(true);
        // Verify proper initialization and cleanup
    } // Destructors called here

    std::cout << "✓ test_constructor_destructor\n";
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "Running Binance futures REST tests...\n\n";

    // URL construction
    test_build_funding_rate_url();
    test_build_funding_rate_history_url();
    test_build_open_interest_url();
    test_build_open_interest_history_url();
    test_build_mark_price_url();
    test_build_klines_url();

    // JSON parsing
    test_parse_funding_rate_json();
    test_parse_funding_rate_history_json();
    test_parse_open_interest_json();
    test_parse_open_interest_history_json();
    test_parse_mark_price_json();
    test_parse_klines_json();

    // Error handling
    test_parse_malformed_json();
    test_parse_missing_fields();
    test_parse_type_mismatch();
    test_parse_empty_array();
    test_parse_klines_malformed_array();
    test_parse_invalid_string_to_double();
    test_parse_klines_numeric_fields();

    // Fetch methods (with MockHttpClient)
    test_fetch_funding_rate();
    test_fetch_funding_rate_history();
    test_fetch_open_interest();
    test_fetch_open_interest_history();
    test_fetch_mark_price();
    test_fetch_klines();
    test_fetch_with_network_error();
    test_constructor_destructor();

    std::cout << "\n✅ All Binance futures REST tests passed!\n";
    return 0;
}
