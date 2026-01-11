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

#include "regime_detector.hpp"
#include "technical_indicators.hpp"
#include "../types.hpp"
#include <cmath>
#include <algorithm>
#include <array>

namespace hft {
namespace strategy {

// =============================================================================
// Signal Output
// =============================================================================

struct SmartSignal {
    enum class Action { NONE, BUY, SELL, EXIT_LONG, EXIT_SHORT };

    Action action = Action::NONE;
    double confidence = 0;      // 0-1, how confident are we?
    double suggested_size = 0;  // Position size multiplier (0-1)
    double entry_price = 0;     // Suggested entry
    double target_price = 0;    // Take profit
    double stop_price = 0;      // Stop loss

    const char* reason = "";    // Human-readable reason

    bool has_signal() const { return action != Action::NONE; }
    bool is_buy() const { return action == Action::BUY; }
    bool is_sell() const { return action == Action::SELL; }
};

// =============================================================================
// Strategy Mode
// =============================================================================

enum class StrategyMode {
    AGGRESSIVE,     // High confidence, take more signals
    NORMAL,         // Standard operation
    CAUTIOUS,       // Only strong signals
    DEFENSIVE,      // Reduce exposure, exit-only preferred
    EXIT_ONLY       // No new positions, only close existing
};

inline const char* mode_to_string(StrategyMode mode) {
    switch (mode) {
        case StrategyMode::AGGRESSIVE: return "AGGR";
        case StrategyMode::NORMAL: return "NORM";
        case StrategyMode::CAUTIOUS: return "CAUT";
        case StrategyMode::DEFENSIVE: return "DEF";
        case StrategyMode::EXIT_ONLY: return "EXIT";
        default: return "?";
    }
}

// =============================================================================
// Configuration
// =============================================================================

struct SmartStrategyConfig {
    // Performance tracking
    int performance_window = 20;        // Track last N trades
    double min_confidence = 0.3;        // Below this, no signal

    // Mode transitions
    int losses_to_cautious = 2;         // Consecutive losses → CAUTIOUS
    int losses_to_defensive = 4;        // Consecutive losses → DEFENSIVE
    int losses_to_exit_only = 6;        // Consecutive losses → EXIT_ONLY
    double drawdown_to_defensive = 0.03; // 3% drawdown → DEFENSIVE
    double drawdown_to_exit = 0.05;     // 5% drawdown → EXIT_ONLY

    // Win rate thresholds
    double win_rate_aggressive = 0.60;  // >60% → can be AGGRESSIVE
    double win_rate_cautious = 0.40;    // <40% → be CAUTIOUS

    // Signal thresholds by mode
    double signal_threshold_aggressive = 0.3;
    double signal_threshold_normal = 0.5;
    double signal_threshold_cautious = 0.7;

    // Position sizing
    double base_position_pct = 0.02;    // 2% of capital per trade
    double max_position_pct = 0.05;     // Max 5% per trade
    double min_position_pct = 0.005;    // Min 0.5% per trade

    // Target/Stop
    double default_target_pct = 0.015;  // 1.5% target
    double default_stop_pct = 0.01;     // 1% stop
    double min_risk_reward = 1.5;       // Minimum R:R ratio
};

// =============================================================================
// SmartStrategy
// =============================================================================

class SmartStrategy {
public:
    explicit SmartStrategy(const SmartStrategyConfig& config = SmartStrategyConfig())
        : config_(config)
    {
        reset_performance();
    }

    // =========================================================================
    // Main Interface
    // =========================================================================

    /**
     * Generate signal based on all available information
     */
    SmartSignal evaluate(
        double bid, double ask,
        MarketRegime regime,
        const TechnicalIndicators& indicators,
        double current_position,    // Current position qty (+ long, - short, 0 flat)
        double unrealized_pnl_pct   // Current unrealized P&L as % of entry
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
        confidence_ = 0.5;  // Start neutral
        mode_ = StrategyMode::NORMAL;
    }

    // =========================================================================
    // Accessors
    // =========================================================================

    StrategyMode mode() const { return mode_; }
    double confidence() const { return confidence_; }
    double win_rate() const {
        return total_trades_ > 0 ? static_cast<double>(wins_) / total_trades_ : 0.5;
    }
    int consecutive_losses() const { return consecutive_losses_; }
    int consecutive_wins() const { return consecutive_wins_; }
    double current_drawdown() const { return current_drawdown_; }
    int total_trades() const { return total_trades_; }

    // For dashboard display
    const char* mode_string() const { return mode_to_string(mode_); }

private:
    SmartStrategyConfig config_;

    // Performance tracking
    std::array<double, 20> trade_results_{};  // Last N trade P&L %
    int total_trades_ = 0;
    int wins_ = 0;
    int losses_ = 0;
    int consecutive_wins_ = 0;
    int consecutive_losses_ = 0;
    double cumulative_pnl_ = 0;
    double peak_pnl_ = 0;
    double current_drawdown_ = 0;

    // Internal state
    double confidence_ = 0.5;
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

        // Win rate based (only after enough trades)
        if (total_trades_ >= 10) {
            double wr = win_rate();
            if (wr >= config_.win_rate_aggressive && consecutive_wins_ >= 2) {
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
        if (total_trades_ < 5) {
            confidence_ = 0.5;  // Not enough data
            return;
        }

        // Base confidence from win rate
        double wr = win_rate();
        confidence_ = wr;

        // Adjust for recent performance (more weight to recent trades)
        double recent_pnl = 0;
        int count = std::min(5, total_trades_);
        for (int i = 0; i < count; ++i) {
            recent_pnl += trade_results_[i];
        }
        recent_pnl /= count;

        // Boost/penalize based on recent performance
        if (recent_pnl > 0.01) {
            confidence_ = std::min(1.0, confidence_ + 0.1);
        } else if (recent_pnl < -0.01) {
            confidence_ = std::max(0.1, confidence_ - 0.1);
        }

        // Penalize for consecutive losses
        confidence_ -= consecutive_losses_ * 0.05;
        confidence_ = std::max(0.1, std::min(1.0, confidence_));
    }

    double calc_momentum_score(const TechnicalIndicators& ind, MarketRegime regime) {
        // Momentum score: How strong is the trend signal?
        // Returns -1 (strong sell) to +1 (strong buy)

        double score = 0;

        // RSI component
        double rsi = ind.rsi();
        if (rsi > 70) score -= 0.3;       // Overbought
        else if (rsi > 60) score += 0.2;  // Bullish momentum
        else if (rsi < 30) score += 0.3;  // Oversold (contrarian in momentum)
        else if (rsi < 40) score -= 0.2;  // Bearish momentum

        // MACD component
        double macd_hist = ind.macd_histogram();
        if (macd_hist > 0.001) score += 0.3;
        else if (macd_hist < -0.001) score -= 0.3;

        // Trend alignment bonus
        if (regime == MarketRegime::TrendingUp) score += 0.2;
        else if (regime == MarketRegime::TrendingDown) score -= 0.2;

        return std::max(-1.0, std::min(1.0, score));
    }

    double calc_mean_reversion_score(const TechnicalIndicators& ind, double price) {
        // Mean reversion score: How far from mean, expecting return?
        // Returns -1 (expect down) to +1 (expect up)

        double score = 0;

        // Bollinger Band position
        double bb_pos = ind.bollinger_position();  // 0 = lower, 0.5 = middle, 1 = upper
        if (bb_pos < 0.2) score += 0.4;           // Near lower band → expect bounce
        else if (bb_pos > 0.8) score -= 0.4;      // Near upper band → expect drop

        // RSI extremes (mean reversion interpretation)
        double rsi = ind.rsi();
        if (rsi < 30) score += 0.3;               // Oversold → buy
        else if (rsi > 70) score -= 0.3;          // Overbought → sell

        // Distance from EMA
        double ema = ind.ema();
        if (ema > 0) {
            double dist_pct = (price - ema) / ema;
            if (dist_pct < -0.02) score += 0.3;   // Below EMA → expect reversion up
            else if (dist_pct > 0.02) score -= 0.3; // Above EMA → expect reversion down
        }

        return std::max(-1.0, std::min(1.0, score));
    }

    double blend_signals(double momentum, double mean_rev, MarketRegime regime) {
        // Blend based on regime
        double mom_weight, mr_weight;

        switch (regime) {
            case MarketRegime::TrendingUp:
            case MarketRegime::TrendingDown:
                mom_weight = 0.7;
                mr_weight = 0.3;
                break;

            case MarketRegime::Ranging:
            case MarketRegime::LowVolatility:
                mom_weight = 0.3;
                mr_weight = 0.7;
                break;

            case MarketRegime::HighVolatility:
                // In high vol, reduce both, be more cautious
                mom_weight = 0.4;
                mr_weight = 0.4;
                break;

            default:
                mom_weight = 0.5;
                mr_weight = 0.5;
        }

        return momentum * mom_weight + mean_rev * mr_weight;
    }

    double apply_filters(double raw_score, double spread_pct) {
        double filtered = raw_score;

        // Reduce signal strength if spread is wide
        if (spread_pct > 0.002) {  // > 0.2% spread
            filtered *= 0.7;
        }

        // Apply confidence multiplier
        filtered *= confidence_;

        // Mode-based adjustment
        switch (mode_) {
            case StrategyMode::AGGRESSIVE:
                filtered *= 1.2;
                break;
            case StrategyMode::CAUTIOUS:
                filtered *= 0.7;
                break;
            case StrategyMode::DEFENSIVE:
                filtered *= 0.5;
                break;
            case StrategyMode::EXIT_ONLY:
                filtered *= 0.3;  // Only very strong signals
                break;
            default:
                break;
        }

        return std::max(-1.0, std::min(1.0, filtered));
    }

    SmartSignal generate_signal(double score, double bid, double ask,
                                 double position, MarketRegime regime) {
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
        if (score > 0) {
            // Bullish signal
            if (position < 0) {
                signal.action = SmartSignal::Action::EXIT_SHORT;
                signal.entry_price = ask;
                signal.reason = "Exit short on bullish signal";
            } else if (mode_ != StrategyMode::EXIT_ONLY) {
                signal.action = SmartSignal::Action::BUY;
                signal.entry_price = ask;
                signal.reason = score > 0.7 ? "Strong buy signal" : "Buy signal";
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
                signal.reason = score < -0.7 ? "Strong sell signal" : "Sell signal";
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

        // Reduce for wide spreads
        if (spread_pct > 0.001) {
            size *= (0.002 / spread_pct);  // Inverse relationship
        }

        // Mode adjustments
        switch (mode_) {
            case StrategyMode::AGGRESSIVE:
                size *= 1.5;
                break;
            case StrategyMode::CAUTIOUS:
                size *= 0.5;
                break;
            case StrategyMode::DEFENSIVE:
            case StrategyMode::EXIT_ONLY:
                size *= 0.25;
                break;
            default:
                break;
        }

        // Clamp to limits
        return std::max(config_.min_position_pct,
                       std::min(config_.max_position_pct, size));
    }

    void calculate_targets(SmartSignal& signal, double bid, double ask, MarketRegime regime) {
        double target_pct = config_.default_target_pct;
        double stop_pct = config_.default_stop_pct;

        // Adjust based on regime
        switch (regime) {
            case MarketRegime::TrendingUp:
            case MarketRegime::TrendingDown:
                target_pct *= 1.5;  // Let winners run in trends
                break;
            case MarketRegime::HighVolatility:
                target_pct *= 1.3;
                stop_pct *= 1.3;    // Wider stops in high vol
                break;
            case MarketRegime::LowVolatility:
                target_pct *= 0.7;  // Smaller targets in low vol
                stop_pct *= 0.7;
                break;
            default:
                break;
        }

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

}  // namespace strategy
}  // namespace hft
