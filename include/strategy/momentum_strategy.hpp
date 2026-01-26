#pragma once

#include "istrategy.hpp"
#include <cmath>
#include <algorithm>

namespace hft {
namespace strategy {

/**
 * MomentumStrategy - Trades in direction of price momentum
 *
 * Momentum trading philosophy:
 * - "Trend is your friend" - ride the wave
 * - Buy strength, sell weakness
 * - Cut losers quickly, let winners run
 *
 * Indicators used:
 * - Rate of Change (ROC): price[now] / price[n] - 1
 * - Momentum EMA: smoothed rate of price change
 * - Trend strength: how consistent is the direction
 *
 * Order Preference:
 * - Strong signals → Market (momentum is time-sensitive!)
 * - Medium signals → Either
 * - Weak signals → Limit (wait for better entry)
 *
 * Suitable Regimes:
 * - TrendingUp: BEST for longs
 * - TrendingDown: BEST for shorts (if allowed)
 * - Ranging: AVOID (whipsaws)
 * - HighVolatility: Risky but can work
 * - LowVolatility: Poor (no momentum to capture)
 */
class MomentumStrategy : public IStrategy {
public:
    struct Config {
        // ROC period (how far back to look)
        size_t roc_period = 10;

        // EMA period for smoothing momentum
        size_t momentum_ema_period = 5;

        // Thresholds (as percentage)
        double strong_momentum_pct = 0.5;   // 0.5% = strong signal
        double medium_momentum_pct = 0.2;   // 0.2% = medium signal
        double weak_momentum_pct = 0.1;     // 0.1% = weak signal

        // Exit thresholds
        double momentum_reversal_pct = -0.1;  // Exit if momentum reverses

        // Position sizing
        double base_position_pct = 0.15;    // 15% of capital (aggressive)
        double max_position_pct = 0.4;      // Max 40% in single asset

        // Price scale
        double price_scale = 1e8;

        // Allow short selling
        bool allow_shorts = false;
    };

    MomentumStrategy() : config_{} { reset(); }

    explicit MomentumStrategy(const Config& config)
        : config_(config)
    {
        reset();
    }

    // =========================================================================
    // IStrategy Implementation
    // =========================================================================

    Signal generate(
        Symbol symbol,
        const MarketSnapshot& market,
        const StrategyPosition& position,
        MarketRegime regime
    ) override {
        (void)symbol;

        if (!ready() || !market.valid()) {
            return Signal::none();
        }

        // Don't trade in unsuitable regimes
        if (!suitable_for_regime(regime)) {
            return Signal::none();
        }

        double momentum = current_momentum();
        double momentum_ema = momentum_ema_;

        // Check for exit first (if we have position)
        if (position.has_position()) {
            return generate_exit_signal(market, position, regime, momentum);
        }

        // Check for entry
        return generate_entry_signal(market, position, regime, momentum);
    }

    std::string_view name() const override {
        return "Momentum";
    }

    OrderPreference default_order_preference() const override {
        return OrderPreference::Market;  // Momentum = time-sensitive
    }

    bool suitable_for_regime(MarketRegime regime) const override {
        switch (regime) {
            case MarketRegime::TrendingUp:
            case MarketRegime::TrendingDown:
                return true;  // BEST - this is what momentum is for
            case MarketRegime::HighVolatility:
                return true;  // Risky but can work
            case MarketRegime::Ranging:
                return false; // AVOID - whipsaws kill momentum traders
            case MarketRegime::LowVolatility:
                return false; // Poor - no momentum to capture
            default:
                return true;  // Unknown, try it
        }
    }

    void on_tick(const MarketSnapshot& market) override {
        if (!market.valid()) return;

        double price = market.mid_usd(config_.price_scale);

        // Store price in circular buffer
        prices_[price_idx_] = price;

        // Update momentum EMA
        if (sample_count_ >= config_.roc_period) {
            double roc = current_momentum();
            if (sample_count_ == config_.roc_period) {
                momentum_ema_ = roc;  // Initialize
            } else {
                double alpha = 2.0 / (config_.momentum_ema_period + 1);
                momentum_ema_ = alpha * roc + (1 - alpha) * momentum_ema_;
            }
        }

        price_idx_ = (price_idx_ + 1) % MAX_PRICES;
        sample_count_++;
    }

    void reset() override {
        std::fill(std::begin(prices_), std::end(prices_), 0.0);
        price_idx_ = 0;
        sample_count_ = 0;
        momentum_ema_ = 0;
    }

    bool ready() const override {
        return sample_count_ >= config_.roc_period + config_.momentum_ema_period;
    }

    // =========================================================================
    // Accessors for debugging/dashboard
    // =========================================================================

    double current_momentum() const {
        if (sample_count_ < config_.roc_period) return 0;

        size_t old_idx = (price_idx_ + MAX_PRICES - config_.roc_period) % MAX_PRICES;
        double old_price = prices_[old_idx];
        double current_price = prices_[(price_idx_ + MAX_PRICES - 1) % MAX_PRICES];

        if (old_price <= 0) return 0;
        return ((current_price / old_price) - 1.0) * 100.0;  // As percentage
    }

    double momentum_ema() const { return momentum_ema_; }

private:
    static constexpr size_t MAX_PRICES = 128;

    Config config_;
    double prices_[MAX_PRICES];
    size_t price_idx_ = 0;
    size_t sample_count_ = 0;
    double momentum_ema_ = 0;

    Signal generate_entry_signal(
        const MarketSnapshot& market,
        const StrategyPosition& position,
        MarketRegime regime,
        double momentum
    ) {
        // Use smoothed momentum for entries
        double mom = momentum_ema_;

        // Only long entries for now (unless shorts allowed)
        if (mom <= 0 && !config_.allow_shorts) {
            return Signal::none();
        }

        // Determine signal strength based on momentum magnitude
        SignalStrength strength = SignalStrength::None;
        if (std::abs(mom) >= config_.strong_momentum_pct) {
            strength = SignalStrength::Strong;
        } else if (std::abs(mom) >= config_.medium_momentum_pct) {
            strength = SignalStrength::Medium;
        } else if (std::abs(mom) >= config_.weak_momentum_pct) {
            strength = SignalStrength::Weak;
        } else {
            return Signal::none();  // Momentum too weak
        }

        // Check regime alignment
        // Don't buy in downtrend, don't sell in uptrend
        if (mom > 0 && regime == MarketRegime::TrendingDown) {
            strength = SignalStrength::Weak;  // Downgrade
        }
        if (mom < 0 && regime == MarketRegime::TrendingUp) {
            strength = SignalStrength::Weak;  // Downgrade
        }

        // Calculate quantity
        double qty = calculate_qty(market, position);
        if (qty <= 0) return Signal::none();

        // Build signal
        Signal sig;
        sig.type = (mom > 0) ? SignalType::Buy : SignalType::Sell;
        sig.strength = strength;
        sig.suggested_qty = qty;

        // Momentum = time-sensitive, prefer market orders
        if (strength >= SignalStrength::Strong) {
            sig.order_pref = OrderPreference::Market;
            sig.reason = mom > 0 ? "Strong upward momentum" : "Strong downward momentum";
        } else if (strength >= SignalStrength::Medium) {
            sig.order_pref = OrderPreference::Either;
            sig.reason = mom > 0 ? "Medium upward momentum" : "Medium downward momentum";
        } else {
            sig.order_pref = OrderPreference::Limit;
            sig.limit_price = market.mid();
            sig.reason = mom > 0 ? "Weak upward momentum" : "Weak downward momentum";
        }

        return sig;
    }

    Signal generate_exit_signal(
        const MarketSnapshot& market,
        const StrategyPosition& position,
        MarketRegime regime,
        double momentum
    ) {
        double mom = momentum_ema_;

        // Exit conditions:
        // 1. Momentum reversal (was positive, now negative or vice versa)
        // 2. Regime change to unfavorable
        // 3. Strong opposite momentum

        bool momentum_reversal = (position.quantity > 0 && mom < config_.momentum_reversal_pct);
        bool regime_unfavorable = (position.quantity > 0 && regime == MarketRegime::TrendingDown);
        bool strong_opposite = (position.quantity > 0 && mom < -config_.medium_momentum_pct);

        if (momentum_reversal || regime_unfavorable || strong_opposite) {
            const char* reason = "Momentum exit";
            if (momentum_reversal) reason = "Momentum reversal - exit";
            if (regime_unfavorable) reason = "Regime turned bearish - exit";
            if (strong_opposite) reason = "Strong opposite momentum - exit";

            return Signal::exit(position.quantity, reason);
        }

        return Signal::none();
    }

    double calculate_qty(const MarketSnapshot& market, const StrategyPosition& position) const {
        double ask_usd = market.ask_usd(config_.price_scale);
        if (ask_usd <= 0) return 0;

        // Aggressive position sizing for momentum
        double target_value = position.cash_available * config_.base_position_pct;
        double qty = target_value / ask_usd;

        // Cap at max position
        double max_qty = (position.max_position * config_.max_position_pct) / ask_usd;
        return std::min(qty, max_qty);
    }
};

}  // namespace strategy
}  // namespace hft
