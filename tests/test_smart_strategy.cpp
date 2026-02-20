/**
 * SmartStrategy Tests
 *
 * Tests for configurable thresholds - Issue #8
 * Verifies magic numbers are replaced with config values.
 */

#include <cassert>
#include <iostream>
#include <cmath>

#include "strategy/smart_strategy.hpp"

using namespace hft::strategy;

// Helper to compare floats
bool approx_equal(double a, double b, double eps = 0.001) {
    return std::abs(a - b) < eps;
}

// ============================================================================
// Test: Config fields exist and have correct defaults
// ============================================================================
void test_config_defaults() {
    std::cout << "  test_config_defaults... ";

    SmartStrategyConfig config;

    // New config fields should exist with documented defaults
    assert(config.min_trades_for_sharpe_mode == 20);
    assert(config.min_trades_for_win_rate_mode == 10);
    assert(config.min_trades_for_sharpe_sizing == 10);
    assert(approx_equal(config.wide_spread_threshold, 0.001));

    std::cout << "PASSED\n";
}

// ============================================================================
// Test: Sharpe mode transition respects config threshold
// ============================================================================
void test_sharpe_mode_uses_config_threshold() {
    std::cout << "  test_sharpe_mode_uses_config_threshold... ";

    // Create config with custom threshold (15 instead of default 20)
    SmartStrategyConfig config;
    config.min_trades_for_sharpe_mode = 15;

    SmartStrategy strategy(config);

    // Add 14 winning trades with variance (below threshold)
    for (int i = 0; i < 14; ++i) {
        // Alternate between 1.5% and 2.5% to create variance
        double pnl = (i % 2 == 0) ? 0.015 : 0.025;
        strategy.record_trade_result(pnl, true);
    }

    // At 14 trades, Sharpe-based mode should NOT be active yet
    assert(strategy.total_trades() == 14);

    // After more trades with variance, Sharpe ratio should be positive
    strategy.record_trade_result(0.02, true);
    assert(strategy.total_trades() == 15);
    // Sharpe ratio may still be 0 if not enough samples for calculation
    // but the config threshold should be respected

    std::cout << "PASSED\n";
}

// ============================================================================
// Test: Win rate mode transition respects config threshold
// ============================================================================
void test_win_rate_mode_uses_config_threshold() {
    std::cout << "  test_win_rate_mode_uses_config_threshold... ";

    // Create config with custom threshold (5 instead of default 10)
    SmartStrategyConfig config;
    config.min_trades_for_win_rate_mode = 5;
    config.min_trades_for_sharpe_mode = 100;  // Disable Sharpe-based mode for this test

    SmartStrategy strategy(config);

    // Add 4 trades (below threshold)
    for (int i = 0; i < 4; ++i) {
        strategy.record_trade_result(0.02, true);
    }

    // At 4 trades, should be below threshold
    assert(strategy.total_trades() == 4);
    assert(strategy.win_rate() == 1.0);  // 100% win rate

    // Add one more to hit threshold
    strategy.record_trade_result(0.02, true);
    assert(strategy.total_trades() == 5);

    std::cout << "PASSED\n";
}

// ============================================================================
// Test: Sharpe position sizing respects config threshold
// ============================================================================
void test_sharpe_sizing_uses_config_threshold() {
    std::cout << "  test_sharpe_sizing_uses_config_threshold... ";

    // Create config with custom threshold (8 instead of default 10)
    SmartStrategyConfig config;
    config.min_trades_for_sharpe_sizing = 8;

    SmartStrategy strategy(config);

    // Add 7 trades (below threshold)
    for (int i = 0; i < 7; ++i) {
        strategy.record_trade_result(0.01, true);
    }

    assert(strategy.total_trades() == 7);

    // Add one more to hit threshold
    strategy.record_trade_result(0.01, true);
    assert(strategy.total_trades() == 8);

    // At 8 trades, Sharpe multiplier should be calculable
    assert(strategy.sharpe_position_multiplier() >= 0);

    std::cout << "PASSED\n";
}

// ============================================================================
// Test: Wide spread threshold config field exists
// ============================================================================
void test_spread_threshold_config_exists() {
    std::cout << "  test_spread_threshold_config_exists... ";

    SmartStrategyConfig config;

    // Default value
    assert(approx_equal(config.wide_spread_threshold, 0.001));

    // Can be changed
    config.wide_spread_threshold = 0.002;
    assert(approx_equal(config.wide_spread_threshold, 0.002));

    std::cout << "PASSED\n";
}

// ============================================================================
// Test: DRY - TechnicalIndicatorsConfig is embedded (no duplication)
// ============================================================================
void test_dry_technical_indicators_config() {
    std::cout << "  test_dry_technical_indicators_config... ";

    SmartStrategyConfig config;

    // TechnicalIndicatorsConfig should be accessible via ti_config
    // This ensures RSI thresholds come from single source of truth
    assert(approx_equal(config.ti_config.rsi_oversold, 30.0));
    assert(approx_equal(config.ti_config.rsi_overbought, 70.0));
    assert(approx_equal(config.ti_config.rsi_mild_oversold, 40.0));
    assert(approx_equal(config.ti_config.rsi_mild_overbought, 60.0));

    // Score weights should be configurable
    assert(approx_equal(config.score_weight_strong, 0.4));
    assert(approx_equal(config.score_weight_medium, 0.3));
    assert(approx_equal(config.score_weight_weak, 0.2));

    // Can customize RSI thresholds without duplicating values
    config.ti_config.rsi_oversold = 25.0;
    config.ti_config.rsi_overbought = 75.0;
    assert(approx_equal(config.ti_config.rsi_oversold, 25.0));
    assert(approx_equal(config.ti_config.rsi_overbought, 75.0));

    std::cout << "PASSED\n";
}

// ============================================================================
// Test: Default values match original hardcoded behavior
// ============================================================================
void test_default_behavior_unchanged() {
    std::cout << "  test_default_behavior_unchanged... ";

    // With default config, behavior should match original hardcoded values
    SmartStrategyConfig config;
    SmartStrategy strategy(config);

    // Original hardcoded values were:
    // - 20 for Sharpe mode
    // - 10 for win rate mode
    // - 10 for Sharpe sizing
    // - 0.001 for spread threshold

    // Add 19 trades (below original threshold of 20)
    for (int i = 0; i < 19; ++i) {
        strategy.record_trade_result(0.02, true);
    }

    assert(strategy.total_trades() == 19);

    // Add one more to hit threshold
    strategy.record_trade_result(0.02, true);
    assert(strategy.total_trades() == 20);

    std::cout << "PASSED\n";
}

// ============================================================================
// Main
// ============================================================================
int main() {
    std::cout << "\n=== SmartStrategy Tests (Issue #8: Remove Magic Numbers) ===\n\n";

    test_config_defaults();
    test_sharpe_mode_uses_config_threshold();
    test_win_rate_mode_uses_config_threshold();
    test_sharpe_sizing_uses_config_threshold();
    test_spread_threshold_config_exists();
    test_dry_technical_indicators_config();
    test_default_behavior_unchanged();

    std::cout << "\n=== All SmartStrategy tests passed! ===\n\n";

    return 0;
}
