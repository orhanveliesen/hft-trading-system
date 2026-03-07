#pragma once

#include "../execution/execution_scorer.hpp"
#include "../execution/limit_manager.hpp"
#include "../strategy/istrategy.hpp"
#include "../strategy/strategy_selector.hpp"
#include "../types.hpp"
#include "event_bus.hpp"
#include "events.hpp"
#include "metrics_manager.hpp"

#include <cmath>

namespace hft::core {

using namespace hft;
using namespace hft::strategy;
using namespace hft::execution;

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
    static constexpr uint64_t DEFAULT_COOLDOWN_NS = 2'000'000'000; // 2000ms

    /**
     * @param bus EventBus for publishing action events
     * @param metrics MetricsManager for getting metrics context
     * @param limit_mgr LimitManager for checking timeouts
     * @param selector StrategySelector for getting active strategy
     * @param cooldown_ns Minimum time between signals per symbol (default 2000ms)
     */
    StrategyEvaluator(EventBus* bus, MetricsManager* metrics, LimitManager* limit_mgr, StrategySelector* selector,
                      uint64_t cooldown_ns = DEFAULT_COOLDOWN_NS)
        : bus_(bus), metrics_(metrics), limit_mgr_(limit_mgr), selector_(selector), cooldown_ns_(cooldown_ns) {
        last_signal_time_.fill(0);
    }

    /**
     * @brief Evaluate strategy and publish events
     *
     * @param symbol Symbol ID
     * @param market Current market snapshot
     * @param position Current position
     */
    void evaluate(Symbol symbol, const MarketSnapshot& market, const StrategyPosition& position) {
        // Check cooldown
        uint64_t now = util::now_ns();
        if (now - last_signal_time_[symbol] < cooldown_ns_) {
            return; // Still in cooldown period
        }

        // Get metrics context
        auto ctx = metrics_->context_for(symbol);

        // Get active strategy
        IStrategy* strategy = nullptr;
        if (ctx.has_regime()) {
            strategy = selector_->select_for_regime(ctx.regime);
        }
        if (!strategy) {
            // No regime-specific strategy → fall back to default
            strategy = selector_->get_default();
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
        uint64_t timestamp_ns = now;
        double qty = signal.suggested_qty; // Use double directly (crypto fractional amounts)

        // Publish event based on execution score
        if (score.prefer_limit()) {
            // Limit order preferred
            Price limit_price = calculate_limit_price(signal, market, side);

            // Check if there's already a pending limit for this symbol
            const auto* pending = limit_mgr_ ? limit_mgr_->get_pending(symbol) : nullptr;
            if (pending) {
                // Decide whether to replace the pending limit
                bool should_replace = should_replace_limit(pending, side, limit_price);

                if (should_replace) {
                    // Publish LimitCancelEvent to cancel the old limit (order_id=0 cancels all for symbol)
                    bus_->publish(LimitCancelEvent{.symbol = symbol,
                                                   .order_id = 0,
                                                   .reason = "replace_with_new_signal",
                                                   .timestamp_ns = timestamp_ns});
                } else {
                    // Keep existing limit, don't place new one
                    return;
                }
            }

            // Publish new limit order event
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

        // Update cooldown timestamp
        last_signal_time_[symbol] = timestamp_ns;
    }

private:
    EventBus* bus_;
    MetricsManager* metrics_;
    LimitManager* limit_mgr_;
    StrategySelector* selector_;

    // Per-symbol cooldown tracking
    std::array<uint64_t, 64> last_signal_time_;
    uint64_t cooldown_ns_;

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
    void check_limit_timeouts() {
        if (limit_mgr_) {
            limit_mgr_->check_timeouts();
        }
    }

    /**
     * @brief Decide whether to replace an existing pending limit order
     *
     * Replace conditions:
     * 1. Different side (buy vs sell) → always replace
     * 2. Same side but price difference > 0.1% → replace (significant price change)
     * 3. Otherwise → keep existing limit
     *
     * @param pending Existing pending limit
     * @param new_side New signal's side
     * @param new_price New signal's limit price
     * @return true if should replace, false if should keep existing
     */
    static bool should_replace_limit(const execution::LimitManager::PendingLimit* pending, Side new_side,
                                     Price new_price) {
        if (!pending)
            return true; // No existing limit, always place new one

        // Condition 1: Different side → always replace
        if (pending->side != new_side)
            return true;

        // Condition 2: Price difference > 0.1% → replace
        Price old_price = pending->limit_price;
        if (old_price == 0)
            return true; // Invalid old price, replace

        double price_diff_pct = std::abs(static_cast<double>(new_price - old_price)) / old_price;
        constexpr double REPLACE_THRESHOLD = 0.001; // 0.1%

        if (price_diff_pct > REPLACE_THRESHOLD)
            return true;

        // Condition 3: Otherwise, keep existing limit
        return false;
    }

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

} // namespace hft::core
