#pragma once

#include "../types.hpp"
#include "../exchange/market_data.hpp"
#include <deque>
#include <cmath>
#include <algorithm>
#include <string>

namespace hft {
namespace strategy {

/**
 * Market Regime Types
 */
enum class MarketRegime {
    Unknown,
    TrendingUp,      // Strong upward trend
    TrendingDown,    // Strong downward trend
    Ranging,         // Sideways, mean-reverting
    HighVolatility,  // Choppy, high uncertainty
    LowVolatility,   // Quiet, low movement
    Spike            // Sudden price spike detected
};

inline std::string regime_to_string(MarketRegime regime) {
    switch (regime) {
        case MarketRegime::TrendingUp: return "TRENDING_UP";
        case MarketRegime::TrendingDown: return "TRENDING_DOWN";
        case MarketRegime::Ranging: return "RANGING";
        case MarketRegime::HighVolatility: return "HIGH_VOL";
        case MarketRegime::LowVolatility: return "LOW_VOL";
        case MarketRegime::Spike: return "SPIKE";
        default: return "UNKNOWN";
    }
}

/**
 * Regime Detection Configuration
 */
struct RegimeConfig {
    int lookback = 20;              // Lookback period for calculations

    // Trend detection
    double trend_threshold = 0.02;   // 2% price change = trend
    int trend_ma_period = 20;        // MA period for trend
    double adx_threshold = 25.0;     // ADX > 25 = trending

    // Volatility detection
    double high_vol_threshold = 0.03;  // 3% daily vol = high
    double low_vol_threshold = 0.01;   // 1% daily vol = low

    // Mean reversion detection (simplified Hurst)
    double mean_reversion_threshold = 0.4;  // < 0.4 = mean reverting
    double trending_threshold = 0.6;        // > 0.6 = trending

    // Spike detection
    double spike_threshold = 3.0;      // Multiplier of avg move for spike detection
    int spike_lookback = 10;           // Bars to use for average move calculation
    double spike_min_move = 0.005;     // Minimum move (0.5%) to consider as spike
    int spike_cooldown = 5;            // Bars to wait after spike before re-detection
};

/**
 * Regime Detector
 *
 * Detects market regime using:
 * 1. Trend: Price vs Moving Average + momentum
 * 2. Volatility: ATR / Standard Deviation
 * 3. Mean Reversion: Simplified Hurst-like indicator
 */
class RegimeDetector {
public:
    explicit RegimeDetector(const RegimeConfig& config = RegimeConfig())
        : config_(config)
        , current_regime_(MarketRegime::Unknown)
        , trend_strength_(0)
        , volatility_(0)
        , mean_reversion_score_(0.5)
    {}

    /**
     * Update with new price data
     */
    void update(double price) {
        prices_.push_back(price);

        if (prices_.size() > static_cast<size_t>(config_.lookback * 2)) {
            prices_.pop_front();
        }

        if (prices_.size() >= static_cast<size_t>(config_.lookback)) {
            calculate_indicators();
            detect_regime();
        }
    }

    /**
     * Update with kline data (more information)
     */
    void update(const exchange::Kline& kline) {
        double close = kline.close / 10000.0;
        double high = kline.high / 10000.0;
        double low = kline.low / 10000.0;

        prices_.push_back(close);
        highs_.push_back(high);
        lows_.push_back(low);

        size_t max_size = static_cast<size_t>(config_.lookback * 2);
        while (prices_.size() > max_size) {
            prices_.pop_front();
            highs_.pop_front();
            lows_.pop_front();
        }

        if (prices_.size() >= static_cast<size_t>(config_.lookback)) {
            calculate_indicators();
            detect_regime();
        }
    }

    MarketRegime current_regime() const { return current_regime_; }
    double trend_strength() const { return trend_strength_; }
    double volatility() const { return volatility_; }
    double mean_reversion_score() const { return mean_reversion_score_; }

    /**
     * Get regime confidence (0-1)
     */
    double confidence() const {
        // Higher confidence when indicators strongly agree
        double vol_clarity = std::abs(volatility_ - 0.02) / 0.02;
        double trend_clarity = std::abs(trend_strength_);
        return std::min(1.0, (vol_clarity + trend_clarity) / 2);
    }

    /**
     * Is the market suitable for mean reversion strategies?
     */
    bool is_mean_reverting() const {
        return current_regime_ == MarketRegime::Ranging ||
               current_regime_ == MarketRegime::LowVolatility;
    }

    /**
     * Is the market suitable for trend-following strategies?
     */
    bool is_trending() const {
        return current_regime_ == MarketRegime::TrendingUp ||
               current_regime_ == MarketRegime::TrendingDown;
    }

    /**
     * Is there a price spike detected?
     */
    bool is_spike() const {
        return current_regime_ == MarketRegime::Spike;
    }

    /**
     * Is the market in a dangerous state (high vol or spike)?
     */
    bool is_dangerous() const {
        return current_regime_ == MarketRegime::HighVolatility ||
               current_regime_ == MarketRegime::Spike;
    }

    void reset() {
        prices_.clear();
        highs_.clear();
        lows_.clear();
        current_regime_ = MarketRegime::Unknown;
        trend_strength_ = 0;
        volatility_ = 0;
        mean_reversion_score_ = 0.5;
    }

private:
    RegimeConfig config_;
    std::deque<double> prices_;
    std::deque<double> highs_;
    std::deque<double> lows_;

    MarketRegime current_regime_;
    double trend_strength_;      // -1 (down) to +1 (up), 0 = no trend
    double volatility_;          // Annualized volatility estimate
    double mean_reversion_score_; // 0 = strong MR, 0.5 = random, 1 = trending

    void calculate_indicators() {
        calculate_trend();
        calculate_volatility();
        calculate_mean_reversion();
    }

    void calculate_trend() {
        if (prices_.size() < 2) return;

        // Simple trend: compare current price to MA
        double ma = 0;
        int count = std::min(static_cast<int>(prices_.size()), config_.trend_ma_period);
        auto it = prices_.end() - count;
        for (int i = 0; i < count; ++i, ++it) {
            ma += *it;
        }
        ma /= count;

        double current = prices_.back();
        double pct_from_ma = (current - ma) / ma;

        // Also consider momentum (rate of change)
        int momentum_period = std::min(10, static_cast<int>(prices_.size()) - 1);
        double past_price = *(prices_.end() - momentum_period - 1);
        double momentum = (current - past_price) / past_price;

        // Combine MA position and momentum
        trend_strength_ = (pct_from_ma + momentum) / 2;

        // Clamp to [-1, 1]
        trend_strength_ = std::max(-1.0, std::min(1.0, trend_strength_ * 10));
    }

    void calculate_volatility() {
        if (prices_.size() < 2) return;

        // Calculate returns
        std::vector<double> returns;
        for (size_t i = 1; i < prices_.size(); ++i) {
            if (prices_[i-1] != 0) {
                returns.push_back((prices_[i] - prices_[i-1]) / prices_[i-1]);
            }
        }

        if (returns.empty()) return;

        // Standard deviation of returns
        double mean = 0;
        for (double r : returns) mean += r;
        mean /= returns.size();

        double variance = 0;
        for (double r : returns) {
            variance += (r - mean) * (r - mean);
        }
        variance /= returns.size();

        // Annualize (assuming hourly data, ~8760 hours/year)
        volatility_ = std::sqrt(variance) * std::sqrt(24.0);  // Daily vol
    }

    void calculate_mean_reversion() {
        // Simplified mean reversion indicator
        // Based on variance ratio test concept

        if (prices_.size() < static_cast<size_t>(config_.lookback)) return;

        // Calculate short-term and long-term variance
        std::vector<double> short_returns, long_returns;

        // Short returns (1-period)
        size_t start_idx = prices_.size() - config_.lookback;
        if (start_idx == 0) start_idx = 1;  // Avoid underflow at i-1
        for (size_t i = start_idx; i < prices_.size(); ++i) {
            if (prices_[i-1] != 0) {
                short_returns.push_back((prices_[i] - prices_[i-1]) / prices_[i-1]);
            }
        }

        // Long returns (5-period)
        for (size_t i = prices_.size() - config_.lookback; i < prices_.size(); i += 5) {
            size_t prev = (i >= 5) ? i - 5 : 0;
            if (prices_[prev] != 0) {
                long_returns.push_back((prices_[i] - prices_[prev]) / prices_[prev]);
            }
        }

        if (short_returns.empty() || long_returns.empty()) return;

        // Variance of short returns
        double short_var = 0, short_mean = 0;
        for (double r : short_returns) short_mean += r;
        short_mean /= short_returns.size();
        for (double r : short_returns) short_var += (r - short_mean) * (r - short_mean);
        short_var /= short_returns.size();

        // Variance of long returns
        double long_var = 0, long_mean = 0;
        for (double r : long_returns) long_mean += r;
        long_mean /= long_returns.size();
        for (double r : long_returns) long_var += (r - long_mean) * (r - long_mean);
        long_var /= long_returns.size();

        // Variance ratio
        // If VR < 1: Mean reverting (short-term variance > expected)
        // If VR > 1: Trending (long-term variance accumulates)
        // VR = 1: Random walk
        if (short_var > 0 && long_returns.size() > 1) {
            double expected_long_var = short_var * 5;  // Under random walk
            double vr = long_var / expected_long_var;

            // Map to 0-1 score
            // VR < 0.7 -> mean reverting (score < 0.4)
            // VR 0.7-1.3 -> random (score ~0.5)
            // VR > 1.3 -> trending (score > 0.6)
            mean_reversion_score_ = std::max(0.0, std::min(1.0, vr / 2.0));
        }
    }

    void detect_regime() {
        // Priority-based regime detection

        // 1. Check for high volatility first (overrides other signals)
        if (volatility_ > config_.high_vol_threshold) {
            current_regime_ = MarketRegime::HighVolatility;
            return;
        }

        // 2. Check for strong trend
        if (std::abs(trend_strength_) > 0.3) {
            if (trend_strength_ > 0) {
                current_regime_ = MarketRegime::TrendingUp;
            } else {
                current_regime_ = MarketRegime::TrendingDown;
            }
            return;
        }

        // 3. Check for low volatility
        if (volatility_ < config_.low_vol_threshold) {
            current_regime_ = MarketRegime::LowVolatility;
            return;
        }

        // 4. Check mean reversion score
        if (mean_reversion_score_ < config_.mean_reversion_threshold) {
            current_regime_ = MarketRegime::Ranging;
            return;
        }

        if (mean_reversion_score_ > config_.trending_threshold) {
            // Weak trend, determine direction
            current_regime_ = (trend_strength_ >= 0) ?
                MarketRegime::TrendingUp : MarketRegime::TrendingDown;
            return;
        }

        // Default to ranging if no strong signal
        current_regime_ = MarketRegime::Ranging;
    }
};

}  // namespace strategy
}  // namespace hft
