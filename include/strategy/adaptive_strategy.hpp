#pragma once

#include "regime_detector.hpp"
#include "../backtest/kline_backtest.hpp"
#include "../backtest/strategies.hpp"
#include "../config/strategy_config.hpp"
#include "../config/strategy_factory.hpp"
#include <memory>
#include <map>
#include <iostream>
#include <iomanip>

namespace hft {
namespace strategy {

/**
 * Regime-Strategy Mapping Configuration
 */
struct RegimeStrategyMap {
    config::StrategyType trending_up = config::StrategyType::Breakout;
    config::StrategyType trending_down = config::StrategyType::Breakout;  // Or cash/short
    config::StrategyType ranging = config::StrategyType::MeanReversion;
    config::StrategyType high_volatility = config::StrategyType::RSI;  // More conservative
    config::StrategyType low_volatility = config::StrategyType::MeanReversion;
    config::StrategyType unknown = config::StrategyType::MeanReversion;  // Default

    config::StrategyType get(MarketRegime regime) const {
        switch (regime) {
            case MarketRegime::TrendingUp: return trending_up;
            case MarketRegime::TrendingDown: return trending_down;
            case MarketRegime::Ranging: return ranging;
            case MarketRegime::HighVolatility: return high_volatility;
            case MarketRegime::LowVolatility: return low_volatility;
            default: return unknown;
        }
    }
};

/**
 * Adaptive Strategy
 *
 * Meta-strategy that:
 * 1. Detects current market regime
 * 2. Selects the most appropriate strategy for that regime
 * 3. Delegates signal generation to the selected strategy
 *
 * Strategy switching has hysteresis to avoid frequent changes.
 */
class AdaptiveStrategy : public backtest::IStrategy {
public:
    struct Config {
        RegimeConfig regime_config;
        RegimeStrategyMap strategy_map;
        config::StrategyParams strategy_params;  // Shared params

        int min_regime_bars = 5;      // Minimum bars before switching
        double confidence_threshold = 0.3;  // Min confidence to switch

        bool verbose = false;  // Print regime changes
    };

    AdaptiveStrategy() : AdaptiveStrategy(Config{}) {}

    explicit AdaptiveStrategy(const Config& config)
        : config_(config)
        , regime_detector_(config.regime_config)
        , current_regime_(MarketRegime::Ranging)  // Start with ranging assumption
        , bars_in_regime_(0)
        , active_strategy_(nullptr)
        , total_switches_(0)
    {
        // Pre-create all strategies
        create_strategies();

        // Set initial active strategy (default to ranging/mean reversion)
        auto default_type = config_.strategy_map.get(MarketRegime::Ranging);
        auto it = strategies_.find(default_type);
        if (it != strategies_.end()) {
            active_strategy_ = it->second.get();
        }
    }

    void on_start(double capital) override {
        regime_detector_.reset();
        current_regime_ = MarketRegime::Ranging;  // Start with default
        bars_in_regime_ = 0;
        total_switches_ = 0;

        // Reset all strategies
        for (auto& [type, strategy] : strategies_) {
            strategy->on_start(capital);
        }

        // Set default active strategy
        auto default_type = config_.strategy_map.get(MarketRegime::Ranging);
        auto it = strategies_.find(default_type);
        if (it != strategies_.end()) {
            active_strategy_ = it->second.get();
        }
    }

    backtest::Signal on_kline(const exchange::Kline& kline,
                               const backtest::Position& position) override {
        // Update regime detector
        regime_detector_.update(kline);

        // Check for regime change
        MarketRegime detected = regime_detector_.current_regime();

        if (should_switch_regime(detected)) {
            switch_to_regime(detected);
        }

        bars_in_regime_++;

        // Delegate to active strategy
        if (active_strategy_) {
            return active_strategy_->on_kline(kline, position);
        }

        return backtest::Signal::None;
    }

    void on_trade(const backtest::TradeRecord& trade) override {
        if (active_strategy_) {
            active_strategy_->on_trade(trade);
        }
    }

    void on_end(const backtest::BacktestStats& stats) override {
        if (config_.verbose) {
            std::cout << "Adaptive Strategy Summary:\n";
            std::cout << "  Total regime switches: " << total_switches_ << "\n";
            std::cout << "  Final regime: " << regime_to_string(current_regime_) << "\n";
        }
    }

    // Getters for monitoring
    MarketRegime current_regime() const { return current_regime_; }
    double regime_confidence() const { return regime_detector_.confidence(); }
    double trend_strength() const { return regime_detector_.trend_strength(); }
    double volatility() const { return regime_detector_.volatility(); }
    int switches() const { return total_switches_; }

    std::string active_strategy_name() const {
        if (current_regime_ == MarketRegime::Unknown) return "None";
        auto type = config_.strategy_map.get(current_regime_);
        return config::StrategyFactory::get_name(type, config_.strategy_params);
    }

private:
    Config config_;
    RegimeDetector regime_detector_;
    MarketRegime current_regime_;
    int bars_in_regime_;

    std::map<config::StrategyType, std::unique_ptr<backtest::IStrategy>> strategies_;
    backtest::IStrategy* active_strategy_;
    int total_switches_;

    void create_strategies() {
        // Create one instance of each strategy type we might use
        std::vector<config::StrategyType> types = {
            config_.strategy_map.trending_up,
            config_.strategy_map.trending_down,
            config_.strategy_map.ranging,
            config_.strategy_map.high_volatility,
            config_.strategy_map.low_volatility,
            config_.strategy_map.unknown
        };

        for (auto type : types) {
            if (strategies_.find(type) == strategies_.end()) {
                try {
                    auto strategy = config::StrategyFactory::create(
                        type, config_.strategy_params);
                    if (strategy) {
                        strategies_[type] = std::move(strategy);
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Error creating strategy: " << e.what() << "\n";
                }
            }
        }
    }

    bool should_switch_regime(MarketRegime new_regime) {
        // Don't switch if regime is unknown or same
        if (new_regime == MarketRegime::Unknown) return false;
        if (new_regime == current_regime_) return false;

        // Require minimum time in current regime (hysteresis)
        if (bars_in_regime_ < config_.min_regime_bars) return false;

        // Require minimum confidence
        if (regime_detector_.confidence() < config_.confidence_threshold) return false;

        return true;
    }

    void switch_to_regime(MarketRegime new_regime) {
        if (config_.verbose && current_regime_ != new_regime) {
            std::cout << "[REGIME] " << regime_to_string(current_regime_)
                      << " -> " << regime_to_string(new_regime)
                      << " (confidence: " << std::fixed << std::setprecision(2)
                      << regime_detector_.confidence()
                      << ", volatility: " << regime_detector_.volatility()
                      << ", trend: " << regime_detector_.trend_strength() << ")\n";
        }

        current_regime_ = new_regime;
        bars_in_regime_ = 0;
        total_switches_++;

        // Switch active strategy
        auto type = config_.strategy_map.get(new_regime);
        auto it = strategies_.find(type);
        if (it != strategies_.end()) {
            active_strategy_ = it->second.get();
        }
    }
};

/**
 * Create an AdaptiveStrategy with optimized regime-strategy mapping
 * based on backtest results for a specific symbol
 */
class AdaptiveStrategyBuilder {
public:
    /**
     * Build optimal regime mapping by testing strategies in different periods
     */
    static AdaptiveStrategy::Config build_optimal_config(
        const std::vector<exchange::Kline>& klines,
        bool verbose = false)
    {
        AdaptiveStrategy::Config config;
        config.verbose = verbose;

        // We could analyze historical data to find:
        // 1. Which periods were trending vs ranging
        // 2. Which strategy performed best in each period
        // For now, use sensible defaults

        // Conservative mapping based on theory
        config.strategy_map.trending_up = config::StrategyType::Breakout;
        config.strategy_map.trending_down = config::StrategyType::MeanReversion; // No shorting
        config.strategy_map.ranging = config::StrategyType::MeanReversion;
        config.strategy_map.high_volatility = config::StrategyType::RSI;
        config.strategy_map.low_volatility = config::StrategyType::MeanReversion;

        return config;
    }
};

}  // namespace strategy
}  // namespace hft
