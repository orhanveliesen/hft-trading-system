/**
 * Rolling Sharpe Ratio Tests
 */

#include "strategy/rolling_sharpe.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <random>

using namespace hft::strategy;

// Helper to compare floats
bool approx_equal(double a, double b, double eps = 0.01) {
    return std::abs(a - b) < eps;
}

// ============================================================================
// Test: Basic Statistics
// ============================================================================
void test_basic_stats() {
    std::cout << "  test_basic_stats... ";

    RollingSharpe<10> sharpe(0); // No risk-free rate for simplicity

    // Add known returns: 1%, 2%, 3%, 4%, 5%
    sharpe.add_return(0.01);
    sharpe.add_return(0.02);
    sharpe.add_return(0.03);
    sharpe.add_return(0.04);
    sharpe.add_return(0.05);

    assert(sharpe.count() == 5);

    // Mean should be 3%
    assert(approx_equal(sharpe.mean(), 0.03));

    // Variance of [0.01, 0.02, 0.03, 0.04, 0.05] with mean=0.03
    // Squared diffs: (0.01-0.03)^2=0.0004, (0.02-0.03)^2=0.0001, 0, 0.0001, 0.0004
    // Sum = 0.001, sample variance = 0.001/4 = 0.00025
    double expected_var = 0.0004 + 0.0001 + 0 + 0.0001 + 0.0004; // Sum of squared diffs
    expected_var /= 4;                                           // n-1
    assert(approx_equal(sharpe.variance(), expected_var, 0.00001));

    std::cout << "PASSED\n";
}

// ============================================================================
// Test: Rolling Window
// ============================================================================
void test_rolling_window() {
    std::cout << "  test_rolling_window... ";

    RollingSharpe<5> sharpe(0);

    // Fill window with 1% returns
    for (int i = 0; i < 5; ++i) {
        sharpe.add_return(0.01);
    }

    assert(sharpe.is_ready());
    assert(approx_equal(sharpe.mean(), 0.01));
    assert(approx_equal(sharpe.std_dev(), 0, 0.0001)); // All same = 0 std

    // Now add a 6% return - oldest (1%) should be removed
    sharpe.add_return(0.06);

    // New window: [0.01, 0.01, 0.01, 0.01, 0.06]
    // Mean = (4*0.01 + 0.06) / 5 = 0.10 / 5 = 0.02
    assert(approx_equal(sharpe.mean(), 0.02));
    assert(sharpe.count() == 5);

    std::cout << "PASSED\n";
}

// ============================================================================
// Test: Sharpe Ratio Calculation
// ============================================================================
void test_sharpe_ratio() {
    std::cout << "  test_sharpe_ratio... ";

    RollingSharpe<100> sharpe(0);

    // Add consistent positive returns (good strategy)
    for (int i = 0; i < 50; ++i) {
        sharpe.add_return(0.005); // 0.5% per trade
    }

    // With zero variance, Sharpe is undefined (returns 0)
    assert(sharpe.sharpe_ratio() == 0);

    // Add some variance
    for (int i = 0; i < 50; ++i) {
        sharpe.add_return(i % 2 == 0 ? 0.008 : 0.002); // Alternating
    }

    // Now we have variance, Sharpe should be positive
    double s = sharpe.sharpe_ratio();
    assert(s > 0); // Positive returns = positive Sharpe

    std::cout << "PASSED (Sharpe=" << s << ")\n";
}

// ============================================================================
// Test: Negative Sharpe (Losing Strategy)
// ============================================================================
void test_negative_sharpe() {
    std::cout << "  test_negative_sharpe... ";

    RollingSharpe<50> sharpe(0);

    // Losing strategy: -0.5% per trade with some variance
    std::mt19937 rng(42);
    std::normal_distribution<double> dist(-0.005, 0.01);

    for (int i = 0; i < 50; ++i) {
        sharpe.add_return(dist(rng));
    }

    double s = sharpe.sharpe_ratio();
    assert(s < 0); // Negative mean = negative Sharpe

    // Should recommend not trading
    assert(!sharpe.should_trade());
    assert(sharpe.position_multiplier() == 0);

    std::cout << "PASSED (Sharpe=" << s << ")\n";
}

// ============================================================================
// Test: Position Sizing Based on Sharpe
// ============================================================================
void test_position_sizing() {
    std::cout << "  test_position_sizing... ";

    RollingSharpe<30> sharpe(0);

    // Start with neutral
    for (int i = 0; i < 30; ++i) {
        sharpe.add_return(0.001 + 0.001 * (i % 3)); // Small positive, some variance
    }

    double mult = sharpe.position_multiplier();
    assert(mult >= 0 && mult <= 1.5);

    auto stats = sharpe.get_stats();
    std::cout << "PASSED (Sharpe=" << stats.sharpe << ", mult=" << stats.position_mult << ")\n";
}

// ============================================================================
// Test: Trade Return Calculator
// ============================================================================
void test_trade_return() {
    std::cout << "  test_trade_return... ";

    // Long trade: buy 100, sell 110 = 10% return
    TradeReturn long_trade{100.0, 110.0, 1.0, true};
    assert(approx_equal(long_trade.return_pct(), 0.10));
    assert(approx_equal(long_trade.pnl(), 10.0));

    // Short trade: sell 100, buy back 90 = 10% return
    TradeReturn short_trade{100.0, 90.0, 1.0, false};
    assert(approx_equal(short_trade.return_pct(), 0.10));
    assert(approx_equal(short_trade.pnl(), 10.0));

    // Losing long: buy 100, sell 95 = -5% return
    TradeReturn losing_long{100.0, 95.0, 2.0, true};
    assert(approx_equal(losing_long.return_pct(), -0.05));
    assert(approx_equal(losing_long.pnl(), -10.0));

    std::cout << "PASSED\n";
}

// ============================================================================
// Test: Welford Algorithm Stability
// ============================================================================
void test_numerical_stability() {
    std::cout << "  test_numerical_stability... ";

    RollingSharpe<1000> sharpe(0);

    // Add many similar values (can cause numerical issues in naive algorithms)
    for (int i = 0; i < 10000; ++i) {
        sharpe.add_return(1000000.001 + 0.0001 * (i % 10));
    }

    // Should not overflow or become NaN
    assert(!std::isnan(sharpe.mean()));
    assert(!std::isnan(sharpe.std_dev()));
    assert(!std::isnan(sharpe.sharpe_ratio()));
    assert(!std::isinf(sharpe.variance()));

    std::cout << "PASSED\n";
}

// ============================================================================
// Test: Integration with Real Trading Scenario
// ============================================================================
void test_trading_scenario() {
    std::cout << "  test_trading_scenario... ";

    RollingSharpe<100> sharpe(0);

    // Simulate a strategy that:
    // - Wins 55% of trades
    // - Wins average 1%, loses average 0.8%
    std::mt19937 rng(123);
    std::uniform_real_distribution<double> win_dist(0, 1);
    std::normal_distribution<double> win_return(0.01, 0.002);
    std::normal_distribution<double> loss_return(-0.008, 0.002);

    int wins = 0, losses = 0;

    for (int i = 0; i < 200; ++i) {
        double r;
        if (win_dist(rng) < 0.55) {
            r = win_return(rng);
            wins++;
        } else {
            r = loss_return(rng);
            losses++;
        }
        sharpe.add_return(r);
    }

    // This should be a profitable strategy
    double s = sharpe.sharpe_ratio();
    assert(sharpe.should_trade());
    assert(s > 0);

    double win_rate = 100.0 * wins / (wins + losses);

    std::cout << "PASSED\n";
    std::cout << "    Win rate: " << win_rate << "%\n";
    std::cout << "    Mean return: " << (sharpe.mean() * 100) << "%\n";
    std::cout << "    Std dev: " << (sharpe.std_dev() * 100) << "%\n";
    std::cout << "    Sharpe: " << s << "\n";
    std::cout << "    Annualized: " << sharpe.annualized_sharpe() << "\n";
    std::cout << "    Position mult: " << sharpe.position_multiplier() << "x\n";
}

// ============================================================================
// Main
// ============================================================================
int main() {
    std::cout << "\n=== Rolling Sharpe Tests ===\n\n";

    test_basic_stats();
    test_rolling_window();
    test_sharpe_ratio();
    test_negative_sharpe();
    test_position_sizing();
    test_trade_return();
    test_numerical_stability();
    test_trading_scenario();

    std::cout << "\n=== All tests passed! ===\n\n";

    std::cout << "How to use in live trading:\n";
    std::cout << "  1. After each trade closes, call sharpe.add_return(return_pct)\n";
    std::cout << "  2. Check sharpe.should_trade() before new trades\n";
    std::cout << "  3. Scale position with sharpe.position_multiplier()\n";
    std::cout << "  4. Monitor sharpe.sharpe_ratio() for strategy health\n\n";

    return 0;
}
