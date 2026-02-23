#include "../include/metrics/trade_stream_metrics.hpp"
#include "../include/types.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace hft;

#define TEST(name) void name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "Running " << #name << "... ";                                                                    \
        name();                                                                                                        \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

#define ASSERT_EQ(a, b) assert((a) == (b))
#define ASSERT_TRUE(x) assert(x)
#define ASSERT_FALSE(x) assert(!(x))
#define ASSERT_NEAR(a, b, eps) assert(std::abs((a) - (b)) < (eps))

// Helper function to convert microseconds to different units
constexpr uint64_t SECOND_US = 1'000'000;
constexpr uint64_t MINUTE_US = 60 * SECOND_US;

// ============================================================================
// Volume Tracking Tests (6 tests)
// ============================================================================

TEST(test_empty_metrics) {
    TradeStreamMetrics metrics;

    auto m = metrics.get_metrics(TradeWindow::W1s);
    ASSERT_NEAR(m.buy_volume, 0.0, 1e-9);
    ASSERT_NEAR(m.sell_volume, 0.0, 1e-9);
    ASSERT_NEAR(m.total_volume, 0.0, 1e-9);
    ASSERT_NEAR(m.delta, 0.0, 1e-9);
    ASSERT_EQ(m.total_trades, 0);
}

TEST(test_single_buy_trade) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 100, true, 0); // price=$1.00, qty=100, buy

    auto m = metrics.get_metrics(TradeWindow::W1s);
    ASSERT_NEAR(m.buy_volume, 100.0, 1e-9);
    ASSERT_NEAR(m.sell_volume, 0.0, 1e-9);
    ASSERT_NEAR(m.total_volume, 100.0, 1e-9);
    ASSERT_NEAR(m.delta, 100.0, 1e-9);
    ASSERT_EQ(m.buy_trades, 1);
    ASSERT_EQ(m.total_trades, 1);
}

TEST(test_single_sell_trade) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 50, false, 0); // price=$1.00, qty=50, sell

    auto m = metrics.get_metrics(TradeWindow::W1s);
    ASSERT_NEAR(m.buy_volume, 0.0, 1e-9);
    ASSERT_NEAR(m.sell_volume, 50.0, 1e-9);
    ASSERT_NEAR(m.total_volume, 50.0, 1e-9);
    ASSERT_NEAR(m.delta, -50.0, 1e-9);
    ASSERT_EQ(m.sell_trades, 1);
    ASSERT_EQ(m.total_trades, 1);
}

TEST(test_mixed_trades_volume) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 100, true, 0); // buy 100
    metrics.on_trade(10010, 50, false, 0); // sell 50
    metrics.on_trade(10020, 75, true, 0);  // buy 75

    auto m = metrics.get_metrics(TradeWindow::W1s);
    ASSERT_NEAR(m.buy_volume, 175.0, 1e-9);
    ASSERT_NEAR(m.sell_volume, 50.0, 1e-9);
    ASSERT_NEAR(m.total_volume, 225.0, 1e-9);
}

TEST(test_delta_calculation) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 100, true, 0);  // delta = +100
    metrics.on_trade(10010, 200, false, 0); // delta = -100
    metrics.on_trade(10020, 150, true, 0);  // delta = +50

    auto m = metrics.get_metrics(TradeWindow::W1s);
    ASSERT_NEAR(m.delta, 50.0, 1e-9); // 100 - 200 + 150 = 50
    ASSERT_NEAR(m.cumulative_delta, 50.0, 1e-9);
}

TEST(test_buy_ratio) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 300, true, 0);  // buy 300
    metrics.on_trade(10010, 100, false, 0); // sell 100

    auto m = metrics.get_metrics(TradeWindow::W1s);
    ASSERT_NEAR(m.buy_ratio, 0.75, 1e-9); // 300 / (300 + 100) = 0.75
}

// ============================================================================
// Trade Count Tests (4 tests)
// ============================================================================

TEST(test_trade_counts) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 100, true, 0);
    metrics.on_trade(10010, 50, false, 0);
    metrics.on_trade(10020, 75, true, 0);
    metrics.on_trade(10030, 25, false, 0);

    auto m = metrics.get_metrics(TradeWindow::W1s);
    ASSERT_EQ(m.total_trades, 4);
    ASSERT_EQ(m.buy_trades, 2);
    ASSERT_EQ(m.sell_trades, 2);
}

TEST(test_large_trades_default_threshold) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 100, true, 0);  // normal
    metrics.on_trade(10010, 500, false, 0); // large (>= 500)
    metrics.on_trade(10020, 1000, true, 0); // large
    metrics.on_trade(10030, 499, false, 0); // normal

    auto m = metrics.get_metrics(TradeWindow::W1s);
    ASSERT_EQ(m.large_trades, 2);
}

TEST(test_large_trades_custom_threshold) {
    TradeStreamMetrics metrics(1000); // custom threshold = 1000

    metrics.on_trade(10000, 500, true, 0);   // normal
    metrics.on_trade(10010, 1000, false, 0); // large
    metrics.on_trade(10020, 1500, true, 0);  // large

    auto m = metrics.get_metrics(TradeWindow::W1s);
    ASSERT_EQ(m.large_trades, 2);
}

TEST(test_trade_counts_multiple_windows) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 100, true, 0);
    metrics.on_trade(10010, 50, false, 2 * SECOND_US); // 2 seconds later
    metrics.on_trade(10020, 75, true, 6 * SECOND_US);  // 6 seconds later

    // 1s window: only last trade
    auto m1s = metrics.get_metrics(TradeWindow::W1s);
    ASSERT_EQ(m1s.total_trades, 1);

    // 10s window: all trades
    auto m10s = metrics.get_metrics(TradeWindow::W10s);
    ASSERT_EQ(m10s.total_trades, 3);
}

// ============================================================================
// Price Metrics Tests (6 tests)
// ============================================================================

TEST(test_vwap_single_trade) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 100, true, 0);

    auto m = metrics.get_metrics(TradeWindow::W1s);
    ASSERT_NEAR(m.vwap, 10000.0, 1e-9);
}

TEST(test_vwap_multiple_trades) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 100, true, 0);
    metrics.on_trade(10100, 200, false, 0);
    metrics.on_trade(10200, 100, true, 0);

    auto m = metrics.get_metrics(TradeWindow::W1s);
    // VWAP = (10000*100 + 10100*200 + 10200*100) / (100 + 200 + 100)
    //      = (1000000 + 2020000 + 1020000) / 400
    //      = 4040000 / 400 = 10100
    ASSERT_NEAR(m.vwap, 10100.0, 1e-9);
}

TEST(test_price_high_low) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 100, true, 0);
    metrics.on_trade(10200, 50, false, 0);
    metrics.on_trade(9800, 75, true, 0);
    metrics.on_trade(10100, 25, false, 0);

    auto m = metrics.get_metrics(TradeWindow::W1s);
    ASSERT_NEAR(m.high, 10200.0, 1e-9);
    ASSERT_NEAR(m.low, 9800.0, 1e-9);
}

TEST(test_price_velocity) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 100, true, 0);
    metrics.on_trade(10100, 50, false, 500'000); // 0.5 seconds later, +100 price

    auto m = metrics.get_metrics(TradeWindow::W1s);
    // velocity = (10100 - 10000) / 0.5 seconds = 200 price units per second
    ASSERT_NEAR(m.price_velocity, 200.0, 1e-6);
}

TEST(test_realized_volatility) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 100, true, 0);
    metrics.on_trade(10100, 50, false, 100'000);
    metrics.on_trade(9900, 75, true, 200'000);
    metrics.on_trade(10050, 25, false, 300'000);

    auto m = metrics.get_metrics(TradeWindow::W1s);
    // Realized volatility = std dev of price changes
    // Changes: +100, -200, +150
    ASSERT_TRUE(m.realized_volatility > 0.0);
}

TEST(test_vwap_zero_volume) {
    TradeStreamMetrics metrics;

    auto m = metrics.get_metrics(TradeWindow::W1s);
    ASSERT_NEAR(m.vwap, 0.0, 1e-9); // No trades = VWAP is 0
}

// ============================================================================
// Streak Tests (4 tests)
// ============================================================================

TEST(test_buy_streak) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 100, true, 0);
    metrics.on_trade(10010, 50, true, 0);
    metrics.on_trade(10020, 75, true, 0);
    metrics.on_trade(10030, 25, false, 0); // breaks streak

    auto m = metrics.get_metrics(TradeWindow::W1s);
    ASSERT_EQ(m.buy_streak, 0); // broken by sell
    ASSERT_EQ(m.max_buy_streak, 3);
}

TEST(test_sell_streak) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 100, false, 0);
    metrics.on_trade(10010, 50, false, 0);
    metrics.on_trade(10020, 75, true, 0); // breaks streak
    metrics.on_trade(10030, 25, false, 0);

    auto m = metrics.get_metrics(TradeWindow::W1s);
    ASSERT_EQ(m.sell_streak, 1);
    ASSERT_EQ(m.max_sell_streak, 2);
}

TEST(test_alternating_sides_no_streaks) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 100, true, 0);
    metrics.on_trade(10010, 50, false, 0);
    metrics.on_trade(10020, 75, true, 0);
    metrics.on_trade(10030, 25, false, 0);

    auto m = metrics.get_metrics(TradeWindow::W1s);
    ASSERT_EQ(m.buy_streak, 0);
    ASSERT_EQ(m.sell_streak, 1);
    ASSERT_EQ(m.max_buy_streak, 1);
    ASSERT_EQ(m.max_sell_streak, 1);
}

TEST(test_streak_reset_on_window_expiry) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 100, true, 0);
    metrics.on_trade(10010, 50, true, 0);
    metrics.on_trade(10020, 75, true, 2 * SECOND_US); // outside 1s window

    auto m1s = metrics.get_metrics(TradeWindow::W1s);
    ASSERT_EQ(m1s.buy_streak, 1); // only last trade in window

    auto m10s = metrics.get_metrics(TradeWindow::W10s);
    ASSERT_EQ(m10s.buy_streak, 3); // all trades in window
}

// ============================================================================
// Timing Metrics Tests (4 tests)
// ============================================================================

TEST(test_avg_inter_trade_time) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 100, true, 0);
    metrics.on_trade(10010, 50, false, 100'000); // 100ms later
    metrics.on_trade(10020, 75, true, 300'000);  // 200ms later
    metrics.on_trade(10030, 25, false, 400'000); // 100ms later

    auto m = metrics.get_metrics(TradeWindow::W1s);
    // Inter-trade times: 100ms, 200ms, 100ms
    // Average: (100 + 200 + 100) / 3 = 133.33ms = 133333 microseconds
    ASSERT_NEAR(m.avg_inter_trade_time_us, 133333.0, 1000.0);
}

TEST(test_min_inter_trade_time) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 100, true, 0);
    metrics.on_trade(10010, 50, false, 100'000); // 100ms
    metrics.on_trade(10020, 75, true, 150'000);  // 50ms (minimum)
    metrics.on_trade(10030, 25, false, 350'000); // 200ms

    auto m = metrics.get_metrics(TradeWindow::W1s);
    ASSERT_NEAR(m.min_inter_trade_time_us, 50000.0, 1e-9);
}

TEST(test_burst_count) {
    TradeStreamMetrics metrics;

    // Burst threshold = 10ms (10000 microseconds)
    metrics.on_trade(10000, 100, true, 0);
    metrics.on_trade(10010, 50, false, 5'000);   // 5ms - burst
    metrics.on_trade(10020, 75, true, 10'000);   // 5ms - burst
    metrics.on_trade(10030, 25, false, 500'000); // 490ms - not burst

    auto m = metrics.get_metrics(TradeWindow::W1s);
    ASSERT_EQ(m.burst_count, 2); // 2 trades within 10ms
}

TEST(test_timing_single_trade) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 100, true, 0);

    auto m = metrics.get_metrics(TradeWindow::W1s);
    ASSERT_NEAR(m.avg_inter_trade_time_us, 0.0, 1e-9);
    ASSERT_NEAR(m.min_inter_trade_time_us, 0.0, 1e-9);
    ASSERT_EQ(m.burst_count, 0);
}

// ============================================================================
// Tick Tests (3 tests)
// ============================================================================

TEST(test_upticks_downticks) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 100, true, 0);
    metrics.on_trade(10010, 50, false, 0); // uptick (+10)
    metrics.on_trade(10005, 75, true, 0);  // downtick (-5)
    metrics.on_trade(10005, 25, false, 0); // zerotick (0)

    auto m = metrics.get_metrics(TradeWindow::W1s);
    ASSERT_EQ(m.uptick_count, 1);
    ASSERT_EQ(m.downtick_count, 1);
    ASSERT_EQ(m.zerotick_count, 1);
}

TEST(test_tick_ratio) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 100, true, 0);
    metrics.on_trade(10010, 50, false, 0); // uptick
    metrics.on_trade(10020, 75, true, 0);  // uptick
    metrics.on_trade(10015, 25, false, 0); // downtick

    auto m = metrics.get_metrics(TradeWindow::W1s);
    // tick_ratio = (upticks - downticks) / total_ticks = (2 - 1) / 3 = 0.333
    ASSERT_NEAR(m.tick_ratio, 0.333, 0.01);
}

TEST(test_zerotick_count) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 100, true, 0);
    metrics.on_trade(10000, 50, false, 0); // zerotick
    metrics.on_trade(10000, 75, true, 0);  // zerotick
    metrics.on_trade(10010, 25, false, 0); // uptick

    auto m = metrics.get_metrics(TradeWindow::W1s);
    ASSERT_EQ(m.zerotick_count, 2);
    ASSERT_EQ(m.uptick_count, 1);
}

// ============================================================================
// Window Expiry Tests (3 tests)
// ============================================================================

TEST(test_1s_window_expiry) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 100, true, 0);
    metrics.on_trade(10010, 50, false, 500'000);  // 0.5s later
    metrics.on_trade(10020, 75, true, 1'500'000); // 1.5s later (outside 1s window)

    auto m = metrics.get_metrics(TradeWindow::W1s);
    ASSERT_EQ(m.total_trades, 1); // only last trade
    ASSERT_NEAR(m.total_volume, 75.0, 1e-9);
}

TEST(test_5s_window) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 100, true, 0);
    metrics.on_trade(10010, 50, false, 2 * SECOND_US);
    metrics.on_trade(10020, 75, true, 4 * SECOND_US);
    metrics.on_trade(10030, 25, false, 6 * SECOND_US); // outside 5s window

    auto m = metrics.get_metrics(TradeWindow::W5s);
    ASSERT_EQ(m.total_trades, 3);
}

TEST(test_multiple_windows_independent) {
    TradeStreamMetrics metrics;

    // Design trades so each window has a specific count
    // Current time will be 65s, windows look back from there
    metrics.on_trade(10000, 100, true, 0);              // 65s old - will be removed (>60s)
    metrics.on_trade(10010, 50, false, 7 * SECOND_US);  // 58s old - within 1min only
    metrics.on_trade(10020, 75, true, 37 * SECOND_US);  // 28s old - within 30s
    metrics.on_trade(10030, 25, false, 57 * SECOND_US); // 8s old - within 10s
    metrics.on_trade(10040, 20, true, 62 * SECOND_US);  // 3s old - within 5s
    metrics.on_trade(10050, 30, false, 65 * SECOND_US); // 0s old - within 1s

    // After cleanup when last trade added: [7s, 37s, 57s, 62s, 65s]
    // Current time: 65s

    auto m1s = metrics.get_metrics(TradeWindow::W1s);
    auto m5s = metrics.get_metrics(TradeWindow::W5s);
    auto m10s = metrics.get_metrics(TradeWindow::W10s);
    auto m30s = metrics.get_metrics(TradeWindow::W30s);
    auto m1min = metrics.get_metrics(TradeWindow::W1min);

    ASSERT_EQ(m1s.total_trades, 1);   // [65s]
    ASSERT_EQ(m5s.total_trades, 2);   // [62s, 65s]
    ASSERT_EQ(m10s.total_trades, 3);  // [57s, 62s, 65s]
    ASSERT_EQ(m30s.total_trades, 4);  // [37s, 57s, 62s, 65s]
    ASSERT_EQ(m1min.total_trades, 5); // [7s, 37s, 57s, 62s, 65s]
}

// ============================================================================
// Reset Test
// ============================================================================

TEST(test_reset) {
    TradeStreamMetrics metrics;

    metrics.on_trade(10000, 100, true, 0);
    metrics.on_trade(10010, 50, false, 0);

    metrics.reset();

    auto m = metrics.get_metrics(TradeWindow::W1s);
    ASSERT_NEAR(m.buy_volume, 0.0, 1e-9);
    ASSERT_NEAR(m.sell_volume, 0.0, 1e-9);
    ASSERT_EQ(m.total_trades, 0);
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "=== TradeStreamMetrics Tests ===\n\n";

    // Volume tracking (6 tests)
    RUN_TEST(test_empty_metrics);
    RUN_TEST(test_single_buy_trade);
    RUN_TEST(test_single_sell_trade);
    RUN_TEST(test_mixed_trades_volume);
    RUN_TEST(test_delta_calculation);
    RUN_TEST(test_buy_ratio);

    // Trade count (4 tests)
    RUN_TEST(test_trade_counts);
    RUN_TEST(test_large_trades_default_threshold);
    RUN_TEST(test_large_trades_custom_threshold);
    RUN_TEST(test_trade_counts_multiple_windows);

    // Price metrics (6 tests)
    RUN_TEST(test_vwap_single_trade);
    RUN_TEST(test_vwap_multiple_trades);
    RUN_TEST(test_price_high_low);
    RUN_TEST(test_price_velocity);
    RUN_TEST(test_realized_volatility);
    RUN_TEST(test_vwap_zero_volume);

    // Streaks (4 tests)
    RUN_TEST(test_buy_streak);
    RUN_TEST(test_sell_streak);
    RUN_TEST(test_alternating_sides_no_streaks);
    RUN_TEST(test_streak_reset_on_window_expiry);

    // Timing (4 tests)
    RUN_TEST(test_avg_inter_trade_time);
    RUN_TEST(test_min_inter_trade_time);
    RUN_TEST(test_burst_count);
    RUN_TEST(test_timing_single_trade);

    // Ticks (3 tests)
    RUN_TEST(test_upticks_downticks);
    RUN_TEST(test_tick_ratio);
    RUN_TEST(test_zerotick_count);

    // Window expiry (3 tests)
    RUN_TEST(test_1s_window_expiry);
    RUN_TEST(test_5s_window);
    RUN_TEST(test_multiple_windows_independent);

    // Reset (1 test)
    RUN_TEST(test_reset);

    std::cout << "\n=== All 34 tests PASSED! ===\n";
    return 0;
}
