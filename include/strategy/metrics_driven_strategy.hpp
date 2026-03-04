#pragma once

#include "istrategy.hpp"
#include "metrics_context.hpp"

#include <cmath>

namespace hft {
namespace strategy {

/**
 * MetricsDrivenStrategy - Multi-factor strategy using all 5 metrics classes
 *
 * Scoring System (score range: [-100, +100]):
 *
 * Factor 1: Trade Flow Imbalance (weight 25%)
 *   - Source: TradeStreamMetrics::Metrics (W5s window)
 *   - Signal: buy_ratio = buy_volume / total_volume
 *   - buy_ratio > 0.65 → +25 (bullish)
 *   - buy_ratio < 0.35 → -25 (bearish)
 *   - Linear interpolation between
 *
 * Factor 2: Order Book Pressure (weight 20%)
 *   - Source: OrderBookMetrics::Metrics
 *   - Signal: top_imbalance (bid vs ask depth at best levels)
 *   - top_imbalance > 0.3 → +20 (more bid depth = support)
 *   - top_imbalance < -0.3 → -20 (more ask depth = resistance)
 *   - Dampen if spread_bps > 20 (illiquid)
 *
 * Factor 3: Order Flow Toxicity (weight 20%)
 *   - Source: OrderFlowMetrics::Metrics (SEC_5 window)
 *   - Cancel ratio asymmetry:
 *     - High cancel_ratio_bid + low cancel_ratio_ask → -20 (fake bids)
 *     - High cancel_ratio_ask + low cancel_ratio_bid → +20 (fake asks)
 *   - Volume removed: bid_volume_removed > ask_volume_removed → -20 (support eroding)
 *
 * Factor 4: Futures Sentiment (weight 20%)
 *   - Source: FuturesMetrics::Metrics (W5s window)
 *   - funding_rate_extreme + positive rate → -20 (overleveraged longs, contrarian)
 *   - funding_rate_extreme + negative rate → +20 (overleveraged shorts, contrarian)
 *   - liquidation_imbalance > 0.5 → -20 (long liquidation cascade)
 *   - liquidation_imbalance < -0.5 → +20 (short squeeze)
 *   - |basis_bps| > 50 → reduce score by 30% (regime uncertainty)
 *
 * Factor 5: Absorption Detection (weight 15%)
 *   - Source: CombinedMetrics::Metrics (SEC_5 window)
 *   - absorption_ratio_bid > 2.0 → +15 (strong bid absorption = bullish)
 *   - absorption_ratio_ask > 2.0 → -15 (strong ask absorption = bearish)
 *
 * Score → Signal Mapping:
 *   - |score| < 20 → Signal::none() (too weak)
 *   - 20 ≤ |score| < 50 → SignalStrength::Weak
 *   - 50 ≤ |score| < 75 → SignalStrength::Medium
 *   - |score| ≥ 75 → SignalStrength::Strong
 *   - score > 0 → Buy, score < 0 → Sell
 *
 * Position Sizing:
 *   - suggested_qty = base_qty * (|score| / 100.0)
 *   - If already in position:
 *     - Signal agrees with position → Signal::none() (hold, no add)
 *     - Signal disagrees with |score| > 60 → Signal::exit()
 *
 * Regime Filter:
 *   - suitable_for_regime(): ALL regimes (adapts automatically)
 *   - High Volatility regime: increase threshold to 30 (from 20)
 */
class MetricsDrivenStrategy : public IStrategy {
public:
    struct Config {
        double score_threshold = 20.0;
        double hvol_threshold = 30.0;
        double base_position_pct = 0.02; // 2% of cash
        double exit_disagreement = 60.0;
        int warmup_ticks = 100;
    };

    explicit MetricsDrivenStrategy(const Config& config) : config_(config), tick_count_(0) {}

    MetricsDrivenStrategy() : config_(), tick_count_(0) {}

    // Override 5-param generate (uses metrics)
    Signal generate(Symbol symbol, const MarketSnapshot& market, const StrategyPosition& position, MarketRegime regime,
                    const MetricsContext* metrics) override;

    // 4-param: no metrics = no signal
    Signal generate(Symbol symbol, const MarketSnapshot& market, const StrategyPosition& position,
                    MarketRegime regime) override {
        (void)symbol;
        (void)market;
        (void)position;
        (void)regime;
        return Signal::none();
    }

    std::string_view name() const override { return "MetricsDriven"; }
    OrderPreference default_order_preference() const override { return OrderPreference::Limit; }
    bool suitable_for_regime(MarketRegime regime) const override {
        (void)regime;
        return true;
    }
    void on_tick(const MarketSnapshot& market) override {
        (void)market;
        tick_count_++;
    }
    void reset() override { tick_count_ = 0; }
    bool ready() const override { return tick_count_ >= config_.warmup_ticks; }

private:
    Config config_;
    int tick_count_;

    double score_trade_flow(const TradeStreamMetrics* trade) const;
    double score_book_pressure(const OrderBookMetrics* book) const;
    double score_flow_toxicity(const OrderFlowMetrics<20>* flow) const;
    double score_futures_sentiment(const FuturesMetrics* futures) const;
    double score_absorption(const CombinedMetrics* combined) const;
};

// ============================================================================
// Implementation (header-only for inline optimization)
// ============================================================================

inline Signal MetricsDrivenStrategy::generate(Symbol symbol, const MarketSnapshot& market,
                                              const StrategyPosition& position, MarketRegime regime,
                                              const MetricsContext* metrics) {
    (void)symbol;

    // No metrics or not ready
    if (!metrics || !metrics->has_any() || !ready()) {
        return Signal::none();
    }

    // Compute multi-factor score
    double score = 0.0;

    // Factor 1: Trade Flow (25%)
    if (metrics->trade) {
        score += score_trade_flow(metrics->trade);
    }

    // Factor 2: Order Book Pressure (20%)
    if (metrics->book) {
        score += score_book_pressure(metrics->book);
    }

    // Factor 3: Order Flow Toxicity (20%)
    if (metrics->flow) {
        score += score_flow_toxicity(metrics->flow);
    }

    // Factor 4: Futures Sentiment (20%)
    if (metrics->futures) {
        score += score_futures_sentiment(metrics->futures);
    }

    // Factor 5: Absorption (15%)
    if (metrics->combined) {
        score += score_absorption(metrics->combined);
    }

    // Apply regime-specific threshold adjustment
    double threshold = config_.score_threshold;
    if (regime == MarketRegime::HighVolatility) {
        threshold = config_.hvol_threshold;
    }

    // Check minimum threshold
    if (std::abs(score) < threshold) {
        return Signal::none();
    }

    // Position management: exit on disagreement, hold on agreement
    if (position.has_position()) {
        bool is_long = position.quantity > 0;
        bool signal_is_buy = score > 0;

        if (is_long == signal_is_buy) {
            // Agreement: hold position, no pyramiding
            return Signal::none();
        } else if (std::abs(score) > config_.exit_disagreement) {
            // Strong disagreement: exit
            return Signal::exit(position.quantity, "Metrics disagree with position");
        } else {
            // Weak disagreement: hold
            return Signal::none();
        }
    }

    // Calculate position size
    double base_qty = position.max_position * config_.base_position_pct;
    double score_pct = std::abs(score) / 100.0;
    double suggested_qty = base_qty * score_pct;

    // Check if we have cash
    if (!position.can_buy()) {
        return Signal::none();
    }

    // Generate signal
    SignalStrength strength;
    if (std::abs(score) >= 75) {
        strength = SignalStrength::Strong;
    } else if (std::abs(score) >= 50) {
        strength = SignalStrength::Medium;
    } else {
        strength = SignalStrength::Weak;
    }

    if (score > 0) {
        return Signal{SignalType::Buy, strength, OrderPreference::Market, suggested_qty, 0, "Multi-factor bullish"};
    } else {
        return Signal{SignalType::Sell, strength, OrderPreference::Market, suggested_qty, 0, "Multi-factor bearish"};
    }
}

inline double MetricsDrivenStrategy::score_trade_flow(const TradeStreamMetrics* trade) const {
    auto m = trade->get_metrics(TradeWindow::W5s);

    double total_volume = m.buy_volume + m.sell_volume;
    if (total_volume < 1e-9) {
        return 0.0;
    }

    double buy_ratio = m.buy_volume / total_volume;

    // Linear interpolation: [0.35, 0.65] → [-25, +25]
    if (buy_ratio > 0.65) {
        return 25.0;
    } else if (buy_ratio < 0.35) {
        return -25.0;
    } else {
        // Interpolate linearly: slope = 25 / 0.15 = 166.67
        return (buy_ratio - 0.5) / 0.15 * 25.0;
    }
}

inline double MetricsDrivenStrategy::score_book_pressure(const OrderBookMetrics* book) const {
    auto m = book->get_metrics();

    double score = 0.0;

    // Top imbalance: [-1, +1] → [-20, +20]
    if (m.top_imbalance > 0.3) {
        score = 20.0 * (m.top_imbalance / 0.3);
        if (score > 20.0)
            score = 20.0;
    } else if (m.top_imbalance < -0.3) {
        score = 20.0 * (m.top_imbalance / 0.3);
        if (score < -20.0)
            score = -20.0;
    } else {
        score = 20.0 * (m.top_imbalance / 0.3);
    }

    // Dampen if spread too wide (illiquid)
    if (m.spread_bps > 20.0) {
        score *= 0.5;
    }

    return score;
}

inline double MetricsDrivenStrategy::score_flow_toxicity(const OrderFlowMetrics<20>* flow) const {
    auto m = flow->get_metrics(Window::SEC_5);

    double score = 0.0;

    // Cancel ratio asymmetry
    double cancel_diff = m.cancel_ratio_bid - m.cancel_ratio_ask;

    // High bid cancels + low ask cancels → fake bids (bearish)
    if (m.cancel_ratio_bid > 0.5 && m.cancel_ratio_ask < 0.2) {
        score -= 20.0;
    }
    // High ask cancels + low bid cancels → fake asks (bullish)
    else if (m.cancel_ratio_ask > 0.5 && m.cancel_ratio_bid < 0.2) {
        score += 20.0;
    }
    // Moderate asymmetry: use diff directly
    else {
        score -= cancel_diff * 20.0; // Negative diff means more bid cancels
    }

    // Volume removed asymmetry
    double vol_diff = m.bid_volume_removed - m.ask_volume_removed;
    if (vol_diff > 0) {
        // More bid volume removed → support eroding (bearish)
        score -= 20.0 * (vol_diff / (m.bid_volume_removed + m.ask_volume_removed + 1e-9));
    } else if (vol_diff < 0) {
        // More ask volume removed → resistance eroding (bullish)
        score += 20.0 * (-vol_diff / (m.bid_volume_removed + m.ask_volume_removed + 1e-9));
    }

    // Clamp to [-20, +20]
    if (score > 20.0)
        score = 20.0;
    if (score < -20.0)
        score = -20.0;

    return score;
}

inline double MetricsDrivenStrategy::score_futures_sentiment(const FuturesMetrics* futures) const {
    auto m = futures->get_metrics(FuturesWindow::W5s);

    double score = 0.0;

    // Contrarian funding rate signals
    if (m.funding_rate_extreme) {
        if (m.funding_rate > 0) {
            // Overleveraged longs → contrarian bearish
            score -= 20.0;
        } else if (m.funding_rate < 0) {
            // Overleveraged shorts → contrarian bullish
            score += 20.0;
        }
    }

    // Liquidation imbalance
    if (m.liquidation_imbalance > 0.5) {
        // Long liquidation cascade → bearish
        score -= 20.0;
    } else if (m.liquidation_imbalance < -0.5) {
        // Short squeeze → bullish
        score += 20.0;
    }

    // Basis uncertainty: reduce conviction
    if (std::abs(m.basis_bps) > 50.0) {
        score *= 0.7; // 30% reduction
    }

    // Clamp to weight limit [-20, +20]
    if (score > 20.0)
        score = 20.0;
    if (score < -20.0)
        score = -20.0;

    return score;
}

inline double MetricsDrivenStrategy::score_absorption(const CombinedMetrics* combined) const {
    auto m = combined->get_metrics(CombinedMetrics::Window::SEC_5);

    double score = 0.0;

    // Bid absorption (bullish)
    if (m.absorption_ratio_bid > 2.0) {
        score += 15.0 * std::min(m.absorption_ratio_bid / 2.0, 1.0);
    }

    // Ask absorption (bearish)
    if (m.absorption_ratio_ask > 2.0) {
        score -= 15.0 * std::min(m.absorption_ratio_ask / 2.0, 1.0);
    }

    return score;
}

} // namespace strategy
} // namespace hft
