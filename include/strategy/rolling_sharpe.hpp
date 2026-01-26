#pragma once

/**
 * Rolling Sharpe Ratio Calculator
 *
 * Real-time Sharpe ratio calculation using Welford's online algorithm
 * for numerically stable incremental mean and variance.
 *
 * Usage:
 *   RollingSharpe sharpe(100);  // 100-trade window
 *
 *   // After each trade closes
 *   double return_pct = (exit_price - entry_price) / entry_price;
 *   sharpe.add_return(return_pct);
 *
 *   // Check current Sharpe
 *   if (sharpe.sharpe_ratio() < 0.5) {
 *       reduce_position_size();
 *   }
 *
 * Interpretation:
 *   Sharpe > 2.0  : Excellent (rare in live trading)
 *   Sharpe > 1.0  : Good
 *   Sharpe > 0.5  : Acceptable
 *   Sharpe < 0.5  : Poor, consider stopping
 *   Sharpe < 0    : Losing money on average
 */

#include <cmath>
#include <array>
#include <cstdint>
#include <algorithm>

namespace hft {
namespace strategy {

// Default window size for rolling calculations
static constexpr size_t DEFAULT_SHARPE_WINDOW = 100;

/**
 * Welford's Online Algorithm for mean and variance
 *
 * Numerically stable, O(1) per update, no storage of all values.
 * But we also keep a ring buffer for rolling window.
 */
template<size_t WindowSize = DEFAULT_SHARPE_WINDOW>
class RollingSharpe {
public:
    // Risk-free rate per trade (annualized 4% / ~2000 trades per year ≈ 0.002%)
    static constexpr double DEFAULT_RISK_FREE_PER_TRADE = 0.00002;

    explicit RollingSharpe(double risk_free_per_trade = DEFAULT_RISK_FREE_PER_TRADE)
        : risk_free_(risk_free_per_trade)
        , count_(0)
        , head_(0)
        , mean_(0)
        , m2_(0)  // Sum of squared differences from mean
    {
        returns_.fill(0);
    }

    /**
     * Add a new return observation
     * @param return_pct Return as decimal (0.01 = 1% gain)
     */
    void add_return(double return_pct) {
        if (count_ < WindowSize) {
            // Growing phase: standard Welford update
            count_++;
            double delta = return_pct - mean_;
            mean_ += delta / count_;
            double delta2 = return_pct - mean_;
            m2_ += delta * delta2;

            returns_[head_] = return_pct;
            head_ = (head_ + 1) % WindowSize;
        } else {
            // Full window: remove oldest, add newest
            double old_value = returns_[head_];
            double new_value = return_pct;

            // Update mean incrementally
            double old_mean = mean_;
            mean_ = mean_ + (new_value - old_value) / WindowSize;

            // Update M2 (sum of squared deviations)
            // This is the tricky part for rolling window
            m2_ = m2_ + (new_value - old_value) *
                        (new_value - mean_ + old_value - old_mean);

            returns_[head_] = new_value;
            head_ = (head_ + 1) % WindowSize;
        }
    }

    /**
     * Current mean return
     */
    double mean() const {
        return count_ > 0 ? mean_ : 0;
    }

    /**
     * Current variance (sample variance)
     */
    double variance() const {
        if (count_ < 2) return 0;
        return m2_ / (count_ - 1);
    }

    /**
     * Current standard deviation
     */
    double std_dev() const {
        return std::sqrt(variance());
    }

    /**
     * Sharpe Ratio = (Mean Return - Risk Free) / Std Dev
     *
     * Returns 0 if not enough data or zero volatility.
     */
    double sharpe_ratio() const {
        if (count_ < 10) return 0;  // Need minimum samples

        double sd = std_dev();
        if (sd < 1e-10) return 0;  // Avoid division by zero

        return (mean_ - risk_free_) / sd;
    }

    /**
     * Annualized Sharpe (assuming ~250 trading days, ~10 trades/day)
     * sqrt(2500) ≈ 50
     */
    double annualized_sharpe() const {
        return sharpe_ratio() * std::sqrt(2500.0);
    }

    /**
     * Number of returns in the window
     */
    size_t count() const { return count_; }

    /**
     * Is window full?
     */
    bool is_ready() const { return count_ >= WindowSize; }

    /**
     * Reset all statistics
     */
    void reset() {
        count_ = 0;
        head_ = 0;
        mean_ = 0;
        m2_ = 0;
        returns_.fill(0);
    }

    // === Trading Decision Helpers ===

    /**
     * Suggested position size multiplier based on Sharpe
     *
     * Sharpe > 1.5: 1.5x (aggressive)
     * Sharpe > 1.0: 1.0x (normal)
     * Sharpe > 0.5: 0.5x (cautious)
     * Sharpe < 0.5: 0.25x (minimal)
     * Sharpe < 0:   0x (stop trading)
     */
    double position_multiplier() const {
        double s = sharpe_ratio();
        if (s < 0) return 0;
        if (s < 0.5) return 0.25;
        if (s < 1.0) return 0.5;
        if (s < 1.5) return 1.0;
        return 1.5;
    }

    /**
     * Should we continue trading?
     */
    bool should_trade() const {
        if (count_ < 20) return true;  // Not enough data, allow trading
        return sharpe_ratio() > 0;      // Only trade if positive expectation
    }

    /**
     * Is strategy performing well?
     */
    bool is_performing_well() const {
        if (!is_ready()) return true;  // Assume OK until proven otherwise
        return sharpe_ratio() >= 0.5;
    }

    // === Statistics for debugging/logging ===

    struct Stats {
        size_t count;
        double mean;
        double std_dev;
        double sharpe;
        double annualized_sharpe;
        double position_mult;
        bool should_trade;
    };

    Stats get_stats() const {
        return Stats{
            count_,
            mean(),
            std_dev(),
            sharpe_ratio(),
            annualized_sharpe(),
            position_multiplier(),
            should_trade()
        };
    }

private:
    double risk_free_;
    size_t count_;
    size_t head_;
    double mean_;
    double m2_;
    std::array<double, WindowSize> returns_;
};

/**
 * Simple return calculator for closed trades
 */
struct TradeReturn {
    double entry_price;
    double exit_price;
    double quantity;
    bool is_long;

    double return_pct() const {
        if (entry_price <= 0) return 0;
        if (is_long) {
            return (exit_price - entry_price) / entry_price;
        } else {
            return (entry_price - exit_price) / entry_price;
        }
    }

    double pnl() const {
        if (is_long) {
            return (exit_price - entry_price) * quantity;
        } else {
            return (entry_price - exit_price) * quantity;
        }
    }
};

}  // namespace strategy
}  // namespace hft
