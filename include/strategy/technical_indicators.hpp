#pragma once

#include <cmath>
#include <algorithm>

namespace hft {
namespace strategy {

/**
 * TechnicalIndicators - Zero-allocation technical analysis
 *
 * All indicators use EMA-based calculations for O(1) updates.
 * No arrays, no allocations - just running statistics.
 *
 * Indicators:
 * - EMA (fast/slow for crossover)
 * - RSI (relative strength index)
 * - Bollinger Bands (volatility bands)
 *
 * Memory: ~20 doubles = 160 bytes per symbol
 * Latency: ~20ns per update
 */
/**
 * Technical Indicators Configuration
 * All values based on established technical analysis literature.
 */
struct TechnicalIndicatorsConfig {
    // EMA periods (Fibonacci-based, common in day trading)
    int fast_period = 8;
    int slow_period = 21;

    // RSI (J. Welles Wilder, 1978)
    int rsi_period = 14;
    double rsi_oversold = 30.0;           // Classic oversold level
    double rsi_overbought = 70.0;         // Classic overbought level
    double rsi_extreme_oversold = 20.0;   // Extreme oversold
    double rsi_extreme_overbought = 80.0; // Extreme overbought
    double rsi_mild_oversold = 40.0;      // Early buy signal
    double rsi_mild_overbought = 60.0;    // Early sell signal

    // Bollinger Bands (John Bollinger, 1980s)
    int bb_period = 20;
    double bb_std_dev = 2.0;
    double bb_near_band_margin = 0.1;  // 10% from band edge

    // Signal scoring thresholds
    int signal_strong_threshold = 5;
    int signal_medium_threshold = 3;
    int signal_weak_threshold = 1;

    // Minimum samples before signals are valid
    int min_samples = 20;
};

class TechnicalIndicators {
public:
    using Config = TechnicalIndicatorsConfig;

    explicit TechnicalIndicators(const Config& config = Config())
        : config_(config)
        , fast_alpha_(2.0 / (config.fast_period + 1))
        , slow_alpha_(2.0 / (config.slow_period + 1))
        , rsi_alpha_(1.0 / config.rsi_period)
        , bb_alpha_(2.0 / (config.bb_period + 1))
    {
        reset();
    }

    /**
     * Update all indicators with new price - O(1), zero allocation
     */
    void update(double price) {
        if (price <= 0) return;

        count_++;

        // First price - initialize
        if (count_ == 1) {
            last_price_ = price;
            ema_fast_ = price;
            ema_slow_ = price;
            ema_price_ = price;  // For Bollinger
            return;
        }

        double change = price - last_price_;
        double pct_change = change / last_price_;
        last_price_ = price;

        // Update EMAs
        update_ema(price);

        // Update RSI
        update_rsi(change);

        // Update Bollinger Bands
        update_bollinger(price);

        // Track previous crossover state
        prev_ema_bullish_ = ema_bullish_;
        ema_bullish_ = (ema_fast_ > ema_slow_);
    }

    // ========================================
    // EMA Crossover Signals
    // ========================================

    bool ema_bullish() const { return ema_bullish_; }
    bool ema_bearish() const { return !ema_bullish_; }

    // Crossover just happened this tick
    bool ema_crossed_up() const { return ema_bullish_ && !prev_ema_bullish_; }
    bool ema_crossed_down() const { return !ema_bullish_ && prev_ema_bullish_; }

    double ema_fast() const { return ema_fast_; }
    double ema_slow() const { return ema_slow_; }

    // EMA trend strength: how far apart are the EMAs (as %)
    double ema_spread() const {
        if (ema_slow_ == 0) return 0;
        return (ema_fast_ - ema_slow_) / ema_slow_;
    }

    // ========================================
    // RSI Signals
    // ========================================

    double rsi() const { return rsi_; }

    bool is_oversold() const { return rsi_ < config_.rsi_oversold; }
    bool is_overbought() const { return rsi_ > config_.rsi_overbought; }

    // Strong signals
    bool is_extremely_oversold() const { return rsi_ < config_.rsi_extreme_oversold; }
    bool is_extremely_overbought() const { return rsi_ > config_.rsi_extreme_overbought; }

    // Custom threshold versions
    bool is_oversold(double threshold) const { return rsi_ < threshold; }
    bool is_overbought(double threshold) const { return rsi_ > threshold; }

    // ========================================
    // Bollinger Band Signals
    // ========================================

    double bb_upper() const { return bb_upper_; }
    double bb_middle() const { return ema_price_; }
    double bb_lower() const { return bb_lower_; }
    double bb_width() const { return bb_upper_ - bb_lower_; }

    // Position relative to bands (-1 = at lower, 0 = at middle, +1 = at upper)
    double bb_position() const {
        double width = bb_width();
        if (width == 0) return 0;
        return (last_price_ - bb_lower_) / width * 2 - 1;
    }

    bool below_lower_band() const { return last_price_ < bb_lower_; }
    bool above_upper_band() const { return last_price_ > bb_upper_; }
    bool near_lower_band() const {
        return bb_position() < (-1 + config_.bb_near_band_margin * 2);
    }
    bool near_upper_band() const {
        return bb_position() > (1 - config_.bb_near_band_margin * 2);
    }

    // ========================================
    // Composite Signals (combine indicators)
    // ========================================

    enum class SignalStrength {
        None = 0,
        Weak = 1,
        Medium = 2,
        Strong = 3
    };

    SignalStrength buy_signal() const {
        int score = 0;

        // EMA bullish or crossed up
        if (ema_crossed_up()) score += 2;
        else if (ema_bullish()) score += 1;

        // RSI oversold
        if (is_extremely_oversold()) score += 2;
        else if (rsi_ < config_.rsi_mild_oversold) score += 1;

        // Below or near lower Bollinger band
        if (below_lower_band()) score += 2;
        else if (near_lower_band()) score += 1;

        if (score >= config_.signal_strong_threshold) return SignalStrength::Strong;
        if (score >= config_.signal_medium_threshold) return SignalStrength::Medium;
        if (score >= config_.signal_weak_threshold) return SignalStrength::Weak;
        return SignalStrength::None;
    }

    SignalStrength sell_signal() const {
        int score = 0;

        // EMA bearish or crossed down
        if (ema_crossed_down()) score += 2;
        else if (ema_bearish()) score += 1;

        // RSI overbought
        if (is_extremely_overbought()) score += 2;
        else if (rsi_ > config_.rsi_mild_overbought) score += 1;

        // Above or near upper Bollinger band
        if (above_upper_band()) score += 2;
        else if (near_upper_band()) score += 1;

        if (score >= config_.signal_strong_threshold) return SignalStrength::Strong;
        if (score >= config_.signal_medium_threshold) return SignalStrength::Medium;
        if (score >= config_.signal_weak_threshold) return SignalStrength::Weak;
        return SignalStrength::None;
    }

    // ========================================
    // Utility
    // ========================================

    void reset() {
        count_ = 0;
        last_price_ = 0;
        ema_fast_ = 0;
        ema_slow_ = 0;
        ema_price_ = 0;
        ema_price_sq_ = 0;
        avg_gain_ = 0;
        avg_loss_ = 0;
        rsi_ = 50;
        bb_upper_ = 0;
        bb_lower_ = 0;
        ema_bullish_ = false;
        prev_ema_bullish_ = false;
    }

    int count() const { return count_; }
    bool ready() const { return count_ >= config_.min_samples; }

private:
    Config config_;

    // Alpha values (precomputed)
    double fast_alpha_;
    double slow_alpha_;
    double rsi_alpha_;
    double bb_alpha_;

    // State
    int count_ = 0;
    double last_price_ = 0;

    // EMA state
    double ema_fast_ = 0;
    double ema_slow_ = 0;
    bool ema_bullish_ = false;
    bool prev_ema_bullish_ = false;

    // RSI state (Wilder's smoothing)
    double avg_gain_ = 0;
    double avg_loss_ = 0;
    double rsi_ = 50;

    // Bollinger state
    double ema_price_ = 0;      // Middle band (EMA of price)
    double ema_price_sq_ = 0;   // EMA of price squared (for std dev)
    double bb_upper_ = 0;
    double bb_lower_ = 0;

    void update_ema(double price) {
        ema_fast_ = fast_alpha_ * price + (1 - fast_alpha_) * ema_fast_;
        ema_slow_ = slow_alpha_ * price + (1 - slow_alpha_) * ema_slow_;
    }

    void update_rsi(double change) {
        // Branchless gain/loss using std::max (compiles to cmov)
        double gain = std::max(0.0, change);
        double loss = std::max(0.0, -change);

        // Wilder's smoothing (similar to EMA)
        avg_gain_ = rsi_alpha_ * gain + (1 - rsi_alpha_) * avg_gain_;
        avg_loss_ = rsi_alpha_ * loss + (1 - rsi_alpha_) * avg_loss_;

        // Branchless RSI calculation: use small epsilon to avoid division by zero
        // instead of branch. This is faster and more predictable.
        static constexpr double EPSILON = 1e-10;
        double rs = avg_gain_ / (avg_loss_ + EPSILON);
        rsi_ = 100.0 - (100.0 / (1.0 + rs));
    }

    void update_bollinger(double price) {
        // EMA of price (middle band)
        ema_price_ = bb_alpha_ * price + (1 - bb_alpha_) * ema_price_;

        // EMA of price squared
        ema_price_sq_ = bb_alpha_ * (price * price) + (1 - bb_alpha_) * ema_price_sq_;

        // Standard deviation: sqrt(E[X^2] - E[X]^2)
        double variance = ema_price_sq_ - (ema_price_ * ema_price_);
        if (variance < 0) variance = 0;
        double std_dev = std::sqrt(variance);

        // Bands
        bb_upper_ = ema_price_ + config_.bb_std_dev * std_dev;
        bb_lower_ = ema_price_ - config_.bb_std_dev * std_dev;
    }
};

}  // namespace strategy
}  // namespace hft
