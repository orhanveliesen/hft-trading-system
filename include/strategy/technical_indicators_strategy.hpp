#pragma once

#include "../risk/enhanced_risk_manager.hpp"
#include "istrategy.hpp"
#include "technical_indicators.hpp"

namespace hft {
namespace strategy {

/**
 * TechnicalIndicatorsStrategy - IStrategy adapter for TechnicalIndicators
 *
 * Wraps the existing TechnicalIndicators class to conform to IStrategy interface.
 * Uses RSI, EMA crossover, and Bollinger Bands for signal generation.
 *
 * Order Preference:
 * - Strong signals → Market (don't miss the opportunity)
 * - Medium/Weak signals → Limit (save slippage)
 *
 * Suitable Regimes:
 * - Ranging: Best (mean reversion works well)
 * - TrendingUp: Good for buying
 * - TrendingDown: Good for selling
 * - HighVolatility: Avoid (indicators lag, whipsaws)
 */
class TechnicalIndicatorsStrategy : public IStrategy {
public:
    struct Config {
        TechnicalIndicatorsConfig indicator_config;

        // Position sizing
        double base_position_pct = 0.1; // 10% of capital per trade
        double max_position_pct = 0.3;  // Max 30% in single asset

        // Price scale for USD conversion
        double price_scale = 1e8; // risk::PRICE_SCALE

        Config() : indicator_config{} {}
    };

    TechnicalIndicatorsStrategy() : config_{}, indicators_(config_.indicator_config) {}

    explicit TechnicalIndicatorsStrategy(const Config& config)
        : config_(config), indicators_(config.indicator_config) {}

    // =========================================================================
    // IStrategy Implementation
    // =========================================================================

    Signal generate(Symbol symbol, const MarketSnapshot& market, const StrategyPosition& position,
                    MarketRegime regime) override {
        (void)symbol; // Not used, but available

        if (!ready() || !market.valid()) {
            return Signal::none();
        }

        // Get signals from technical indicators
        auto buy_strength = indicators_.buy_signal();
        auto sell_strength = indicators_.sell_signal();

        // Check for exit signals first (if we have position)
        if (position.has_position()) {
            return generate_exit_signal(market, position, regime, sell_strength);
        }

        // Check for entry signals (if we can buy)
        if (position.can_buy()) {
            return generate_entry_signal(market, position, regime, buy_strength);
        }

        return Signal::none();
    }

    std::string_view name() const override { return "TechnicalIndicators"; }

    OrderPreference default_order_preference() const override {
        return OrderPreference::Either; // Let signal strength decide
    }

    bool suitable_for_regime(MarketRegime regime) const override {
        switch (regime) {
        case MarketRegime::Ranging:
        case MarketRegime::LowVolatility:
            return true; // Best for mean reversion signals
        case MarketRegime::TrendingUp:
        case MarketRegime::TrendingDown:
            return true; // Can work with trend
        case MarketRegime::HighVolatility:
            return false; // Indicators lag, avoid
        default:
            return true; // Unknown, allow
        }
    }

    void on_tick(const MarketSnapshot& market) override {
        if (market.valid()) {
            double mid_usd = market.mid_usd(config_.price_scale);
            indicators_.update(mid_usd);
        }
    }

    void reset() override { indicators_.reset(); }

    bool ready() const override { return indicators_.ready(); }

    // =========================================================================
    // Accessors for debugging/dashboard
    // =========================================================================

    const TechnicalIndicators& indicators() const { return indicators_; }
    double rsi() const { return indicators_.rsi(); }
    double ema_fast() const { return indicators_.ema_fast(); }
    double ema_slow() const { return indicators_.ema_slow(); }

private:
    Config config_;
    TechnicalIndicators indicators_;

    // Convert TechnicalIndicators::SignalStrength to strategy::SignalStrength
    SignalStrength convert_strength(TechnicalIndicators::SignalStrength s) const {
        switch (s) {
        case TechnicalIndicators::SignalStrength::Strong:
            return SignalStrength::Strong;
        case TechnicalIndicators::SignalStrength::Medium:
            return SignalStrength::Medium;
        case TechnicalIndicators::SignalStrength::Weak:
            return SignalStrength::Weak;
        default:
            return SignalStrength::None;
        }
    }

    Signal generate_entry_signal(const MarketSnapshot& market, const StrategyPosition& position, MarketRegime regime,
                                 TechnicalIndicators::SignalStrength buy_strength) {
        using SS = TechnicalIndicators::SignalStrength;

        // No signal if weak and in bad regime
        if (buy_strength == SS::None)
            return Signal::none();
        if (buy_strength == SS::Weak && regime == MarketRegime::TrendingDown) {
            return Signal::none();
        }

        // Calculate quantity
        double qty = calculate_qty(market, position);
        if (qty <= 0)
            return Signal::none();

        // Build signal
        Signal sig;
        sig.type = SignalType::Buy;
        sig.strength = convert_strength(buy_strength);
        sig.suggested_qty = qty;

        // Order type decision based on strength
        if (buy_strength >= SS::Strong) {
            sig.order_pref = OrderPreference::Market;
            sig.reason = "Strong buy: RSI oversold + EMA bullish";
        } else if (buy_strength >= SS::Medium) {
            sig.order_pref = OrderPreference::Either; // Let execution decide
            sig.limit_price = market.mid();           // Suggest mid for limit
            sig.reason = "Medium buy: Multiple indicators aligned";
        } else {
            sig.order_pref = OrderPreference::Limit;              // Weak = passive
            sig.limit_price = market.bid + (market.spread() / 4); // Aggressive limit
            sig.reason = "Weak buy: Some indicators positive";
        }

        return sig;
    }

    Signal generate_exit_signal(const MarketSnapshot& market, const StrategyPosition& position, MarketRegime regime,
                                TechnicalIndicators::SignalStrength sell_strength) {
        using SS = TechnicalIndicators::SignalStrength;

        // Strong exit conditions
        bool trend_reversal = (regime == MarketRegime::TrendingDown);
        bool strong_sell = (sell_strength >= SS::Strong);
        bool rsi_overbought = indicators_.is_overbought();

        if (trend_reversal || strong_sell) {
            // Exit immediately
            return Signal::exit(position.quantity, trend_reversal ? "Trend reversal - exit" : "Strong sell signal");
        }

        // Medium sell + overbought = exit
        if (sell_strength >= SS::Medium && rsi_overbought) {
            Signal sig = Signal::sell(SignalStrength::Medium, position.quantity, "Medium sell + RSI overbought");
            sig.order_pref = OrderPreference::Market; // Exit quickly
            return sig;
        }

        // Weak sell in high volatility = partial exit
        if (sell_strength >= SS::Weak && regime == MarketRegime::HighVolatility) {
            Signal sig =
                Signal::sell(SignalStrength::Weak, position.quantity * 0.5, "Weak sell in high volatility - reduce");
            sig.order_pref = OrderPreference::Limit;
            sig.limit_price = market.ask - (market.spread() / 4); // Aggressive ask
            return sig;
        }

        return Signal::none();
    }

    double calculate_qty(const MarketSnapshot& market, const StrategyPosition& position) const {
        double ask_usd = market.ask_usd(config_.price_scale);
        if (ask_usd <= 0)
            return 0;

        // Position size = base_position_pct * available_cash / price
        double target_value = position.cash_available * config_.base_position_pct;
        double qty = target_value / ask_usd;

        // Cap at max position
        double max_qty = (position.max_position * config_.max_position_pct) / ask_usd;
        return std::min(qty, max_qty);
    }
};

} // namespace strategy
} // namespace hft
