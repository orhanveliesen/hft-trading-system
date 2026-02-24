#pragma once

/**
 * SmartStrategy - Adaptive Trading Strategy
 *
 * Combines multiple signal sources and self-adjusts based on performance.
 *
 * Key features:
 * 1. Multi-model: Can switch between momentum/mean-reversion modes
 * 2. Self-assessment: Tracks own performance and adjusts confidence
 * 3. Adaptive sizing: Position size based on confidence and conditions
 * 4. Risk-aware: Reduces exposure after losses, increases after wins
 *
 * Overhead: ~200ns per evaluation (negligible for our use case)
 */

#include "../types.hpp"
#include "regime_detector.hpp"
#include "rolling_sharpe.hpp"
#include "strategy_constants.hpp"
#include "technical_indicators.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace hft {
namespace strategy {

// =============================================================================
// Signal Output
// =============================================================================

struct SmartSignal {
    enum class Action { NONE, BUY, SELL, EXIT_LONG, EXIT_SHORT };

    Action action = Action::NONE;
    double confidence = 0;     // 0-1, how confident are we?
    double suggested_size = 0; // Position size multiplier (0-1)
    double entry_price = 0;    // Suggested entry
    double target_price = 0;   // Take profit
    double stop_price = 0;     // Stop loss

    const char* reason = ""; // Human-readable reason

    bool has_signal() const { return action != Action::NONE; }
    bool is_buy() const { return action == Action::BUY; }
    bool is_sell() const { return action == Action::SELL; }
};

// =============================================================================
// Strategy Mode
// =============================================================================

enum class StrategyMode {
    AGGRESSIVE, // High confidence, take more signals
    NORMAL,     // Standard operation
    CAUTIOUS,   // Only strong signals
    DEFENSIVE,  // Reduce exposure, exit-only preferred
    EXIT_ONLY   // No new positions, only close existing
};

inline const char* mode_to_string(StrategyMode mode) {
    switch (mode) {
    case StrategyMode::AGGRESSIVE:
        return "AGGR";
    case StrategyMode::NORMAL:
        return "NORM";
    case StrategyMode::CAUTIOUS:
        return "CAUT";
    case StrategyMode::DEFENSIVE:
        return "DEF";
    case StrategyMode::EXIT_ONLY:
        return "EXIT";
    default:
        return "?";
    }
}

// =============================================================================
// Configuration
// =============================================================================

// Score bounds and mathematical constants
namespace constants {
// Signal score bounds
static constexpr double SCORE_MIN = -1.0;    // Minimum signal score (strong sell)
static constexpr double SCORE_MAX = 1.0;     // Maximum signal score (strong buy)
static constexpr double SCORE_NEUTRAL = 0.0; // Neutral score (no signal)

// BB position conversion: [-1, +1] → [0, 1]
static constexpr double BB_RANGE_OFFSET = 1.0;
static constexpr double BB_RANGE_SCALE = 2.0;
static constexpr double BB_UPPER_BOUND = 1.0;

// Confidence bounds
static constexpr double CONFIDENCE_DEFAULT = 0.5; // Neutral starting confidence
static constexpr double CONFIDENCE_MIN = 0.1;     // Minimum confidence
static constexpr double CONFIDENCE_MAX = 1.0;     // Maximum confidence

// Performance tracking
static constexpr int MIN_TRADES_FOR_CONFIDENCE = 5; // Min trades before adjusting confidence
static constexpr int RECENT_TRADES_WINDOW = 5;      // Window for recent trade analysis

// Confidence adjustment
static constexpr double RECENT_PNL_THRESHOLD = 0.01;  // 1% threshold for recent PnL boost/penalty
static constexpr double CONFIDENCE_ADJUSTMENT = 0.1;  // Amount to adjust confidence
static constexpr double LOSS_PENALTY_PER_LOSS = 0.05; // Confidence penalty per consecutive loss

// Signal strength thresholds
static constexpr double STRONG_SIGNAL_THRESHOLD = 0.7; // Above this = "strong" signal

// Spread filtering
static constexpr double WIDE_SPREAD_FILTER_THRESHOLD = 0.002; // 0.2% spread triggers filtering
static constexpr double WIDE_SPREAD_SIGNAL_MULT = 0.7;        // Multiplier when spread is wide
static constexpr double SPREAD_INVERSE_SCALE = 2.0;           // For inverse spread size scaling

// Helper function to clamp score to valid range
inline double clamp_score(double score) {
    return std::max(SCORE_MIN, std::min(SCORE_MAX, score));
}

// Helper function to clamp confidence to valid range
inline double clamp_confidence(double conf) {
    return std::max(CONFIDENCE_MIN, std::min(CONFIDENCE_MAX, conf));
}
} // namespace constants

struct SmartStrategyConfig {
    // Technical indicators config (DRY: single source of truth for RSI/BB thresholds)
    TechnicalIndicatorsConfig ti_config{};

    // Score weights for signal calculation
    double score_weight_strong = 0.4;     // Weight for strong signals (extreme RSI, outside BB)
    double score_weight_medium = 0.3;     // Weight for medium signals (oversold/overbought)
    double score_weight_weak = 0.2;       // Weight for weak signals (mild conditions)
    double ema_spread_threshold = 0.001;  // EMA spread threshold for momentum signals
    double ema_distance_threshold = 0.02; // EMA distance % for mean reversion signals

    // Performance tracking
    int performance_window = 20; // Track last N trades
    double min_confidence = 0.3; // Below this, no signal

    // Minimum trades thresholds for mode transitions and sizing
    int min_trades_for_sharpe_mode = 20;   // Min trades before Sharpe-based mode transitions
    int min_trades_for_win_rate_mode = 10; // Min trades before win rate mode transitions
    int min_trades_for_sharpe_sizing = 10; // Min trades before Sharpe-based position sizing
    double wide_spread_threshold = 0.001;  // Spread % above which position size is reduced

    // Mode transitions (reference strategy_constants.hpp for defaults)
    int losses_to_cautious = StreakThresholds::LOSSES_TO_CAUTIOUS;
    int losses_to_defensive = StreakThresholds::LOSSES_TO_DEFENSIVE;
    int losses_to_exit_only = StreakThresholds::LOSSES_TO_EXIT_ONLY;
    double drawdown_to_defensive = DrawdownThresholds::TO_DEFENSIVE;
    double drawdown_to_exit = DrawdownThresholds::TO_EXIT_ONLY;

    // Win rate thresholds
    double win_rate_aggressive = 0.60;                             // >60% → can be AGGRESSIVE
    double win_rate_cautious = 0.40;                               // <40% → be CAUTIOUS
    int wins_to_aggressive = StreakThresholds::WINS_TO_AGGRESSIVE; // Consecutive wins for AGGRESSIVE

    // Sharpe ratio thresholds (risk-adjusted performance)
    double sharpe_aggressive = 1.0; // Sharpe > 1.0 → can be AGGRESSIVE
    double sharpe_cautious = 0.3;   // Sharpe < 0.3 → be CAUTIOUS
    double sharpe_defensive = 0.0;  // Sharpe < 0 → DEFENSIVE

    // Signal thresholds by mode
    double signal_threshold_aggressive = 0.3;
    double signal_threshold_normal = 0.5;
    double signal_threshold_cautious = 0.7;

    // Position sizing
    double base_position_pct = 0.05; // 5% of capital per trade
    double max_position_pct = 0.15;  // Max 15% per trade
    double min_position_pct = 0.01;  // Min 1% per trade

    // Target/Stop - wider stops to avoid frequent stop-outs
    double default_target_pct = 0.03; // 3% target
    double default_stop_pct = 0.05;   // 5% stop (requires ~38% win rate)
    double min_risk_reward = 0.6;     // Allow stop > target for low win rate
};

// =============================================================================
// Lookup Tables (Branchless mode/regime calculations)
// Index: enum value, Value: multiplier or weight
// =============================================================================

namespace lookup {
// Mode-based signal multipliers: indexed by StrategyMode
// AGGRESSIVE=0, NORMAL=1, CAUTIOUS=2, DEFENSIVE=3, EXIT_ONLY=4
static constexpr std::array<double, 5> MODE_SIGNAL_MULT = {
    1.2, // AGGRESSIVE
    1.0, // NORMAL
    0.7, // CAUTIOUS
    0.5, // DEFENSIVE
    0.3  // EXIT_ONLY
};

// Mode-based size multipliers
static constexpr std::array<double, 5> MODE_SIZE_MULT = {
    1.5,  // AGGRESSIVE
    1.0,  // NORMAL
    0.5,  // CAUTIOUS
    0.25, // DEFENSIVE
    0.25  // EXIT_ONLY
};

// Regime-based momentum/MR weights
// Unknown=0, TrendingUp=1, TrendingDown=2, Ranging=3, HighVol=4, LowVol=5, Spike=6
struct RegimeWeights {
    double momentum;
    double mean_reversion;
};

static constexpr std::array<RegimeWeights, 7> REGIME_WEIGHTS = {{
    {0.5, 0.5}, // Unknown
    {0.7, 0.3}, // TrendingUp
    {0.7, 0.3}, // TrendingDown
    {0.3, 0.7}, // Ranging
    {0.4, 0.6}, // HighVolatility
    {0.3, 0.7}, // LowVolatility
    {0.2, 0.2}  // Spike (reduce both)
}};

// Regime-based target/stop multipliers
struct TargetStopMult {
    double target;
    double stop;
};

static constexpr std::array<TargetStopMult, 7> REGIME_TARGET_STOP = {{
    {1.0, 1.0}, // Unknown
    {1.5, 1.0}, // TrendingUp (let winners run)
    {1.5, 1.0}, // TrendingDown
    {1.0, 1.0}, // Ranging
    {1.3, 1.3}, // HighVolatility (wider stops)
    {0.7, 0.7}, // LowVolatility (smaller targets)
    {0.5, 2.0}  // Spike (tight target, wide stop for safety)
}};
} // namespace lookup

// =============================================================================
// SmartStrategy
// =============================================================================

class SmartStrategy {
public:
    explicit SmartStrategy(const SmartStrategyConfig& config = SmartStrategyConfig()) : config_(config) {
        reset_performance();
    }

    // =========================================================================
    // Main Interface
    // =========================================================================

    /**
     * Generate signal based on all available information
     */
    SmartSignal evaluate(double bid, double ask, MarketRegime regime, const TechnicalIndicators& indicators,
                         double current_position,  // Current position qty (+ long, - short, 0 flat)
                         double unrealized_pnl_pct // Current unrealized P&L as % of entry
    ) {
        SmartSignal signal;

        double mid = (bid + ask) / 2.0;
        double spread_pct = (ask - bid) / mid;

        // 1. Update internal state
        update_mode();

        // 2. Check if we should even generate signals
        if (mode_ == StrategyMode::EXIT_ONLY && current_position == 0) {
            signal.reason = "EXIT_ONLY mode, no new positions";
            return signal;
        }

        // 3. Generate raw signals from different models
        double momentum_score = calc_momentum_score(indicators, regime);
        double mean_rev_score = calc_mean_reversion_score(indicators, mid);

        // 4. Blend signals based on regime
        double blended_score = blend_signals(momentum_score, mean_rev_score, regime);

        // 5. Apply confidence and mode filters
        double adjusted_score = apply_filters(blended_score, spread_pct);

        // 6. Generate final signal
        signal = generate_signal(adjusted_score, bid, ask, current_position, regime);

        // 7. Adjust position size based on confidence
        if (signal.has_signal()) {
            signal.suggested_size = calculate_position_size(signal.confidence, spread_pct);
            calculate_targets(signal, bid, ask, regime);
        }

        return signal;
    }

    /**
     * Record trade result for self-assessment
     */
    void record_trade_result(double pnl_pct, bool was_win) {
        // Shift history
        for (int i = config_.performance_window - 1; i > 0; --i) {
            trade_results_[i] = trade_results_[i - 1];
        }
        trade_results_[0] = pnl_pct;

        // Update Rolling Sharpe with this trade's return
        sharpe_.add_return(pnl_pct);

        total_trades_++;
        if (was_win) {
            wins_++;
            consecutive_losses_ = 0;
            consecutive_wins_++;
        } else {
            losses_++;
            consecutive_wins_ = 0;
            consecutive_losses_++;
        }

        // Update peak and drawdown
        cumulative_pnl_ += pnl_pct;
        if (cumulative_pnl_ > peak_pnl_) {
            peak_pnl_ = cumulative_pnl_;
        }
        current_drawdown_ = peak_pnl_ - cumulative_pnl_;

        // Recalculate confidence
        update_confidence();
    }

    /**
     * Reset after significant event (e.g., new session)
     */
    void reset_performance() {
        std::fill(trade_results_.begin(), trade_results_.end(), 0.0);
        total_trades_ = 0;
        wins_ = 0;
        losses_ = 0;
        consecutive_wins_ = 0;
        consecutive_losses_ = 0;
        cumulative_pnl_ = 0;
        peak_pnl_ = 0;
        current_drawdown_ = 0;
        confidence_ = constants::CONFIDENCE_DEFAULT; // Start neutral
        mode_ = StrategyMode::NORMAL;
        sharpe_.reset(); // Reset Sharpe calculator
    }

    // =========================================================================
    // Accessors
    // =========================================================================

    StrategyMode mode() const { return mode_; }
    double confidence() const { return confidence_; }
    double win_rate() const {
        return total_trades_ > 0 ? static_cast<double>(wins_) / total_trades_ : constants::CONFIDENCE_DEFAULT;
    }
    int consecutive_losses() const { return consecutive_losses_; }
    int consecutive_wins() const { return consecutive_wins_; }
    double current_drawdown() const { return current_drawdown_; }
    int total_trades() const { return total_trades_; }

    // Sharpe ratio accessors
    double sharpe_ratio() const { return sharpe_.sharpe_ratio(); }
    double annualized_sharpe() const { return sharpe_.annualized_sharpe(); }
    double sharpe_position_multiplier() const { return sharpe_.position_multiplier(); }
    bool sharpe_should_trade() const { return sharpe_.should_trade(); }
    const RollingSharpe<100>& sharpe() const { return sharpe_; }

    // For dashboard display
    const char* mode_string() const { return mode_to_string(mode_); }

private:
    SmartStrategyConfig config_;

    // Performance tracking
    std::array<double, 20> trade_results_{}; // Last N trade P&L %
    int total_trades_ = 0;
    int wins_ = 0;
    int losses_ = 0;
    int consecutive_wins_ = 0;
    int consecutive_losses_ = 0;
    double cumulative_pnl_ = 0;
    double peak_pnl_ = 0;
    double current_drawdown_ = 0;

    // Rolling Sharpe for risk-adjusted performance
    RollingSharpe<100> sharpe_{0}; // 100-trade window, 0 risk-free rate for simplicity

    // Internal state
    double confidence_ = constants::CONFIDENCE_DEFAULT;
    StrategyMode mode_ = StrategyMode::NORMAL;

    // =========================================================================
    // Internal Methods
    // =========================================================================

    void update_mode() {
        // Drawdown-based transitions (highest priority)
        if (current_drawdown_ >= config_.drawdown_to_exit) {
            mode_ = StrategyMode::EXIT_ONLY;
            return;
        }
        if (current_drawdown_ >= config_.drawdown_to_defensive) {
            mode_ = StrategyMode::DEFENSIVE;
            return;
        }

        // Loss streak transitions
        if (consecutive_losses_ >= config_.losses_to_exit_only) {
            mode_ = StrategyMode::EXIT_ONLY;
            return;
        }
        if (consecutive_losses_ >= config_.losses_to_defensive) {
            mode_ = StrategyMode::DEFENSIVE;
            return;
        }
        if (consecutive_losses_ >= config_.losses_to_cautious) {
            mode_ = StrategyMode::CAUTIOUS;
            return;
        }

        // Sharpe ratio based (after enough trades for reliable Sharpe)
        if (sharpe_.count() >= static_cast<size_t>(config_.min_trades_for_sharpe_mode)) {
            double sr = sharpe_.sharpe_ratio();

            // Negative Sharpe = losing money on risk-adjusted basis
            if (sr < config_.sharpe_defensive) {
                mode_ = StrategyMode::DEFENSIVE;
                return;
            }

            // Low Sharpe = poor risk-adjusted returns
            if (sr < config_.sharpe_cautious) {
                mode_ = StrategyMode::CAUTIOUS;
                return;
            }

            // High Sharpe + good conditions = can be aggressive
            if (sr >= config_.sharpe_aggressive && consecutive_wins_ >= config_.wins_to_aggressive &&
                win_rate() >= config_.win_rate_aggressive) {
                mode_ = StrategyMode::AGGRESSIVE;
                return;
            }
        }

        // Win rate based (only after enough trades)
        if (total_trades_ >= config_.min_trades_for_win_rate_mode) {
            double wr = win_rate();
            if (wr >= config_.win_rate_aggressive && consecutive_wins_ >= config_.wins_to_aggressive) {
                mode_ = StrategyMode::AGGRESSIVE;
                return;
            }
            if (wr < config_.win_rate_cautious) {
                mode_ = StrategyMode::CAUTIOUS;
                return;
            }
        }

        // Default
        mode_ = StrategyMode::NORMAL;
    }

    void update_confidence() {
        if (total_trades_ < constants::MIN_TRADES_FOR_CONFIDENCE) {
            confidence_ = constants::CONFIDENCE_DEFAULT; // Not enough data
            return;
        }

        // Base confidence from win rate
        double wr = win_rate();
        confidence_ = wr;

        // Adjust for recent performance (more weight to recent trades)
        double recent_pnl = 0;
        int count = std::min(constants::RECENT_TRADES_WINDOW, total_trades_);
        for (int i = 0; i < count; ++i) {
            recent_pnl += trade_results_[i];
        }
        recent_pnl /= count;

        // Boost/penalize based on recent performance
        if (recent_pnl > constants::RECENT_PNL_THRESHOLD) {
            confidence_ = std::min(constants::CONFIDENCE_MAX, confidence_ + constants::CONFIDENCE_ADJUSTMENT);
        } else if (recent_pnl < -constants::RECENT_PNL_THRESHOLD) {
            confidence_ = std::max(constants::CONFIDENCE_MIN, confidence_ - constants::CONFIDENCE_ADJUSTMENT);
        }

        // Penalize for consecutive losses
        confidence_ -= consecutive_losses_ * constants::LOSS_PENALTY_PER_LOSS;
        confidence_ = constants::clamp_confidence(confidence_);
    }

    double calc_momentum_score(const TechnicalIndicators& ind, MarketRegime regime) {
        // Momentum score: How strong is the trend signal?
        // Returns -1 (strong sell) to +1 (strong buy)

        double score = 0;
        const auto& ti = config_.ti_config;

        // RSI component (using TechnicalIndicatorsConfig thresholds - DRY)
        double rsi = ind.rsi();
        if (rsi > ti.rsi_overbought)
            score -= config_.score_weight_medium; // Overbought
        else if (rsi > ti.rsi_mild_overbought)
            score += config_.score_weight_weak; // Bullish momentum
        else if (rsi < ti.rsi_oversold)
            score += config_.score_weight_medium; // Oversold (contrarian)
        else if (rsi < ti.rsi_mild_oversold)
            score -= config_.score_weight_weak; // Bearish momentum

        // EMA crossover component
        double ema_spread = ind.ema_spread();
        if (ema_spread > config_.ema_spread_threshold)
            score += config_.score_weight_medium;
        else if (ema_spread < -config_.ema_spread_threshold)
            score -= config_.score_weight_medium;

        // Trend alignment bonus
        if (regime == MarketRegime::TrendingUp)
            score += config_.score_weight_weak;
        else if (regime == MarketRegime::TrendingDown)
            score -= config_.score_weight_weak;

        return constants::clamp_score(score);
    }

    double calc_mean_reversion_score(const TechnicalIndicators& ind, double price) {
        // Mean reversion score: How far from mean, expecting return?
        // Returns -1 (expect down) to +1 (expect up)

        double score = 0;
        const auto& ti = config_.ti_config;

        // Bollinger Band position
        // bb_position() returns -1 to +1, convert to 0 to 1 range
        double bb_pos = (ind.bb_position() + constants::BB_RANGE_OFFSET) / constants::BB_RANGE_SCALE;
        double near_band = ti.bb_near_band_margin;
        if (bb_pos < near_band)
            score += config_.score_weight_strong; // Near lower band
        else if (bb_pos > (constants::BB_UPPER_BOUND - near_band))
            score -= config_.score_weight_strong;

        // RSI extremes (mean reversion interpretation - using TechnicalIndicatorsConfig)
        double rsi = ind.rsi();
        if (rsi < ti.rsi_oversold)
            score += config_.score_weight_medium; // Oversold → buy
        else if (rsi > ti.rsi_overbought)
            score -= config_.score_weight_medium; // Overbought → sell

        // Distance from slow EMA (ema > 0 is validity check: EMA must be initialized, prices are always positive)
        double ema = ind.ema_slow();
        if (ema > constants::SCORE_NEUTRAL) { // EMA initialized check (valid prices are > 0)
            double dist_pct = (price - ema) / ema;
            if (dist_pct < -config_.ema_distance_threshold)
                score += config_.score_weight_medium;
            else if (dist_pct > config_.ema_distance_threshold)
                score -= config_.score_weight_medium;
        }

        return constants::clamp_score(score);
    }

    double blend_signals(double momentum, double mean_rev, MarketRegime regime) {
        // Branchless lookup: index into table by regime enum value
        size_t idx = static_cast<size_t>(regime);
        const auto& weights = lookup::REGIME_WEIGHTS[idx];
        return momentum * weights.momentum + mean_rev * weights.mean_reversion;
    }

    double apply_filters(double raw_score, double spread_pct) {
        double filtered = raw_score;

        // Reduce signal strength if spread is wide
        if (spread_pct > constants::WIDE_SPREAD_FILTER_THRESHOLD) {
            filtered *= constants::WIDE_SPREAD_SIGNAL_MULT;
        }

        // Apply confidence multiplier
        filtered *= confidence_;

        // Branchless mode-based adjustment via lookup table
        filtered *= lookup::MODE_SIGNAL_MULT[static_cast<size_t>(mode_)];

        return constants::clamp_score(filtered);
    }

    SmartSignal generate_signal(double score, double bid, double ask, double position, MarketRegime regime) {
        SmartSignal signal;

        // Determine threshold based on mode
        double threshold;
        switch (mode_) {
        case StrategyMode::AGGRESSIVE:
            threshold = config_.signal_threshold_aggressive;
            break;
        case StrategyMode::CAUTIOUS:
        case StrategyMode::DEFENSIVE:
            threshold = config_.signal_threshold_cautious;
            break;
        default:
            threshold = config_.signal_threshold_normal;
        }

        double abs_score = std::abs(score);

        // Not strong enough
        if (abs_score < threshold) {
            signal.reason = "Signal below threshold";
            return signal;
        }

        // Below minimum confidence
        if (abs_score < config_.min_confidence) {
            signal.reason = "Below minimum confidence";
            return signal;
        }

        signal.confidence = abs_score;

        // Determine action
        if (score > constants::SCORE_NEUTRAL) {
            // Bullish signal
            if (position < 0) {
                signal.action = SmartSignal::Action::EXIT_SHORT;
                signal.entry_price = ask;
                signal.reason = "Exit short on bullish signal";
            } else if (mode_ != StrategyMode::EXIT_ONLY) {
                signal.action = SmartSignal::Action::BUY;
                signal.entry_price = ask;
                signal.reason = score > constants::STRONG_SIGNAL_THRESHOLD ? "Strong buy signal" : "Buy signal";
            }
        } else {
            // Bearish signal
            if (position > 0) {
                signal.action = SmartSignal::Action::EXIT_LONG;
                signal.entry_price = bid;
                signal.reason = "Exit long on bearish signal";
            } else if (mode_ != StrategyMode::EXIT_ONLY) {
                signal.action = SmartSignal::Action::SELL;
                signal.entry_price = bid;
                signal.reason = score < -constants::STRONG_SIGNAL_THRESHOLD ? "Strong sell signal" : "Sell signal";
            }
        }

        return signal;
    }

    double calculate_position_size(double signal_confidence, double spread_pct) {
        double size = config_.base_position_pct;

        // Scale by confidence
        size *= signal_confidence;

        // Scale by strategy confidence
        size *= confidence_;

        // Scale by Sharpe-based position multiplier (risk-adjusted sizing)
        // This reduces size when Sharpe is low/negative, increases when high
        if (sharpe_.count() >= static_cast<size_t>(config_.min_trades_for_sharpe_sizing)) {
            size *= sharpe_.position_multiplier();
        }

        // Reduce for wide spreads
        if (spread_pct > config_.wide_spread_threshold) {
            size *=
                (config_.wide_spread_threshold * constants::SPREAD_INVERSE_SCALE / spread_pct); // Inverse relationship
        }

        // Branchless mode adjustments via lookup table
        size *= lookup::MODE_SIZE_MULT[static_cast<size_t>(mode_)];

        // Clamp to limits
        return std::max(config_.min_position_pct, std::min(config_.max_position_pct, size));
    }

    void calculate_targets(SmartSignal& signal, double bid, double ask, MarketRegime regime) {
        double target_pct = config_.default_target_pct;
        double stop_pct = config_.default_stop_pct;

        // Branchless regime adjustment via lookup table
        size_t idx = static_cast<size_t>(regime);
        const auto& ts = lookup::REGIME_TARGET_STOP[idx];
        target_pct *= ts.target;
        stop_pct *= ts.stop;

        // Ensure minimum risk:reward
        if (target_pct / stop_pct < config_.min_risk_reward) {
            target_pct = stop_pct * config_.min_risk_reward;
        }

        if (signal.action == SmartSignal::Action::BUY) {
            signal.target_price = signal.entry_price * (1 + target_pct);
            signal.stop_price = signal.entry_price * (1 - stop_pct);
        } else if (signal.action == SmartSignal::Action::SELL) {
            signal.target_price = signal.entry_price * (1 - target_pct);
            signal.stop_price = signal.entry_price * (1 + stop_pct);
        }
    }
};

} // namespace strategy
} // namespace hft
