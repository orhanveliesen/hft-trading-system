#pragma once

#include "../strategy/istrategy.hpp"
#include "../strategy/metrics_context.hpp"
#include "../types.hpp"

#include <algorithm>
#include <cmath>

namespace hft {
namespace execution {

/// Result of execution scoring computation
struct ExecScoreResult {
    double score = 0.0;        // [-50, +50] total score
    double spread_value = 0.0; // [-15, +15] spread factor
    double fill_prob = 0.0;    // [-12, +12] fill probability factor
    double adverse = 0.0;      // [-12, +12] adverse selection factor
    double urgency = 0.0;      // [-10, +10] signal urgency factor

    /// Returns true if limit order is preferred (score > 0)
    bool prefer_limit() const { return score > 0.0; }
};

/// Execution scorer: decides Market vs Limit using 4-factor system
///
/// Score range: [-50, +50]
///   score > 0  → Limit preferred (higher = more patient)
///   score ≤ 0  → Market preferred (lower = more urgent)
///
/// Factors:
///   1. Spread Value (30%, [-15, +15])  - Is limit worth the wait?
///   2. Fill Probability (25%, [-12, +12]) - Will limit get filled?
///   3. Adverse Selection (25%, [-12, +12]) - Are we being run over?
///   4. Signal Urgency (20%, [-10, +10])   - How urgent is the signal?
class ExecutionScorer {
public:
    /// Compute execution score for given signal and metrics
    ///
    /// @param signal Strategy signal (strength, direction)
    /// @param metrics Real-time market metrics (nullable)
    /// @param order_side Side of the order to execute
    /// @return ExecScoreResult with total score and factor breakdown
    static ExecScoreResult compute(const strategy::Signal& signal, const strategy::MetricsContext* metrics,
                                   Side order_side) {
        ExecScoreResult result;

        if (!metrics) {
            // No metrics → default to Market (safe fallback)
            result.urgency = score_signal_urgency(signal);
            result.score = result.urgency;
            return result;
        }

        result.spread_value = score_spread_value(metrics);
        result.fill_prob = score_fill_probability(metrics);
        result.adverse = score_adverse_selection(metrics, order_side);
        result.urgency = score_signal_urgency(signal);

        result.score = result.spread_value + result.fill_prob + result.adverse + result.urgency;

        // Clamp to [-50, +50]
        result.score = std::clamp(result.score, -50.0, 50.0);

        return result;
    }

private:
    /// Factor 1: Spread Value (weight: 30%, range [-15, +15])
    ///
    /// Logic:
    ///   spread < 3 bps   → -15 (Market cheap, limit not worth it)
    ///   spread 3-8 bps   → linear [-15, 0]
    ///   spread 8-20 bps  → linear [0, +15]
    ///   spread > 20 bps  → +15 (Limit saves significant cost)
    static double score_spread_value(const strategy::MetricsContext* metrics) {
        if (!metrics || !metrics->book) {
            return 0.0;
        }

        double spread_bps = metrics->book->get_metrics().spread_bps;

        if (spread_bps < 3.0) {
            return -15.0;
        } else if (spread_bps < 8.0) {
            return lerp_score(spread_bps, 3.0, 8.0, -15.0, 0.0);
        } else if (spread_bps < 20.0) {
            return lerp_score(spread_bps, 8.0, 20.0, 0.0, 15.0);
        } else {
            return 15.0;
        }
    }

    /// Factor 2: Fill Probability (weight: 25%, range [-12, +12])
    ///
    /// Sources:
    ///   - trade_to_depth_ratio (SEC_5) - high = active market
    ///   - cancel_ratio_bid/ask (SEC_5) - high = unreliable book
    ///   - burst_count (W5s) - bursts = sudden activity (unused, implicit in
    ///   trade_to_depth)
    ///   - short_lived ratios (SEC_5) - high = fake liquidity
    ///
    /// Logic:
    ///   trade_to_depth < 0.5 → +6 (calm, limit sits)
    ///   trade_to_depth > 2.0 → -6 (active, limit risky)
    ///
    ///   avg_cancel_ratio < 0.2 → +6 (stable book)
    ///   avg_cancel_ratio > 0.6 → -6 (unreliable book)
    static double score_fill_probability(const strategy::MetricsContext* metrics) {
        if (!metrics || !metrics->combined || !metrics->flow) {
            return 0.0;
        }

        double score = 0.0;

        // Component 1: Trade-to-depth ratio
        double trade_to_depth = metrics->combined->get_metrics(CombinedMetrics::Window::SEC_5).trade_to_depth_ratio;
        if (trade_to_depth < 0.5) {
            score += 6.0;
        } else if (trade_to_depth > 2.0) {
            score -= 6.0;
        } else {
            score += lerp_score(trade_to_depth, 0.5, 2.0, 6.0, -6.0);
        }

        // Component 2: Cancel ratio (average of bid and ask)
        auto flow_metrics = metrics->flow->get_metrics(Window::SEC_5);
        double cancel_bid = flow_metrics.cancel_ratio_bid;
        double cancel_ask = flow_metrics.cancel_ratio_ask;
        double avg_cancel = (cancel_bid + cancel_ask) / 2.0;

        if (avg_cancel < 0.2) {
            score += 6.0;
        } else if (avg_cancel > 0.6) {
            score -= 6.0;
        } else {
            score += lerp_score(avg_cancel, 0.2, 0.6, 6.0, -6.0);
        }

        return std::clamp(score, -12.0, 12.0);
    }

    /// Factor 3: Adverse Selection (weight: 25%, range [-12, +12])
    ///
    /// Sources:
    ///   - liquidation_imbalance (W5s) - cascade = informed flow
    ///   - absorption_ratio_bid/ask (SEC_5) - aggressive taking
    ///   - bid/ask_volume_removed (SEC_5) - depth pulled
    ///   - large_trades (W5s) - institutional flow (unused, implicit in
    ///   absorption)
    ///
    /// Logic:
    ///   |liquidation_imbalance| < 0.2 → +4 (calm)
    ///   |liquidation_imbalance| > 0.6 → -4 (cascade)
    ///
    ///   max(absorption_ratio_bid, absorption_ratio_ask) < 1.5 → +4
    ///   max(absorption_ratio_bid, absorption_ratio_ask) > 3.0 → -4
    ///
    ///   For Buy:  bid_volume_removed high → -4 (support eroding)
    ///   For Sell: ask_volume_removed high → -4 (resistance eroding)
    static double score_adverse_selection(const strategy::MetricsContext* metrics, Side order_side) {
        if (!metrics || !metrics->futures || !metrics->combined || !metrics->flow) {
            return 0.0;
        }

        double score = 0.0;

        // Component 1: Liquidation imbalance
        double liq_imbalance = metrics->futures->get_metrics(FuturesWindow::W5s).liquidation_imbalance;
        double abs_liq = std::abs(liq_imbalance);
        if (abs_liq < 0.2) {
            score += 4.0;
        } else if (abs_liq > 0.6) {
            score -= 4.0;
        } else {
            score += lerp_score(abs_liq, 0.2, 0.6, 4.0, -4.0);
        }

        // Component 2: Absorption ratio
        auto combined_metrics = metrics->combined->get_metrics(CombinedMetrics::Window::SEC_5);
        double abs_bid = combined_metrics.absorption_ratio_bid;
        double abs_ask = combined_metrics.absorption_ratio_ask;
        double max_absorption = std::max(abs_bid, abs_ask);

        if (max_absorption < 1.5) {
            score += 4.0;
        } else if (max_absorption > 3.0) {
            score -= 4.0;
        } else {
            score += lerp_score(max_absorption, 1.5, 3.0, 4.0, -4.0);
        }

        // Component 3: Volume removed (direction-dependent)
        auto flow_metrics = metrics->flow->get_metrics(Window::SEC_5);
        double vol_removed = 0.0;
        if (order_side == Side::Buy) {
            vol_removed = flow_metrics.bid_volume_removed;
        } else {
            vol_removed = flow_metrics.ask_volume_removed;
        }

        // High volume removed = depth eroding → prefer market
        if (vol_removed > 100000.0) {
            score -= 4.0;
        } else if (vol_removed < 10000.0) {
            score += 4.0;
        } else {
            score += lerp_score(vol_removed, 10000.0, 100000.0, 4.0, -4.0);
        }

        return std::clamp(score, -12.0, 12.0);
    }

    /// Factor 4: Signal Urgency (weight: 20%, range [-10, +10])
    ///
    /// Logic:
    ///   Strong  → -10 (urgent, market)
    ///   Medium  → 0
    ///   Weak    → +10 (patient, limit)
    static double score_signal_urgency(const strategy::Signal& signal) {
        switch (signal.strength) {
        case strategy::SignalStrength::Strong:
            return -10.0;
        case strategy::SignalStrength::Medium:
            return 0.0;
        case strategy::SignalStrength::Weak:
            return 10.0;
        default:
            return 0.0;
        }
    }

    /// Linear interpolation with clamping
    ///
    /// Maps value in [low, high] to score in [score_low, score_high]
    /// Clamps value to [low, high] before interpolation
    static double lerp_score(double value, double low, double high, double score_low, double score_high) {
        value = std::clamp(value, low, high);
        double t = (value - low) / (high - low);
        return score_low + t * (score_high - score_low);
    }
};

} // namespace execution
} // namespace hft
