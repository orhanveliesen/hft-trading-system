#pragma once

/**
 * Stateless Strategy Scorers
 *
 * Pure scoring functions that map market indicators to a [-1, +1] score.
 * - Positive score = bullish signal (buy)
 * - Negative score = bearish signal (sell)
 * - Zero = neutral
 *
 * Design Principles:
 * - Stateless: No internal state, pure functions
 * - Branchless: Optimized for hot path execution
 * - Concept-constrained: All scorers satisfy StrategyScorer concept
 * - SoA-friendly: Take symbol index and access SoA config arrays
 *
 * Usage:
 *   RSIScorer scorer(state.rsi);
 *   double score = scorer.score(sym, state.common, indicators);
 */

#include "../trading/trading_state.hpp"

#include <algorithm>
#include <cmath>
#include <concepts>

namespace hft {
namespace strategy {

/**
 * Market indicators for scoring.
 * Populated by indicator calculators before strategy scoring.
 */
struct Indicators {
    double rsi = 50.0;           // RSI value (0-100)
    double macd_histogram = 0.0; // MACD histogram value
    double macd_scale = 1.0;     // Scale factor for normalization
    double momentum = 0.0;       // Price momentum (return over lookback)
    double ema_deviation = 0.0;  // Distance from EMA as percentage
    double volatility = 0.0;     // Current volatility
};

/**
 * StrategyScorer concept - all scorers must satisfy this.
 */
template <typename T>
concept StrategyScorer = requires(T t, size_t sym, const trading::CommonConfig& common, const Indicators& ind) {
    { t.score(sym, common, ind) } -> std::same_as<double>;
};

/**
 * RSI Scorer - Relative Strength Index based scoring.
 *
 * Score = (50 - RSI) / 50, clamped to [-1, +1]
 * - RSI < 50: Positive score (bullish)
 * - RSI > 50: Negative score (bearish)
 * - RSI = 50: Zero score (neutral)
 */
class RSIScorer {
public:
    explicit RSIScorer(const trading::RSIConfig& config) : config_(config) {}

    double score(size_t sym, const trading::CommonConfig& /*common*/, const Indicators& ind) const {
        // Normalize RSI to [-1, +1]
        // RSI 0 -> score +1 (very oversold, bullish)
        // RSI 50 -> score 0 (neutral)
        // RSI 100 -> score -1 (very overbought, bearish)
        double normalized = (50.0 - ind.rsi) / 50.0;
        return std::clamp(normalized, -1.0, 1.0);
    }

private:
    const trading::RSIConfig& config_;
};

/**
 * MACD Scorer - Moving Average Convergence Divergence based scoring.
 *
 * Score = MACD histogram / scale, clamped to [-1, +1]
 * - Positive histogram: Positive score (bullish)
 * - Negative histogram: Negative score (bearish)
 */
class MACDScorer {
public:
    explicit MACDScorer(const trading::MACDConfig& config) : config_(config) {}

    double score(size_t /*sym*/, const trading::CommonConfig& /*common*/, const Indicators& ind) const {
        // Normalize histogram to [-1, +1]
        if (ind.macd_scale <= 0.0)
            return 0.0;

        double normalized = ind.macd_histogram / ind.macd_scale;
        return std::clamp(normalized, -1.0, 1.0);
    }

private:
    const trading::MACDConfig& config_;
};

/**
 * Momentum Scorer - Price momentum based scoring.
 *
 * Score = momentum / threshold, clamped to [-1, +1]
 * - Positive momentum: Positive score (bullish)
 * - Negative momentum: Negative score (bearish)
 */
class MomentumScorer {
public:
    explicit MomentumScorer(const trading::MomentumConfig& config) : config_(config) {}

    double score(size_t sym, const trading::CommonConfig& /*common*/, const Indicators& ind) const {
        double threshold = config_.threshold[sym];
        if (threshold <= 0.0)
            return 0.0;

        double normalized = ind.momentum / threshold;
        return std::clamp(normalized, -1.0, 1.0);
    }

private:
    const trading::MomentumConfig& config_;
};

/**
 * Defensive Scorer - Always returns 0 (no new positions).
 * Used when market conditions are unfavorable.
 */
class DefensiveScorer {
public:
    double score(size_t /*sym*/, const trading::CommonConfig& /*common*/, const Indicators& /*ind*/) const {
        return 0.0; // Never signal
    }
};

/**
 * Test Scorer - Always returns positive score for testing.
 * Use this to verify the hot path flow works before implementing real strategies.
 * Returns 0.5 (above SCORE_THRESHOLD of 0.3) to trigger BUY signals.
 */
class TestScorer {
public:
    double score(size_t /*sym*/, const trading::CommonConfig& /*common*/, const Indicators& /*ind*/) const {
        return 0.5; // Always bullish for testing
    }
};

/**
 * Score dispatcher - maps StrategyId to scorer without vtable.
 *
 * Uses branchless-friendly switch statement that compiler can optimize.
 *
 * @param sym Symbol index
 * @param state Reference to TradingState
 * @param ind Current indicators
 * @return Score in [-1, +1] range
 */
inline double dispatch_score(size_t sym, const trading::TradingState& state, const Indicators& ind) {
    trading::StrategyId strategy = state.strategies.active[sym];

    switch (strategy) {
    case trading::StrategyId::RSI: {
        RSIScorer scorer(state.rsi);
        return scorer.score(sym, state.common, ind);
    }
    case trading::StrategyId::MACD: {
        MACDScorer scorer(state.macd);
        return scorer.score(sym, state.common, ind);
    }
    case trading::StrategyId::MOMENTUM: {
        MomentumScorer scorer(state.momentum);
        return scorer.score(sym, state.common, ind);
    }
    case trading::StrategyId::DEFENSIVE: {
        DefensiveScorer scorer;
        return scorer.score(sym, state.common, ind);
    }
    case trading::StrategyId::TEST: {
        TestScorer scorer;
        return scorer.score(sym, state.common, ind);
    }
    case trading::StrategyId::NONE:
    default:
        return 0.0;
    }
}

} // namespace strategy
} // namespace hft
