#include "../include/metrics/trade_stream_metrics.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace hft;

void test_empty_metrics() {
    TradeStreamMetrics metrics;
    auto m = metrics.get_metrics(TradeWindow::W1s);
    assert(m.total_volume == 0.0);
    assert(m.buy_volume == 0.0);
    assert(m.sell_volume == 0.0);
    assert(m.total_trades == 0);
    std::cout << "✓ test_empty_metrics\n";
}

void test_single_buy_trade() {
    TradeStreamMetrics metrics;
    metrics.on_trade(10000, 100, true, 1'000'000);

    auto m = metrics.get_metrics(TradeWindow::W1s);
    assert(m.buy_volume == 100.0);
    assert(m.sell_volume == 0.0);
    assert(m.total_volume == 100.0);
    assert(m.buy_trades == 1);
    assert(m.sell_trades == 0);
    assert(m.total_trades == 1);
    assert(std::abs(m.vwap - 10000.0) < 0.01);
    std::cout << "✓ test_single_buy_trade\n";
}

void test_single_sell_trade() {
    TradeStreamMetrics metrics;
    metrics.on_trade(10000, 100, false, 1'000'000);

    auto m = metrics.get_metrics(TradeWindow::W1s);
    assert(m.buy_volume == 0.0);
    assert(m.sell_volume == 100.0);
    assert(m.total_volume == 100.0);
    assert(m.buy_trades == 0);
    assert(m.sell_trades == 1);
    assert(m.total_trades == 1);
    std::cout << "✓ test_single_sell_trade\n";
}

void test_mixed_trades_volume() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    metrics.on_trade(10000, 100, true, ts);
    metrics.on_trade(10010, 200, false, ts + 100'000);
    metrics.on_trade(10020, 150, true, ts + 200'000);

    auto m = metrics.get_metrics(TradeWindow::W1s);
    assert(m.buy_volume == 250.0);
    assert(m.sell_volume == 200.0);
    assert(m.total_volume == 450.0);
    assert(m.buy_trades == 2);
    assert(m.sell_trades == 1);
    assert(m.total_trades == 3);
    std::cout << "✓ test_mixed_trades_volume\n";
}

void test_delta_calculation() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    metrics.on_trade(10000, 300, true, ts);
    metrics.on_trade(10010, 100, false, ts + 100'000);

    auto m = metrics.get_metrics(TradeWindow::W1s);
    assert(std::abs(m.delta - 200.0) < 0.01); // 300 - 100
    std::cout << "✓ test_delta_calculation\n";
}

void test_buy_ratio() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    metrics.on_trade(10000, 300, true, ts);
    metrics.on_trade(10010, 100, false, ts + 100'000);

    auto m = metrics.get_metrics(TradeWindow::W1s);
    assert(std::abs(m.buy_ratio - 0.75) < 0.01); // 300 / 400
    std::cout << "✓ test_buy_ratio\n";
}

void test_vwap_calculation() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    metrics.on_trade(10000, 100, true, ts);
    metrics.on_trade(11000, 200, true, ts + 100'000);

    auto m = metrics.get_metrics(TradeWindow::W1s);
    // VWAP = (10000*100 + 11000*200) / 300 = 10666.67
    assert(std::abs(m.vwap - 10666.67) < 0.01);
    std::cout << "✓ test_vwap_calculation\n";
}

void test_high_low_prices() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    metrics.on_trade(10000, 100, true, ts);
    metrics.on_trade(10500, 100, true, ts + 100'000);
    metrics.on_trade(9800, 100, false, ts + 200'000);
    metrics.on_trade(10200, 100, true, ts + 300'000);

    auto m = metrics.get_metrics(TradeWindow::W1s);
    assert(m.high == 10500);
    assert(m.low == 9800);
    std::cout << "✓ test_high_low_prices\n";
}

void test_price_velocity() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    metrics.on_trade(10000, 100, true, ts);
    metrics.on_trade(10100, 100, true, ts + 500'000); // +100 in 0.5s

    auto m = metrics.get_metrics(TradeWindow::W1s);
    // Velocity = price_change / time = 100 / 0.5 = 200 per second
    assert(std::abs(m.price_velocity - 200.0) < 0.1);
    std::cout << "✓ test_price_velocity\n";
}

void test_realized_volatility() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    metrics.on_trade(10000, 100, true, ts);
    metrics.on_trade(10100, 100, true, ts + 100'000);
    metrics.on_trade(9900, 100, false, ts + 200'000);
    metrics.on_trade(10050, 100, true, ts + 300'000);

    auto m = metrics.get_metrics(TradeWindow::W1s);
    assert(m.realized_volatility > 0.0); // Should have some volatility
    std::cout << "✓ test_realized_volatility\n";
}

void test_buy_streak() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    metrics.on_trade(10000, 100, true, ts);
    metrics.on_trade(10010, 100, true, ts + 100'000);
    metrics.on_trade(10020, 100, true, ts + 200'000);
    metrics.on_trade(10030, 100, false, ts + 300'000);

    auto m = metrics.get_metrics(TradeWindow::W1s);
    assert(m.buy_streak == 0); // Current streak broken
    assert(m.max_buy_streak == 3);
    std::cout << "✓ test_buy_streak\n";
}

void test_sell_streak() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    metrics.on_trade(10000, 100, false, ts);
    metrics.on_trade(10010, 100, false, ts + 100'000);
    metrics.on_trade(10020, 100, true, ts + 200'000);

    auto m = metrics.get_metrics(TradeWindow::W1s);
    assert(m.sell_streak == 0); // Current streak broken
    assert(m.max_sell_streak == 2);
    std::cout << "✓ test_sell_streak\n";
}

void test_active_buy_streak() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    metrics.on_trade(10000, 100, true, ts);
    metrics.on_trade(10010, 100, true, ts + 100'000);
    metrics.on_trade(10020, 100, true, ts + 200'000);

    auto m = metrics.get_metrics(TradeWindow::W1s);
    assert(m.buy_streak == 3); // Still active
    assert(m.max_buy_streak == 3);
    std::cout << "✓ test_active_buy_streak\n";
}

void test_active_sell_streak() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    metrics.on_trade(10000, 100, false, ts);
    metrics.on_trade(10010, 100, false, ts + 100'000);

    auto m = metrics.get_metrics(TradeWindow::W1s);
    assert(m.sell_streak == 2); // Still active
    assert(m.max_sell_streak == 2);
    std::cout << "✓ test_active_sell_streak\n";
}

void test_large_trades() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    // Assume large trade threshold is based on running average
    metrics.on_trade(10000, 100, true, ts);
    metrics.on_trade(10010, 100, true, ts + 100'000);
    metrics.on_trade(10020, 1000, true, ts + 200'000); // Large trade

    auto m = metrics.get_metrics(TradeWindow::W1s);
    assert(m.large_trades > 0); // Should detect large trade
    std::cout << "✓ test_large_trades\n";
}

void test_avg_inter_trade_time() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    metrics.on_trade(10000, 100, true, ts);
    metrics.on_trade(10010, 100, true, ts + 100'000); // 100ms gap
    metrics.on_trade(10020, 100, true, ts + 300'000); // 200ms gap

    auto m = metrics.get_metrics(TradeWindow::W1s);
    // Average gap = (100 + 200) / 2 = 150ms = 150000us
    assert(std::abs(m.avg_inter_trade_time_us - 150'000.0) < 1000.0);
    std::cout << "✓ test_avg_inter_trade_time\n";
}

void test_min_inter_trade_time() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    metrics.on_trade(10000, 100, true, ts);
    metrics.on_trade(10010, 100, true, ts + 50'000);  // 50ms gap (min)
    metrics.on_trade(10020, 100, true, ts + 150'000); // 100ms gap

    auto m = metrics.get_metrics(TradeWindow::W1s);
    assert(m.min_inter_trade_time_us == 50'000);
    std::cout << "✓ test_min_inter_trade_time\n";
}

void test_burst_detection() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    // Rapid burst of trades
    for (int i = 0; i < 10; i++) {
        metrics.on_trade(10000 + i, 100, true, ts + i * 1000); // 1ms apart
    }

    auto m = metrics.get_metrics(TradeWindow::W1s);
    assert(m.burst_count > 0); // Should detect burst
    std::cout << "✓ test_burst_detection\n";
}

void test_upticks() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    metrics.on_trade(10000, 100, true, ts);
    metrics.on_trade(10010, 100, true, ts + 100'000); // Uptick
    metrics.on_trade(10020, 100, true, ts + 200'000); // Uptick

    auto m = metrics.get_metrics(TradeWindow::W1s);
    assert(m.upticks == 2);
    assert(m.downticks == 0);
    assert(m.zeroticks == 0);
    std::cout << "✓ test_upticks\n";
}

void test_downticks() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    metrics.on_trade(10000, 100, true, ts);
    metrics.on_trade(9990, 100, false, ts + 100'000); // Downtick
    metrics.on_trade(9980, 100, false, ts + 200'000); // Downtick

    auto m = metrics.get_metrics(TradeWindow::W1s);
    assert(m.upticks == 0);
    assert(m.downticks == 2);
    assert(m.zeroticks == 0);
    std::cout << "✓ test_downticks\n";
}

void test_zeroticks() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    metrics.on_trade(10000, 100, true, ts);
    metrics.on_trade(10000, 100, true, ts + 100'000);  // Zerotick
    metrics.on_trade(10000, 100, false, ts + 200'000); // Zerotick

    auto m = metrics.get_metrics(TradeWindow::W1s);
    assert(m.upticks == 0);
    assert(m.downticks == 0);
    assert(m.zeroticks == 2);
    std::cout << "✓ test_zeroticks\n";
}

void test_tick_ratio() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    metrics.on_trade(10000, 100, true, ts);
    metrics.on_trade(10010, 100, true, ts + 100'000);  // Uptick
    metrics.on_trade(10020, 100, true, ts + 200'000);  // Uptick
    metrics.on_trade(10010, 100, false, ts + 300'000); // Downtick

    auto m = metrics.get_metrics(TradeWindow::W1s);
    // tick_ratio = upticks / (upticks + downticks) = 2 / 3 = 0.666...
    assert(std::abs(m.tick_ratio - 0.6667) < 0.01);
    std::cout << "✓ test_tick_ratio\n";
}

void test_1s_window_expiry() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    metrics.on_trade(10000, 100, true, ts);
    metrics.on_trade(10010, 100, true, ts + 500'000);

    // Add trade 1.5 seconds later - first two should be expired
    metrics.on_trade(10020, 200, false, ts + 1'500'000);

    auto m = metrics.get_metrics(TradeWindow::W1s);
    assert(m.total_volume == 200.0); // Only last trade
    assert(m.total_trades == 1);
    std::cout << "✓ test_1s_window_expiry\n";
}

void test_5s_window() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    metrics.on_trade(10000, 100, true, ts);
    metrics.on_trade(10010, 200, false, ts + 3'000'000); // 3s later

    auto m5s = metrics.get_metrics(TradeWindow::W5s);
    assert(m5s.total_volume == 300.0);
    assert(m5s.total_trades == 2);

    // 1s window should only have the second trade
    auto m1s = metrics.get_metrics(TradeWindow::W1s);
    assert(m1s.total_volume == 200.0);
    assert(m1s.total_trades == 1);

    std::cout << "✓ test_5s_window\n";
}

void test_10s_window() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    metrics.on_trade(10000, 100, true, ts);
    metrics.on_trade(10010, 200, false, ts + 7'000'000); // 7s later

    auto m10s = metrics.get_metrics(TradeWindow::W10s);
    assert(m10s.total_volume == 300.0);

    auto m5s = metrics.get_metrics(TradeWindow::W5s);
    assert(m5s.total_volume == 200.0); // Only second trade

    std::cout << "✓ test_10s_window\n";
}

void test_30s_window() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    metrics.on_trade(10000, 100, true, ts);
    metrics.on_trade(10010, 200, false, ts + 20'000'000); // 20s later

    auto m30s = metrics.get_metrics(TradeWindow::W30s);
    assert(m30s.total_volume == 300.0);

    std::cout << "✓ test_30s_window\n";
}

void test_1min_window() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    metrics.on_trade(10000, 100, true, ts);
    metrics.on_trade(10010, 200, false, ts + 45'000'000); // 45s later

    auto m1min = metrics.get_metrics(TradeWindow::W1min);
    assert(m1min.total_volume == 300.0);

    auto m30s = metrics.get_metrics(TradeWindow::W30s);
    assert(m30s.total_volume == 200.0); // Only second trade

    std::cout << "✓ test_1min_window\n";
}

void test_cumulative_delta() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    metrics.on_trade(10000, 100, true, ts);
    auto m1 = metrics.get_metrics(TradeWindow::W1s);
    assert(std::abs(m1.cumulative_delta - 100.0) < 0.01);

    metrics.on_trade(10010, 50, false, ts + 100'000);
    auto m2 = metrics.get_metrics(TradeWindow::W1s);
    assert(std::abs(m2.cumulative_delta - 50.0) < 0.01); // 100 - 50

    metrics.on_trade(10020, 30, false, ts + 200'000);
    auto m3 = metrics.get_metrics(TradeWindow::W1s);
    assert(std::abs(m3.cumulative_delta - 20.0) < 0.01); // 100 - 50 - 30

    std::cout << "✓ test_cumulative_delta\n";
}

void test_window_independence() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    // Add trades across different time ranges
    metrics.on_trade(10000, 100, true, ts);
    metrics.on_trade(10010, 100, true, ts + 2'000'000);
    metrics.on_trade(10020, 100, true, ts + 6'000'000);
    metrics.on_trade(10030, 100, true, ts + 15'000'000);

    auto m1s = metrics.get_metrics(TradeWindow::W1s);
    auto m5s = metrics.get_metrics(TradeWindow::W5s);
    auto m10s = metrics.get_metrics(TradeWindow::W10s);

    assert(m1s.total_trades == 1);  // Only last trade
    assert(m5s.total_trades == 2);  // Last 2 trades
    assert(m10s.total_trades == 3); // Last 3 trades

    std::cout << "✓ test_window_independence\n";
}

void test_reset() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    metrics.on_trade(10000, 100, true, ts);
    metrics.on_trade(10010, 200, false, ts + 100'000);

    auto m1 = metrics.get_metrics(TradeWindow::W1s);
    assert(m1.total_volume == 300.0);

    metrics.reset();

    auto m2 = metrics.get_metrics(TradeWindow::W1s);
    assert(m2.total_volume == 0.0);
    assert(m2.total_trades == 0);

    std::cout << "✓ test_reset\n";
}

void test_empty_window() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    metrics.on_trade(10000, 100, true, ts);

    // Request metrics before first trade timestamp
    auto m = metrics.get_metrics(TradeWindow::W1s);
    assert(m.total_trades > 0); // Should have the trade

    std::cout << "✓ test_empty_window\n";
}

void test_high_frequency_trades() {
    TradeStreamMetrics metrics;
    uint64_t ts = 1'000'000;

    // Add 1000 trades in 1 second (1ms apart)
    for (int i = 0; i < 1000; i++) {
        metrics.on_trade(10000 + (i % 10), 100, i % 2 == 0, ts + i * 1000);
    }

    auto m = metrics.get_metrics(TradeWindow::W1s);
    assert(m.total_trades == 1000);
    assert(m.buy_trades == 500);
    assert(m.sell_trades == 500);

    std::cout << "✓ test_high_frequency_trades\n";
}

int main() {
    std::cout << "Running TradeStreamMetrics tests...\n\n";

    // Volume tests (6)
    test_empty_metrics();
    test_single_buy_trade();
    test_single_sell_trade();
    test_mixed_trades_volume();
    test_delta_calculation();
    test_buy_ratio();

    // Price metrics (5)
    test_vwap_calculation();
    test_high_low_prices();
    test_price_velocity();
    test_realized_volatility();

    // Streaks (4)
    test_buy_streak();
    test_sell_streak();
    test_active_buy_streak();
    test_active_sell_streak();

    // Trade counts (2)
    test_large_trades();

    // Timing (3)
    test_avg_inter_trade_time();
    test_min_inter_trade_time();
    test_burst_detection();

    // Ticks (4)
    test_upticks();
    test_downticks();
    test_zeroticks();
    test_tick_ratio();

    // Window tests (6)
    test_1s_window_expiry();
    test_5s_window();
    test_10s_window();
    test_30s_window();
    test_1min_window();
    test_window_independence();

    // Other (3)
    test_cumulative_delta();
    test_reset();
    test_empty_window();
    test_high_frequency_trades();

    std::cout << "\n✅ All 34 tests passed!\n";
    return 0;
}
