#pragma once

#include "istrategy.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace hft {
namespace strategy {

/**
 * FairValueStrategy - Mean reversion around calculated fair value
 *
 * Fair value trading philosophy:
 * - Price oscillates around a "true" value
 * - Buy when price is significantly below fair value
 * - Sell when price is significantly above fair value
 * - Profit from mean reversion
 *
 * Fair value calculation:
 * - Primary: EMA (smoothed price)
 * - Deviation: Bollinger Bands (standard deviation)
 * - Confirmation: Price velocity (is it reverting?)
 *
 * Order Preference:
 * - Always Limit orders (mean reversion is patient)
 * - Place orders inside the spread to capture reversion
 *
 * Suitable Regimes:
 * - Ranging: BEST (price oscillates around mean)
 * - LowVolatility: Good (stable fair value)
 * - TrendingUp/Down: AVOID (fair value keeps moving)
 * - HighVolatility: Risky (bands too wide)
 */
class FairValueStrategy : public IStrategy {
public:
    struct Config {
        // EMA period for fair value
        size_t fair_value_period = 20;

        // Standard deviation lookback
        size_t std_dev_period = 20;

        // Deviation thresholds (in standard deviations)
        double strong_deviation = 2.0; // 2 std dev = strong signal
        double medium_deviation = 1.5; // 1.5 std dev = medium signal
        double weak_deviation = 1.0;   // 1 std dev = weak signal

        // Minimum deviation in percentage (avoid tiny moves)
        double min_deviation_pct = 0.3; // At least 0.3% from fair value

        // Velocity check (is price moving back toward FV?)
        bool require_reversion = true; // Only enter if price is reverting
        size_t velocity_period = 3;    // Look at last N ticks for direction

        // Position sizing
        double base_position_pct = 0.1; // 10% of capital (conservative)
        double max_position_pct = 0.25; // Max 25% in single asset

        // Price scale
        double price_scale = 1e8;
    };

    FairValueStrategy() : config_{} { reset(); }

    explicit FairValueStrategy(const Config& config) : config_(config) { reset(); }

    // =========================================================================
    // IStrategy Implementation
    // =========================================================================

    Signal generate(Symbol symbol, const MarketSnapshot& market, const StrategyPosition& position,
                    MarketRegime regime) override {
        (void)symbol;

        if (!ready() || !market.valid()) {
            return Signal::none();
        }

        // Don't trade in unsuitable regimes
        if (!suitable_for_regime(regime)) {
            return Signal::none();
        }

        double current_price = market.mid_usd(config_.price_scale);
        double fv = fair_value();
        double std_dev = standard_deviation();

        // Calculate deviation from fair value
        double deviation = (current_price - fv) / fv * 100.0; // As percentage
        double deviation_sigmas = (std_dev > 0) ? (current_price - fv) / std_dev : 0;

        // Check for exit first (if we have position)
        if (position.has_position()) {
            return generate_exit_signal(market, position, deviation_sigmas, fv);
        }

        // Check for entry
        return generate_entry_signal(market, position, deviation, deviation_sigmas);
    }

    std::string_view name() const override { return "FairValue"; }

    OrderPreference default_order_preference() const override {
        return OrderPreference::Limit; // Mean reversion = patient
    }

    bool suitable_for_regime(MarketRegime regime) const override {
        switch (regime) {
        case MarketRegime::Ranging:
            return true; // BEST - price oscillates around mean
        case MarketRegime::LowVolatility:
            return true; // Good - stable fair value
        case MarketRegime::TrendingUp:
        case MarketRegime::TrendingDown:
            return false; // AVOID - fair value keeps moving
        case MarketRegime::HighVolatility:
            return false; // Bands too wide, risky
        default:
            return true; // Unknown, try it
        }
    }

    void on_tick(const MarketSnapshot& market) override {
        if (!market.valid())
            return;

        double price = market.mid_usd(config_.price_scale);

        // Store price in circular buffer
        prices_[price_idx_] = price;

        // Update EMA (fair value)
        if (sample_count_ == 0) {
            ema_ = price;
        } else {
            double alpha = 2.0 / (config_.fair_value_period + 1);
            ema_ = alpha * price + (1 - alpha) * ema_;
        }

        price_idx_ = (price_idx_ + 1) % MAX_PRICES;
        sample_count_++;
    }

    void reset() override {
        std::fill(std::begin(prices_), std::end(prices_), 0.0);
        price_idx_ = 0;
        sample_count_ = 0;
        ema_ = 0;
    }

    bool ready() const override { return sample_count_ >= std::max(config_.fair_value_period, config_.std_dev_period); }

    // =========================================================================
    // Accessors for debugging/dashboard
    // =========================================================================

    double fair_value() const { return ema_; }

    double standard_deviation() const {
        if (sample_count_ < config_.std_dev_period)
            return 0;

        // Calculate mean over std_dev_period
        double sum = 0;
        for (size_t i = 0; i < config_.std_dev_period; ++i) {
            size_t idx = (price_idx_ + MAX_PRICES - 1 - i) % MAX_PRICES;
            sum += prices_[idx];
        }
        double mean = sum / config_.std_dev_period;

        // Calculate variance
        double var_sum = 0;
        for (size_t i = 0; i < config_.std_dev_period; ++i) {
            size_t idx = (price_idx_ + MAX_PRICES - 1 - i) % MAX_PRICES;
            double diff = prices_[idx] - mean;
            var_sum += diff * diff;
        }

        return std::sqrt(var_sum / config_.std_dev_period);
    }

    double current_deviation_sigmas() const {
        double fv = fair_value();
        double std_dev = standard_deviation();
        if (std_dev <= 0 || sample_count_ == 0)
            return 0;

        double current_price = prices_[(price_idx_ + MAX_PRICES - 1) % MAX_PRICES];
        return (current_price - fv) / std_dev;
    }

private:
    static constexpr size_t MAX_PRICES = 128;

    Config config_;
    double prices_[MAX_PRICES];
    size_t price_idx_ = 0;
    size_t sample_count_ = 0;
    double ema_ = 0; // Fair value (EMA)

    bool is_price_reverting(bool expect_up) const {
        if (sample_count_ < config_.velocity_period + 1)
            return true; // Assume yes

        // Check if price is moving in expected direction
        size_t current_idx = (price_idx_ + MAX_PRICES - 1) % MAX_PRICES;
        size_t old_idx = (price_idx_ + MAX_PRICES - 1 - config_.velocity_period) % MAX_PRICES;

        double current = prices_[current_idx];
        double old = prices_[old_idx];

        if (expect_up) {
            return current > old; // Price is moving up (reverting from below)
        } else {
            return current < old; // Price is moving down (reverting from above)
        }
    }

    Signal generate_entry_signal(const MarketSnapshot& market, const StrategyPosition& position, double deviation_pct,
                                 double deviation_sigmas) {
        // Check minimum deviation
        if (std::abs(deviation_pct) < config_.min_deviation_pct) {
            return Signal::none(); // Too close to fair value
        }

        // Determine signal strength based on standard deviations
        SignalStrength strength = SignalStrength::None;
        if (std::abs(deviation_sigmas) >= config_.strong_deviation) {
            strength = SignalStrength::Strong;
        } else if (std::abs(deviation_sigmas) >= config_.medium_deviation) {
            strength = SignalStrength::Medium;
        } else if (std::abs(deviation_sigmas) >= config_.weak_deviation) {
            strength = SignalStrength::Weak;
        } else {
            return Signal::none(); // Deviation too small
        }

        // Determine direction
        // Negative deviation = price below FV = buy opportunity
        // Positive deviation = price above FV = sell opportunity
        bool should_buy = deviation_sigmas < 0;

        // Check reversion (if required)
        if (config_.require_reversion) {
            if (!is_price_reverting(should_buy)) {
                // Price still moving away from FV, wait
                if (strength > SignalStrength::Weak) {
                    strength = SignalStrength::Weak; // Downgrade
                } else {
                    return Signal::none();
                }
            }
        }

        // Calculate quantity
        double qty = calculate_qty(market, position);
        if (qty <= 0)
            return Signal::none();

        // Build signal
        Signal sig;
        sig.type = should_buy ? SignalType::Buy : SignalType::Sell;
        sig.strength = strength;
        sig.suggested_qty = qty;
        sig.order_pref = OrderPreference::Limit; // Always limit for FV

        // Set limit price inside spread (aggressive limit)
        Price spread = market.spread();
        if (should_buy) {
            // Buy: place above bid but below mid
            sig.limit_price = market.bid + (spread / 4);
            sig.reason = "Price below fair value - buy";
        } else {
            // Sell: place below ask but above mid
            sig.limit_price = market.ask - (spread / 4);
            sig.reason = "Price above fair value - sell";
        }

        return sig;
    }

    Signal generate_exit_signal(const MarketSnapshot& market, const StrategyPosition& position, double deviation_sigmas,
                                double fv) {
        // Exit conditions for fair value strategy:
        // 1. Price has reverted past fair value (profit taking)
        // 2. Price moved further away (stop loss - deviation > 3 sigma)

        double current_price = market.mid_usd(config_.price_scale);

        // For long position: exit if price >= fair value (reverted)
        // For short position: exit if price <= fair value (reverted)
        bool long_position = position.quantity > 0;

        if (long_position) {
            // Exit if price reverted to or above fair value
            if (current_price >= fv) {
                return Signal::exit(position.quantity, "Price reverted to fair value - take profit");
            }
            // Stop loss: deviation exceeded 3 sigma on wrong side
            if (deviation_sigmas < -3.0) {
                return Signal::exit(position.quantity, "Fair value stop loss - deviation > 3 sigma");
            }
        } else {
            // Short position
            if (current_price <= fv) {
                return Signal::exit(std::abs(position.quantity), "Price reverted to fair value - take profit");
            }
            if (deviation_sigmas > 3.0) {
                return Signal::exit(std::abs(position.quantity), "Fair value stop loss - deviation > 3 sigma");
            }
        }

        return Signal::none();
    }

    double calculate_qty(const MarketSnapshot& market, const StrategyPosition& position) const {
        double ask_usd = market.ask_usd(config_.price_scale);
        if (ask_usd <= 0)
            return 0;

        // Conservative position sizing for mean reversion
        double target_value = position.cash_available * config_.base_position_pct;
        double qty = target_value / ask_usd;

        // Cap at max position
        double max_qty = (position.max_position * config_.max_position_pct) / ask_usd;
        return std::min(qty, max_qty);
    }
};

} // namespace strategy
} // namespace hft
