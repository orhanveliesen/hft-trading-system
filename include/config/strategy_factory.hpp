#pragma once

#include "strategy_config.hpp"
#include "../backtest/kline_backtest.hpp"
#include "../backtest/strategies.hpp"
#include "../backtest/strategy_adapter.hpp"
#include <memory>

namespace hft {
namespace config {

/**
 * Strategy Factory
 *
 * Creates strategy instances from configuration.
 * Uses unique_ptr for ownership management.
 */
class StrategyFactory {
public:
    /**
     * Create a strategy from SymbolConfig
     */
    static std::unique_ptr<backtest::IStrategy> create(const SymbolConfig& config) {
        return create(config.strategy, config.params);
    }

    /**
     * Create a strategy from type and params
     */
    static std::unique_ptr<backtest::IStrategy> create(StrategyType type, const StrategyParams& params) {
        switch (type) {
            case StrategyType::SMA:
                return std::make_unique<backtest::SMACrossover>(
                    params.sma_fast, params.sma_slow);

            case StrategyType::RSI:
                return std::make_unique<backtest::RSIStrategy>(
                    params.rsi_period, params.rsi_oversold, params.rsi_overbought);

            case StrategyType::MeanReversion:
                return std::make_unique<backtest::MeanReversion>(
                    params.mr_lookback, params.mr_std_mult);

            case StrategyType::Breakout:
                return std::make_unique<backtest::BreakoutStrategy>(
                    params.breakout_lookback);

            case StrategyType::MACD:
                return std::make_unique<backtest::MACDStrategy>(
                    params.macd_fast, params.macd_slow, params.macd_signal);

            case StrategyType::SimpleMR_HFT:
                return std::make_unique<backtest::SimpleMRAdapter>();

            case StrategyType::Momentum_HFT: {
                strategy::MomentumConfig cfg;
                cfg.lookback_ticks = params.momentum_lookback;
                cfg.threshold_bps = params.momentum_threshold_bps;
                return std::make_unique<backtest::MomentumAdapter>(cfg);
            }

            default:
                throw std::runtime_error("Unknown strategy type");
        }
    }

    /**
     * Get strategy name from type and params
     */
    static std::string get_name(StrategyType type, const StrategyParams& params) {
        switch (type) {
            case StrategyType::SMA:
                return "SMA(" + std::to_string(params.sma_fast) + "/" +
                       std::to_string(params.sma_slow) + ")";

            case StrategyType::RSI:
                return "RSI(" + std::to_string(params.rsi_period) + "," +
                       std::to_string(static_cast<int>(params.rsi_oversold)) + "/" +
                       std::to_string(static_cast<int>(params.rsi_overbought)) + ")";

            case StrategyType::MeanReversion:
                return "MeanRev(" + std::to_string(params.mr_lookback) + "," +
                       std::to_string(params.mr_std_mult).substr(0, 3) + ")";

            case StrategyType::Breakout:
                return "Breakout(" + std::to_string(params.breakout_lookback) + ")";

            case StrategyType::MACD:
                return "MACD(" + std::to_string(params.macd_fast) + "/" +
                       std::to_string(params.macd_slow) + "/" +
                       std::to_string(params.macd_signal) + ")";

            case StrategyType::SimpleMR_HFT:
                return "SimpleMR_HFT";

            case StrategyType::Momentum_HFT:
                return "Momentum_HFT(" + std::to_string(params.momentum_lookback) + "," +
                       std::to_string(params.momentum_threshold_bps) + "bps)";

            default:
                return "Unknown";
        }
    }

    /**
     * Get all available strategy types
     */
    static std::vector<StrategyType> get_all_types() {
        return {
            StrategyType::SMA,
            StrategyType::RSI,
            StrategyType::MeanReversion,
            StrategyType::Breakout,
            StrategyType::MACD
            // HFT strategies excluded - not suitable for kline data
        };
    }

    /**
     * Get default params for a strategy type
     */
    static StrategyParams get_default_params(StrategyType type) {
        StrategyParams params;
        // All defaults are already set in StrategyParams struct
        return params;
    }
};

}  // namespace config
}  // namespace hft
