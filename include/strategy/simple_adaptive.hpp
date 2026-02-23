#pragma once

#include "../backtest/kline_backtest.hpp"
#include "../backtest/strategies.hpp"
#include "regime_detector.hpp"

#include <iomanip>
#include <iostream>
#include <memory>

namespace hft {
namespace strategy {

/**
 * Simple Adaptive Strategy
 *
 * Switches between Mean Reversion (for ranging) and Breakout (for trending).
 * Much simpler than the full AdaptiveStrategy - no factory pattern.
 */
class SimpleAdaptive : public backtest::IStrategy {
public:
    struct Config {
        int regime_lookback = 20;
        int min_bars_before_switch = 10;
        bool verbose = false;

        // Strategy params
        int mr_lookback = 20;
        double mr_std_mult = 2.0;
        int breakout_lookback = 20;
    };

    SimpleAdaptive() : SimpleAdaptive(Config{}) {}

    explicit SimpleAdaptive(const Config& config)
        : config_(config), regime_detector_(make_regime_config(config.regime_lookback)),
          mr_strategy_(config.mr_lookback, config.mr_std_mult), breakout_strategy_(config.breakout_lookback),
          using_mean_reversion_(true), bars_since_switch_(0), switch_count_(0) {}

private:
    static RegimeConfig make_regime_config(int lookback) {
        RegimeConfig cfg;
        cfg.lookback = lookback;
        return cfg;
    }

public:
    void on_start(double capital) override {
        regime_detector_.reset();
        using_mean_reversion_ = true; // Start with mean reversion
        bars_since_switch_ = 0;
        switch_count_ = 0;
    }

    backtest::Signal on_kline(const exchange::Kline& kline, const backtest::BacktestPosition& position) override {
        // Update regime detector
        regime_detector_.update(kline);

        MarketRegime regime = regime_detector_.current_regime();
        bars_since_switch_++;

        // Check if we should switch
        if (bars_since_switch_ >= config_.min_bars_before_switch) {
            bool should_use_mr = regime_detector_.is_mean_reverting() || regime == MarketRegime::LowVolatility ||
                                 regime == MarketRegime::HighVolatility;

            if (should_use_mr != using_mean_reversion_) {
                if (config_.verbose) {
                    std::cout << "[SWITCH] " << (using_mean_reversion_ ? "MeanReversion" : "Breakout") << " -> "
                              << (should_use_mr ? "MeanReversion" : "Breakout")
                              << " (regime: " << regime_to_string(regime) << ", trend: " << std::fixed
                              << std::setprecision(2) << regime_detector_.trend_strength()
                              << ", vol: " << regime_detector_.volatility() << ")\n";
                }
                using_mean_reversion_ = should_use_mr;
                bars_since_switch_ = 0;
                switch_count_++;
            }
        }

        // Delegate to active strategy
        if (using_mean_reversion_) {
            return mr_strategy_.on_kline(kline, position);
        } else {
            return breakout_strategy_.on_kline(kline, position);
        }
    }

    // Getters
    bool is_using_mean_reversion() const { return using_mean_reversion_; }
    int switch_count() const { return switch_count_; }
    MarketRegime current_regime() const { return regime_detector_.current_regime(); }
    std::string active_strategy_name() const { return using_mean_reversion_ ? "MeanReversion" : "Breakout"; }

private:
    Config config_;
    RegimeDetector regime_detector_;

    // The two strategies we switch between
    backtest::MeanReversion mr_strategy_;
    backtest::BreakoutStrategy breakout_strategy_;

    bool using_mean_reversion_;
    int bars_since_switch_;
    int switch_count_;
};

} // namespace strategy
} // namespace hft
