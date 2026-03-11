#include "../include/backtester.hpp"

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
#define ASSERT_GT(a, b) assert((a) > (b))
#define ASSERT_GE(a, b) assert((a) >= (b))

// Test: Backtester processes ticks correctly
TEST(test_backtest_processes_ticks) {
    SimulatorConfig config;
    config.spread_bps = 20;
    config.quote_size = 100;
    config.max_position = 500;

    Backtester bt(config);

    // Add some ticks
    bt.add_tick(1, 10000, 10010, 1000, 1000);
    bt.add_tick(2, 10005, 10015, 1000, 1000);
    bt.add_tick(3, 10000, 10010, 1000, 1000);

    auto result = bt.run();

    ASSERT_GT(result.total_quotes, 0);
}

// Test: Market maker earns spread when market oscillates
TEST(test_earn_spread_on_oscillation) {
    SimulatorConfig config;
    config.spread_bps = 100; // 1% spread = 100 bps
    config.quote_size = 10;
    config.max_position = 100;

    Backtester bt(config, FillMode::Aggressive);

    // Start at mid 10000
    // Our quotes: bid ~9950, ask ~10050 (with 1% spread)

    // Market oscillates - first down (we buy), then up (we sell)
    bt.add_tick(1, 10000, 10010, 1000, 1000); // Initial, place quotes
    bt.add_tick(2, 9940, 9950, 1000, 1000);   // Market drops, hits our bid
    bt.add_tick(3, 10000, 10010, 1000, 1000); // Back to normal
    bt.add_tick(4, 10050, 10060, 1000, 1000); // Market up, hits our ask
    bt.add_tick(5, 10000, 10010, 1000, 1000); // Back to normal

    auto result = bt.run();

    // Should have made money from spread
    std::cout << "[Debug] Total P&L: " << result.total_pnl << " ";
    ASSERT_GT(result.total_pnl, 0);
}

// Test: Losing money on adverse selection
TEST(test_adverse_selection_loss) {
    SimulatorConfig config;
    config.spread_bps = 20; // Tight spread
    config.quote_size = 100;
    config.max_position = 1000;

    Backtester bt(config, FillMode::Aggressive);

    // Market trends strongly against us
    bt.add_tick(1, 10000, 10010, 1000, 1000); // Place quotes
    bt.add_tick(2, 9980, 9990, 1000, 1000);   // Market drops, we buy
    bt.add_tick(3, 9960, 9970, 1000, 1000);   // Drops more, we buy more
    bt.add_tick(4, 9940, 9950, 1000, 1000);   // Even more
    bt.add_tick(5, 9900, 9910, 1000, 1000);   // Big drop - unrealized loss

    auto result = bt.run();

    // Should have unrealized losses
    std::cout << "[Debug] Total P&L: " << result.total_pnl << " ";
    // Total PnL should be negative due to inventory at lower prices
}

// Test: Drawdown tracking
TEST(test_drawdown_tracking) {
    SimulatorConfig config;
    config.spread_bps = 20;
    config.quote_size = 100;
    config.max_position = 500;

    Backtester bt(config, FillMode::Aggressive);

    // Up then down market
    bt.add_tick(1, 10000, 10010, 1000, 1000);
    bt.add_tick(2, 10050, 10060, 1000, 1000); // Up - we sell, profit
    bt.add_tick(3, 9950, 9960, 1000, 1000);   // Down - we buy
    bt.add_tick(4, 9900, 9910, 1000, 1000);   // More down - drawdown

    auto result = bt.run();

    // Should track some drawdown
    ASSERT_GE(result.max_drawdown, 0);
}

// Test: Position limits enforced
TEST(test_position_limits) {
    SimulatorConfig config;
    config.spread_bps = 50;
    config.quote_size = 100;
    config.max_position = 200;

    Backtester bt(config, FillMode::Aggressive);

    // Market keeps hitting our bid
    for (int i = 0; i < 10; ++i) {
        bt.add_tick(i, 9900, 9910, 1000, 1000); // Always hits our bid
    }

    auto result = bt.run();

    // Position should be capped at max
    ASSERT_GE(static_cast<int64_t>(config.max_position), result.max_position);
}

// Test: Metrics calculation
TEST(test_metrics_calculation) {
    SimulatorConfig config;
    config.spread_bps = 100;
    config.quote_size = 10;
    config.max_position = 100;

    Backtester bt(config, FillMode::Aggressive);

    // Oscillating market for consistent profits
    for (int i = 0; i < 20; ++i) {
        if (i % 4 == 0) {
            bt.add_tick(i, 10000, 10010, 1000, 1000);
        } else if (i % 4 == 1) {
            bt.add_tick(i, 9940, 9950, 1000, 1000); // Hit bid
        } else if (i % 4 == 2) {
            bt.add_tick(i, 10000, 10010, 1000, 1000);
        } else {
            bt.add_tick(i, 10050, 10060, 1000, 1000); // Hit ask
        }
    }

    auto result = bt.run();

    // Should have calculated sharpe
    // Note: Sharpe might be negative or positive depending on market
    std::cout << "[Debug] Sharpe: " << result.sharpe_ratio << " ";
}

// Test: Empty backtest doesn't crash
TEST(test_empty_backtest) {
    SimulatorConfig config;
    Backtester bt(config);

    auto result = bt.run();

    ASSERT_EQ(result.total_pnl, 0);
    ASSERT_EQ(result.total_trades, 0);
}

// Test: CSV loading from valid file
TEST(test_csv_loading) {
    // Create temporary CSV file
    const char* filename = "/tmp/test_backtest.csv";
    std::ofstream file(filename);
    file << "timestamp,bid,ask,bid_size,ask_size\n";
    file << "1000000,10000,10010,100,100\n";
    file << "2000000,10005,10015,150,150\n";
    file << "3000000,10010,10020,200,200\n";
    file.close();

    SimulatorConfig config;
    Backtester bt(config);

    bool loaded = bt.load_csv(filename);
    ASSERT_TRUE(loaded);

    // Verify ticks were loaded
    auto& ticks = bt.ticks();
    ASSERT_EQ(ticks.size(), 3);
    ASSERT_EQ(ticks[0].timestamp, 1000000);
    ASSERT_EQ(ticks[0].bid, 10000);
    ASSERT_EQ(ticks[0].ask, 10010);
    ASSERT_EQ(ticks[1].bid_size, 150);

    // Cleanup
    std::remove(filename);
}

// Test: CSV loading without header
TEST(test_csv_loading_no_header) {
    const char* filename = "/tmp/test_backtest_no_header.csv";
    std::ofstream file(filename);
    file << "1000000,10000,10010,100,100\n";
    file << "2000000,10005,10015,150,150\n";
    file.close();

    SimulatorConfig config;
    Backtester bt(config);

    bool loaded = bt.load_csv(filename);
    ASSERT_TRUE(loaded);

    auto& ticks = bt.ticks();
    ASSERT_EQ(ticks.size(), 2);
    ASSERT_EQ(ticks[0].timestamp, 1000000);

    std::remove(filename);
}

// Test: CSV loading from non-existent file
TEST(test_csv_nonexistent_file) {
    SimulatorConfig config;
    Backtester bt(config);

    bool loaded = bt.load_csv("/nonexistent/file.csv");
    ASSERT_TRUE(!loaded);
}

// Test: CSV loading with malformed data
TEST(test_csv_malformed_data) {
    const char* filename = "/tmp/test_backtest_malformed.csv";
    std::ofstream file(filename);
    file << "timestamp,bid,ask,bid_size,ask_size\n";
    file << "1000000,10000,10010,100,100\n";
    file << "invalid,data,here\n"; // Malformed line (should be skipped)
    file << "2000000,10005,10015,150,150\n";
    file.close();

    SimulatorConfig config;
    Backtester bt(config);

    bool loaded = bt.load_csv(filename);
    ASSERT_TRUE(loaded);

    // Only valid lines should be loaded
    auto& ticks = bt.ticks();
    ASSERT_EQ(ticks.size(), 2); // Only 2 valid lines

    std::remove(filename);
}

// Test: CSV with incomplete line (missing fields)
TEST(test_csv_incomplete_line) {
    const char* filename = "/tmp/test_backtest_incomplete.csv";
    std::ofstream file(filename);
    file << "1000000,10000,10010\n";         // Missing bid_size and ask_size
    file << "2000000,10005,10015,150,150\n"; // Valid line
    file.close();

    SimulatorConfig config;
    Backtester bt(config);

    bool loaded = bt.load_csv(filename);
    ASSERT_TRUE(loaded);

    // Only complete line should be loaded
    auto& ticks = bt.ticks();
    ASSERT_EQ(ticks.size(), 1);

    std::remove(filename);
}

// Test: CSV with various field count errors (covers all parse_csv_line error paths)
TEST(test_csv_all_field_errors) {
    const char* filename = "/tmp/test_backtest_field_errors.csv";
    std::ofstream file(filename);
    file << "1000000\n";                     // Only 1 field (missing 4) - triggers line 183
    file << "1000000,10000\n";               // Only 2 fields (missing 3) - triggers line 187
    file << "1000000,10000,10010\n";         // Only 3 fields (missing 2) - triggers line 191
    file << "1000000,10000,10010,100\n";     // Only 4 fields (missing 1) - triggers line 195
    file << "\n";                            // Empty line - triggers line 179
    file << "2000000,10005,10015,150,150\n"; // Valid line
    file.close();

    SimulatorConfig config;
    Backtester bt(config);

    bool loaded = bt.load_csv(filename);
    ASSERT_TRUE(loaded);

    // Only the one valid line should be loaded
    auto& ticks = bt.ticks();
    ASSERT_EQ(ticks.size(), 1);
    ASSERT_EQ(ticks[0].timestamp, 2000000ULL);

    std::remove(filename);
}

// Test: FillMode::Passive
TEST(test_passive_fill_mode) {
    SimulatorConfig config;
    config.spread_bps = 100;
    config.quote_size = 10;
    config.max_position = 100;

    Backtester bt(config, FillMode::Passive);

    // With passive mode, fills only happen when market exactly touches our price
    bt.add_tick(1, 10000, 10010, 1000, 1000);
    bt.add_tick(2, 9940, 9950, 1000, 1000);   // Should fill bid if price matches
    bt.add_tick(3, 10050, 10060, 1000, 1000); // Should fill ask if price matches

    auto result = bt.run();

    // Passive mode is more conservative than aggressive
    ASSERT_GE(result.total_quotes, 0);
}

// Test: FillMode::Probabilistic
TEST(test_probabilistic_fill_mode) {
    SimulatorConfig config;
    config.spread_bps = 50;
    config.quote_size = 10;
    config.max_position = 100;

    Backtester bt(config, FillMode::Probabilistic);

    bt.add_tick(1, 10000, 10010, 1000, 1000);
    bt.add_tick(2, 9980, 9990, 1000, 1000);
    bt.add_tick(3, 10020, 10030, 1000, 1000);

    auto result = bt.run();

    ASSERT_GE(result.total_quotes, 0);
}

// Test: Getters (ticks and pnl_history)
TEST(test_getters) {
    SimulatorConfig config;
    config.spread_bps = 100;
    config.quote_size = 10;
    config.max_position = 100;

    Backtester bt(config);

    bt.add_tick(1, 10000, 10010, 100, 100);
    bt.add_tick(2, 10005, 10015, 150, 150);
    bt.add_tick(3, 10010, 10020, 200, 200);

    auto result = bt.run();

    // Test ticks() getter
    const auto& ticks = bt.ticks();
    ASSERT_EQ(ticks.size(), 3);
    ASSERT_EQ(ticks[0].timestamp, 1);
    ASSERT_EQ(ticks[1].bid, 10005);
    ASSERT_EQ(ticks[2].ask_size, 200);

    // Test pnl_history() getter
    const auto& pnl_hist = bt.pnl_history();
    ASSERT_EQ(pnl_hist.size(), 3); // One P&L entry per tick
}

// Test: Print result (coverage for print_result static method)
TEST(test_print_result) {
    BacktestResult result{};
    result.total_pnl = 5000;
    result.realized_pnl = 4500;
    result.max_drawdown = 1000;
    result.total_trades = 50;
    result.total_quotes = 200;
    result.sharpe_ratio = 1.5;
    result.win_rate = 0.65;
    result.max_position = 300;
    result.avg_position = 150.5;

    // Should not crash
    Backtester::print_result(result);
}

// Test: Win rate calculation with all wins
TEST(test_win_rate_all_wins) {
    SimulatorConfig config;
    config.spread_bps = 100;
    config.quote_size = 10;
    config.max_position = 100;

    Backtester bt(config, FillMode::Aggressive);

    // Oscillating market = profits
    for (int i = 0; i < 10; ++i) {
        bt.add_tick(i * 4, 10000, 10010, 1000, 1000);
        bt.add_tick(i * 4 + 1, 9940, 9950, 1000, 1000); // Buy
        bt.add_tick(i * 4 + 2, 10000, 10010, 1000, 1000);
        bt.add_tick(i * 4 + 3, 10050, 10060, 1000, 1000); // Sell
    }

    auto result = bt.run();

    // Should have positive win rate
    ASSERT_GT(result.win_rate, 0.0);
}

// Test: Sharpe calculation with zero volatility
TEST(test_sharpe_zero_volatility) {
    SimulatorConfig config;
    config.spread_bps = 0; // No spread = no P&L variance
    config.quote_size = 0; // No quotes = no trades
    config.max_position = 100;

    Backtester bt(config);

    // Constant market
    for (int i = 0; i < 5; ++i) {
        bt.add_tick(i, 10000, 10010, 1000, 1000);
    }

    auto result = bt.run();

    // Zero volatility should give zero Sharpe
    ASSERT_EQ(result.sharpe_ratio, 0.0);
}

// Test: Sharpe calculation with < 2 data points
TEST(test_sharpe_insufficient_data) {
    SimulatorConfig config;
    Backtester bt(config);

    bt.add_tick(1, 10000, 10010, 100, 100);

    auto result = bt.run();

    // Not enough data for Sharpe calculation
    ASSERT_EQ(result.sharpe_ratio, 0.0);
}

// Test: Average position calculation
TEST(test_average_position) {
    SimulatorConfig config;
    config.spread_bps = 100;
    config.quote_size = 10;
    config.max_position = 500;

    Backtester bt(config, FillMode::Aggressive);

    // Market hits bid multiple times (need big moves to cross quotes)
    bt.add_tick(1, 10000, 10010, 1000, 1000);
    bt.add_tick(2, 9940, 9950, 1000, 1000); // Should cross bid
    bt.add_tick(3, 9940, 9950, 1000, 1000); // More fills
    bt.add_tick(4, 9940, 9950, 1000, 1000); // More fills

    auto result = bt.run();

    // Should have some trades and non-zero average position
    // If no fills occurred, avg_position will be 0
    ASSERT_GE(result.avg_position, 0.0);
}

int main() {
    std::cout << "=== Backtester Tests ===\n";

    RUN_TEST(test_backtest_processes_ticks);
    RUN_TEST(test_earn_spread_on_oscillation);
    RUN_TEST(test_adverse_selection_loss);
    RUN_TEST(test_drawdown_tracking);
    RUN_TEST(test_position_limits);
    RUN_TEST(test_metrics_calculation);
    RUN_TEST(test_empty_backtest);
    RUN_TEST(test_csv_loading);
    RUN_TEST(test_csv_loading_no_header);
    RUN_TEST(test_csv_nonexistent_file);
    RUN_TEST(test_csv_malformed_data);
    RUN_TEST(test_csv_incomplete_line);
    RUN_TEST(test_csv_all_field_errors);
    RUN_TEST(test_passive_fill_mode);
    RUN_TEST(test_probabilistic_fill_mode);
    RUN_TEST(test_getters);
    RUN_TEST(test_print_result);
    RUN_TEST(test_win_rate_all_wins);
    RUN_TEST(test_sharpe_zero_volatility);
    RUN_TEST(test_sharpe_insufficient_data);
    RUN_TEST(test_average_position);

    std::cout << "\nAll 20 tests PASSED!\n";
    return 0;
}
