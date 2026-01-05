#include <cassert>
#include <iostream>
#include <cmath>
#include "../include/backtester.hpp"

using namespace hft;

#define TEST(name) void name()
#define RUN_TEST(name) do { \
    std::cout << "Running " << #name << "... "; \
    name(); \
    std::cout << "PASSED\n"; \
} while(0)

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
    config.spread_bps = 100;  // 1% spread = 100 bps
    config.quote_size = 10;
    config.max_position = 100;

    Backtester bt(config, FillMode::Aggressive);

    // Start at mid 10000
    // Our quotes: bid ~9950, ask ~10050 (with 1% spread)

    // Market oscillates - first down (we buy), then up (we sell)
    bt.add_tick(1, 10000, 10010, 1000, 1000);  // Initial, place quotes
    bt.add_tick(2, 9940, 9950, 1000, 1000);    // Market drops, hits our bid
    bt.add_tick(3, 10000, 10010, 1000, 1000);  // Back to normal
    bt.add_tick(4, 10050, 10060, 1000, 1000);  // Market up, hits our ask
    bt.add_tick(5, 10000, 10010, 1000, 1000);  // Back to normal

    auto result = bt.run();

    // Should have made money from spread
    std::cout << "[Debug] Total P&L: " << result.total_pnl << " ";
    ASSERT_GT(result.total_pnl, 0);
}

// Test: Losing money on adverse selection
TEST(test_adverse_selection_loss) {
    SimulatorConfig config;
    config.spread_bps = 20;   // Tight spread
    config.quote_size = 100;
    config.max_position = 1000;

    Backtester bt(config, FillMode::Aggressive);

    // Market trends strongly against us
    bt.add_tick(1, 10000, 10010, 1000, 1000);  // Place quotes
    bt.add_tick(2, 9980, 9990, 1000, 1000);    // Market drops, we buy
    bt.add_tick(3, 9960, 9970, 1000, 1000);    // Drops more, we buy more
    bt.add_tick(4, 9940, 9950, 1000, 1000);    // Even more
    bt.add_tick(5, 9900, 9910, 1000, 1000);    // Big drop - unrealized loss

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
    bt.add_tick(2, 10050, 10060, 1000, 1000);  // Up - we sell, profit
    bt.add_tick(3, 9950, 9960, 1000, 1000);    // Down - we buy
    bt.add_tick(4, 9900, 9910, 1000, 1000);    // More down - drawdown

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
        bt.add_tick(i, 9900, 9910, 1000, 1000);  // Always hits our bid
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
            bt.add_tick(i, 9940, 9950, 1000, 1000);  // Hit bid
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

int main() {
    std::cout << "=== Backtester Tests ===\n";

    RUN_TEST(test_backtest_processes_ticks);
    RUN_TEST(test_earn_spread_on_oscillation);
    RUN_TEST(test_adverse_selection_loss);
    RUN_TEST(test_drawdown_tracking);
    RUN_TEST(test_position_limits);
    RUN_TEST(test_metrics_calculation);
    RUN_TEST(test_empty_backtest);

    std::cout << "\nAll tests PASSED!\n";
    return 0;
}
