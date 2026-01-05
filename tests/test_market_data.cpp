#include <cassert>
#include <iostream>
#include <fstream>
#include <cstdio>
#include "../include/exchange/market_data.hpp"

using namespace hft;
using namespace hft::exchange;

#define TEST(name) void name()
#define RUN_TEST(name) do { \
    std::cout << "Running " << #name << "... "; \
    name(); \
    std::cout << "PASSED\n"; \
} while(0)

#define ASSERT_EQ(a, b) assert((a) == (b))
#define ASSERT_TRUE(x) assert(x)
#define ASSERT_FALSE(x) assert(!(x))
#define ASSERT_NEAR(a, b, eps) assert(std::abs((a) - (b)) < (eps))

// === Kline Tests ===

TEST(test_kline_mid) {
    Kline k;
    k.high = 100000;  // $10.00
    k.low = 90000;    // $9.00

    ASSERT_EQ(k.mid(), 95000);  // $9.50
}

TEST(test_kline_range) {
    Kline k;
    k.high = 100000;
    k.low = 90000;

    ASSERT_EQ(k.range(), 10000);
}

TEST(test_kline_bullish) {
    Kline k;
    k.open = 90000;
    k.close = 100000;

    ASSERT_TRUE(k.is_bullish());
    ASSERT_FALSE(k.is_bearish());
}

TEST(test_kline_bearish) {
    Kline k;
    k.open = 100000;
    k.close = 90000;

    ASSERT_TRUE(k.is_bearish());
    ASSERT_FALSE(k.is_bullish());
}

TEST(test_kline_body_ratio) {
    Kline k;
    k.open = 90000;
    k.close = 100000;
    k.high = 105000;
    k.low = 85000;

    // Body = 10000, Range = 20000, Ratio = 0.5
    ASSERT_NEAR(k.body_ratio(), 0.5, 0.001);
}

TEST(test_kline_body_ratio_zero_range) {
    Kline k;
    k.open = 100000;
    k.close = 100000;
    k.high = 100000;
    k.low = 100000;

    ASSERT_EQ(k.body_ratio(), 0.0);
}

// === CSV Tests ===

TEST(test_save_load_klines_csv) {
    const std::string filename = "/tmp/test_klines.csv";

    // Create test data
    std::vector<Kline> original;

    Kline k1;
    k1.open_time = 1704067200000;  // 2024-01-01 00:00:00
    k1.close_time = 1704070799999;
    k1.open = 420000;  // $42.00
    k1.high = 430000;
    k1.low = 415000;
    k1.close = 425000;
    k1.volume = 1000.5;
    k1.quote_volume = 42500.0;
    k1.trades = 500;
    k1.taker_buy_volume = 600.3;
    original.push_back(k1);

    Kline k2;
    k2.open_time = 1704070800000;
    k2.close_time = 1704074399999;
    k2.open = 425000;
    k2.high = 428000;
    k2.low = 420000;
    k2.close = 427000;
    k2.volume = 800.2;
    k2.quote_volume = 34100.0;
    k2.trades = 350;
    k2.taker_buy_volume = 450.1;
    original.push_back(k2);

    // Save
    save_klines_csv(filename, original);

    // Load
    auto loaded = load_klines_csv(filename);

    ASSERT_EQ(loaded.size(), 2);

    // Check first kline
    ASSERT_EQ(loaded[0].open_time, k1.open_time);
    ASSERT_EQ(loaded[0].close_time, k1.close_time);
    ASSERT_EQ(loaded[0].open, k1.open);
    ASSERT_EQ(loaded[0].high, k1.high);
    ASSERT_EQ(loaded[0].low, k1.low);
    ASSERT_EQ(loaded[0].close, k1.close);
    ASSERT_EQ(loaded[0].trades, k1.trades);

    // Check second kline
    ASSERT_EQ(loaded[1].open_time, k2.open_time);
    ASSERT_EQ(loaded[1].close, k2.close);

    // Cleanup
    std::remove(filename.c_str());
}

TEST(test_load_klines_csv_with_header) {
    const std::string filename = "/tmp/test_klines_header.csv";

    // Create CSV with header
    std::ofstream f(filename);
    f << "open_time,open,high,low,close,volume,close_time,quote_volume,trades,taker_buy_volume,ignore\n";
    f << "1704067200000,42.0,43.0,41.5,42.5,1000.5,1704070799999,42500,500,600.3,0\n";
    f.close();

    auto klines = load_klines_csv(filename);

    ASSERT_EQ(klines.size(), 1);
    ASSERT_EQ(klines[0].open_time, 1704067200000ULL);
    ASSERT_EQ(klines[0].open, 420000);  // 42.0 * 10000

    std::remove(filename.c_str());
}

TEST(test_load_klines_csv_no_header) {
    const std::string filename = "/tmp/test_klines_noheader.csv";

    // Create CSV without header
    std::ofstream f(filename);
    f << "1704067200000,42.0,43.0,41.5,42.5,1000.5,1704070799999,42500,500,600.3,0\n";
    f.close();

    auto klines = load_klines_csv(filename);

    ASSERT_EQ(klines.size(), 1);
    ASSERT_EQ(klines[0].open, 420000);

    std::remove(filename.c_str());
}

TEST(test_load_klines_csv_empty_file) {
    const std::string filename = "/tmp/test_klines_empty.csv";

    std::ofstream f(filename);
    f << "";
    f.close();

    auto klines = load_klines_csv(filename);
    ASSERT_EQ(klines.size(), 0);

    std::remove(filename.c_str());
}

TEST(test_load_klines_csv_file_not_found) {
    bool exception_thrown = false;
    try {
        load_klines_csv("/tmp/nonexistent_file.csv");
    } catch (const std::runtime_error& e) {
        exception_thrown = true;
    }
    ASSERT_TRUE(exception_thrown);
}

// === Trade Tests ===

TEST(test_save_load_trades_csv) {
    const std::string filename = "/tmp/test_trades.csv";

    // Create test data
    std::vector<MarketTrade> original_trades;

    MarketTrade trade1;
    trade1.time = 1704067200000;
    trade1.price = 420000;
    trade1.quantity = 0.5;
    trade1.is_buyer_maker = true;
    original_trades.push_back(trade1);

    MarketTrade trade2;
    trade2.time = 1704067201000;
    trade2.price = 420100;
    trade2.quantity = 1.2;
    trade2.is_buyer_maker = false;
    original_trades.push_back(trade2);

    // Create CSV manually (no save function for trades yet)
    std::ofstream f(filename);
    f << "time,price,quantity,is_buyer_maker\n";
    f << "1704067200000,42.0,0.5,true\n";
    f << "1704067201000,42.01,1.2,false\n";
    f.close();

    // Load
    auto loaded = load_trades_csv(filename);

    ASSERT_EQ(loaded.size(), 2);
    ASSERT_EQ(loaded[0].time, 1704067200000ULL);
    ASSERT_EQ(loaded[0].price, 420000);
    ASSERT_NEAR(loaded[0].quantity, 0.5, 0.001);
    ASSERT_TRUE(loaded[0].is_buyer_maker);

    ASSERT_FALSE(loaded[1].is_buyer_maker);

    std::remove(filename.c_str());
}

int main() {
    std::cout << "=== Market Data Tests ===\n\n";

    // Kline tests
    RUN_TEST(test_kline_mid);
    RUN_TEST(test_kline_range);
    RUN_TEST(test_kline_bullish);
    RUN_TEST(test_kline_bearish);
    RUN_TEST(test_kline_body_ratio);
    RUN_TEST(test_kline_body_ratio_zero_range);

    // CSV tests
    RUN_TEST(test_save_load_klines_csv);
    RUN_TEST(test_load_klines_csv_with_header);
    RUN_TEST(test_load_klines_csv_no_header);
    RUN_TEST(test_load_klines_csv_empty_file);
    RUN_TEST(test_load_klines_csv_file_not_found);

    // Trade tests
    RUN_TEST(test_save_load_trades_csv);

    std::cout << "\nAll tests PASSED!\n";
    return 0;
}
