#pragma once

#include "istrategy.hpp"
#include "market_maker.hpp"

namespace hft {
namespace strategy {

/**
 * MarketMakerStrategy - IStrategy adapter for MarketMaker
 *
 * Market making strategy that:
 * - Always uses LIMIT orders (no slippage!)
 * - Provides two-sided liquidity (bid and ask)
 * - Skews quotes based on inventory
 * - Profits from bid-ask spread
 *
 * Order Preference: Always Limit
 *
 * Suitable Regimes:
 * - Ranging: Best (stable spread)
 * - LowVolatility: Good (predictable fills)
 * - HighVolatility: Avoid (spread can blow out, adverse selection)
 * - Trending: Risky (get run over)
 *
 * Note: This strategy alternates between Buy and Sell signals
 * based on current position. A full MM would post both sides
 * simultaneously, but IStrategy only returns one signal at a time.
 */
class MarketMakerStrategy : public IStrategy {
public:
    struct Config {
        MarketMakerConfig mm_config;
        double price_scale = 1e8;

        // Which side to quote (both, buy_only, sell_only)
        bool quote_bids = true;
        bool quote_asks = true;

        // Minimum spread to quote (bps) - don't quote if spread too tight
        double min_spread_bps = 5.0;

        Config() : mm_config{} {}
    };

    MarketMakerStrategy() : config_{}, mm_(config_.mm_config), last_mid_(0), samples_(0) {}

    explicit MarketMakerStrategy(const Config& config)
        : config_(config)
        , mm_(config.mm_config)
        , last_mid_(0)
        , samples_(0)
    {}

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

        // Check if spread is wide enough to profit
        if (market.spread_bps() < config_.min_spread_bps) {
            return Signal::none();  // Spread too tight, no edge
        }

        // Generate quotes from MarketMaker
        int64_t position_scaled = static_cast<int64_t>(position.quantity * config_.price_scale);
        Quote quote = mm_.generate_quotes(market.mid(), position_scaled);

        // Decide which side to quote based on position
        // If long, prefer to sell (reduce inventory)
        // If short, prefer to buy (reduce inventory)
        // If flat, quote the more profitable side

        double position_ratio = position.quantity / config_.mm_config.max_position;

        if (position_ratio > 0.5 && quote.has_ask && config_.quote_asks) {
            // Long inventory - prioritize selling
            return create_sell_signal(quote, market);
        } else if (position_ratio < -0.5 && quote.has_bid && config_.quote_bids) {
            // Short inventory - prioritize buying
            return create_buy_signal(quote, market);
        } else {
            // Neutral - alternate or pick based on order book imbalance
            bool bid_pressure = market.bid_size > market.ask_size;
            if (bid_pressure && quote.has_ask && config_.quote_asks) {
                // More buyers, sell to them
                return create_sell_signal(quote, market);
            } else if (quote.has_bid && config_.quote_bids) {
                // More sellers, buy from them
                return create_buy_signal(quote, market);
            }
        }

        return Signal::none();
    }

    std::string_view name() const override {
        return "MarketMaker";
    }

    OrderPreference default_order_preference() const override {
        return OrderPreference::Limit;  // Always limit - that's the whole point!
    }

    bool suitable_for_regime(MarketRegime regime) const override {
        switch (regime) {
            case MarketRegime::Ranging:
            case MarketRegime::LowVolatility:
                return true;  // Ideal for MM
            case MarketRegime::TrendingUp:
            case MarketRegime::TrendingDown:
                return false; // Trending = adverse selection risk
            case MarketRegime::HighVolatility:
                return false; // Spread blows out, inventory risk
            default:
                return true;  // Unknown, try it
        }
    }

    void on_tick(const MarketSnapshot& market) override {
        if (market.valid()) {
            last_mid_ = market.mid();
            samples_++;
        }
    }

    void reset() override {
        last_mid_ = 0;
        samples_ = 0;
    }

    bool ready() const override {
        return samples_ >= 10;  // Need some price history
    }

    // =========================================================================
    // Accessors
    // =========================================================================

    const MarketMaker& market_maker() const { return mm_; }

private:
    Config config_;
    MarketMaker mm_;
    Price last_mid_;
    int samples_;

    Signal create_buy_signal(const Quote& quote, const MarketSnapshot& market) {
        double qty_usd = quote.bid_size / config_.price_scale;

        Signal sig;
        sig.type = SignalType::Buy;
        sig.strength = SignalStrength::Weak;  // MM signals are always passive
        sig.order_pref = OrderPreference::Limit;  // Always limit!
        sig.suggested_qty = qty_usd;
        sig.limit_price = quote.bid_price;
        sig.reason = "MM bid quote";

        return sig;
    }

    Signal create_sell_signal(const Quote& quote, const MarketSnapshot& market) {
        double qty_usd = quote.ask_size / config_.price_scale;

        Signal sig;
        sig.type = SignalType::Sell;
        sig.strength = SignalStrength::Weak;  // MM signals are always passive
        sig.order_pref = OrderPreference::Limit;  // Always limit!
        sig.suggested_qty = qty_usd;
        sig.limit_price = quote.ask_price;
        sig.reason = "MM ask quote";

        return sig;
    }
};

}  // namespace strategy
}  // namespace hft
