#pragma once

#include "../execution/execution_scorer.hpp"
#include "../strategy/istrategy.hpp"
#include "../strategy/strategy_selector.hpp"
#include "../types.hpp"
#include "event_bus.hpp"
#include "events.hpp"
#include "metrics_manager.hpp"

#include <cmath>

namespace core {

using namespace hft;
using namespace hft::strategy;
using namespace hft::execution;

// Forward declaration
class LimitManager;

/**
 * @brief Evaluates strategy signals and publishes action events
 *
 * Responsibilities:
 * 1. Get MetricsContext from MetricsManager
 * 2. Call strategy.generate()
 * 3. Compute execution score
 * 4. Publish Market or Limit event based on score
 * 5. Check pending limits for timeout if no signal
 *
 * Flow:
 *   Signal → ExecutionScorer → Event (Market/Limit) → EventBus → Execution
 */
class StrategyEvaluator {
public:
    /**
     * @param bus EventBus for publishing action events
     * @param metrics MetricsManager for getting metrics context
     * @param limit_mgr LimitManager for checking timeouts
     * @param selector StrategySelector for getting active strategy
     */
    StrategyEvaluator(EventBus* bus, MetricsManager* metrics, LimitManager* limit_mgr, StrategySelector* selector)
        : bus_(bus), metrics_(metrics), limit_mgr_(limit_mgr), selector_(selector) {}

    /**
     * @brief Evaluate strategy and publish events
     *
     * @param symbol Symbol ID
     * @param market Current market snapshot
     * @param position Current position
     */
    void evaluate(Symbol symbol, const MarketSnapshot& market, const StrategyPosition& position) {
        // Get metrics context
        auto ctx = metrics_->context_for(symbol);

        // Get active strategy
        IStrategy* strategy = nullptr;
        if (ctx.has_regime()) {
            strategy = selector_->select_for_regime(ctx.regime);
        }
        if (!strategy) {
            return; // No strategy available
        }

        // Generate signal
        Signal signal = strategy->generate(symbol, market, position, ctx.regime, &ctx);

        if (!signal.is_actionable()) {
            // No action → check pending limits for timeout
            check_limit_timeouts();
            return;
        }

        // Check if regime is dangerous
        if (is_dangerous_regime(ctx.regime)) {
            // Don't trade in dangerous regimes
            return;
        }

        // Determine side
        Side side = signal.is_buy() ? Side::Buy : Side::Sell;

        // Compute execution score
        auto score = ExecutionScorer::compute(signal, &ctx, side);

        // Convert signal strength to double [0.0, 1.0]
        double strength_val = signal_strength_to_double(signal.strength);

        // Get timestamp
        uint64_t timestamp_ns = util::now_ns();

        // Publish event based on execution score
        if (score.prefer_limit()) {
            // Limit order preferred
            Price limit_price = calculate_limit_price(signal, market, side);
            Quantity qty = static_cast<Quantity>(signal.suggested_qty);

            if (signal.is_buy()) {
                bus_->publish(SpotLimitBuyEvent{.symbol = symbol,
                                                .qty = qty,
                                                .limit_price = limit_price,
                                                .strength = strength_val,
                                                .exec_score = score.score,
                                                .reason = signal.reason,
                                                .timestamp_ns = timestamp_ns});
            } else {
                bus_->publish(SpotLimitSellEvent{.symbol = symbol,
                                                 .qty = qty,
                                                 .limit_price = limit_price,
                                                 .strength = strength_val,
                                                 .exec_score = score.score,
                                                 .reason = signal.reason,
                                                 .timestamp_ns = timestamp_ns});
            }
        } else {
            // Market order preferred
            Quantity qty = static_cast<Quantity>(signal.suggested_qty);

            if (signal.is_buy()) {
                bus_->publish(SpotBuyEvent{.symbol = symbol,
                                           .qty = qty,
                                           .strength = strength_val,
                                           .reason = signal.reason,
                                           .timestamp_ns = timestamp_ns});
            } else {
                bus_->publish(SpotSellEvent{.symbol = symbol,
                                            .qty = qty,
                                            .strength = strength_val,
                                            .reason = signal.reason,
                                            .timestamp_ns = timestamp_ns});
            }
        }
    }

private:
    EventBus* bus_;
    MetricsManager* metrics_;
    LimitManager* limit_mgr_;
    StrategySelector* selector_;

    /**
     * @brief Convert SignalStrength to double
     */
    static double signal_strength_to_double(SignalStrength strength) {
        switch (strength) {
        case SignalStrength::Weak:
            return 0.33;
        case SignalStrength::Medium:
            return 0.66;
        case SignalStrength::Strong:
            return 1.0;
        default:        // LCOV_EXCL_LINE
            return 0.0; // LCOV_EXCL_LINE
        }               // LCOV_EXCL_LINE
    }

    /**
     * @brief Calculate limit price from signal and market
     */
    static Price calculate_limit_price(const Signal& signal, const MarketSnapshot& market, Side side) {
        // If signal provides limit price, use it
        if (signal.limit_price > 0) {
            return signal.limit_price;
        }

        // Otherwise, use best bid/ask
        if (side == Side::Buy) {
            return market.bid; // Passive buy at bid
        } else {
            return market.ask; // Passive sell at ask
        }
    }

    /**
     * @brief Check if regime is too dangerous to trade
     */
    static bool is_dangerous_regime(MarketRegime regime) {
        return regime == MarketRegime::Spike || regime == MarketRegime::HighVolatility;
    }

    /**
     * @brief Check pending limits for timeout (forward to LimitManager)
     */
    void check_limit_timeouts();

    /**
     * @brief Get current timestamp in nanoseconds
     */
    struct util {
        static uint64_t now_ns() {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }
    };
};

} // namespace core
