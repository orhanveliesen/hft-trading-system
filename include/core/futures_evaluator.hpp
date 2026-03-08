#pragma once

#include "../execution/funding_scheduler.hpp"
#include "../execution/futures_position.hpp"
#include "../strategy/istrategy.hpp"
#include "../util/time_utils.hpp"
#include "event_bus.hpp"
#include "events.hpp"
#include "metrics_manager.hpp"

#include <array>
#include <cmath>

namespace hft {
namespace core {

using namespace hft::execution;
using namespace hft::strategy;

/**
 * @brief Evaluates futures trading modes and publishes action events
 *
 * Three trading modes:
 * 1. Hedge (Cash-and-Carry): Spot long + futures short to lock basis premium
 * 2. Directional (Leveraged): Amplify strong signals with leverage (STUB - Phase 5.2)
 * 3. Funding Farming: Contrarian positions to capture funding payments
 *
 * Architecture Pattern (mirrors StrategyEvaluator):
 * - MetricsManager change_callback → FuturesEvaluator::evaluate()
 * - evaluate() → mode evaluation methods
 * - mode methods → EventBus.publish(FuturesBuyEvent/FuturesSellEvent/Close...)
 * - EventBus subscribers → FuturesEngine.execute()
 *
 * HFT Performance:
 * - Single syscall per evaluate(): wall_clock_ns() called ONCE, passed to all sub-methods
 * - No heap allocation: All stack-allocated
 * - Per-symbol cooldown: 5s between evaluations
 *
 * Design:
 * - Header-only (inline implementations)
 * - Static thresholds (can be made configurable later)
 * - Cooldown to prevent excessive event spam
 */
class FuturesEvaluator {
public:
    // Thresholds for each mode
    static constexpr double HEDGE_BASIS_THRESHOLD = 20.0;       ///< 20 bps minimum basis for hedge
    static constexpr double FARMING_FUNDING_THRESHOLD = 0.0005; ///< 0.05% funding rate threshold
    static constexpr uint64_t MODE_COOLDOWN_NS = 5'000'000'000; ///< 5 seconds between evaluations

    /**
     * @brief Constructor
     *
     * @param bus EventBus for publishing futures events
     * @param metrics MetricsManager for futures metrics
     * @param positions FuturesPosition tracker
     */
    FuturesEvaluator(EventBus* bus, MetricsManager* metrics, FuturesPosition* positions)
        : bus_(bus), metrics_(metrics), positions_(positions) {
        last_eval_time_.fill(0);
    }

    /**
     * @brief Evaluate futures modes and publish events
     *
     * Called from MetricsManager change_callback (same pattern as StrategyEvaluator).
     *
     * Flow:
     * 1. Check cooldown
     * 2. Get futures metrics from MetricsManager
     * 3. Call wall_clock_ns() ONCE (single syscall)
     * 4. Pass now_ns to all mode evaluation methods
     * 5. Update last_eval_time
     *
     * @param symbol Symbol ID
     * @param market Current market snapshot
     * @param spot_position Current spot position (for hedge mode)
     */
    void evaluate(Symbol symbol, const MarketSnapshot& market, const StrategyPosition& spot_position) {
        // CRITICAL: Call wall_clock_ns() ONCE at start (single syscall per evaluate)
        // Pass now_ns to all sub-methods to avoid multiple syscalls
        uint64_t now_ns = util::wall_clock_ns();

        // Check cooldown
        if (now_ns - last_eval_time_[symbol] < MODE_COOLDOWN_NS)
            return;

        // Get futures metrics
        auto fm_ptr = metrics_->futures(symbol);
        if (!fm_ptr)
            return; // No futures data yet

        auto fm = fm_ptr->get_metrics(FuturesWindow::W1s); // Use 1s window

        // CRITICAL: Check exit conditions FIRST (before opening new positions)
        evaluate_exits(symbol, market, spot_position, fm, now_ns);

        // Evaluate all modes (pass now_ns to avoid syscalls in each mode)
        evaluate_hedge_mode(symbol, market, spot_position, fm, now_ns);
        evaluate_directional_mode(symbol, market, fm, now_ns); // STUB - Phase 5.2
        evaluate_funding_farming_mode(symbol, market, fm, now_ns);

        last_eval_time_[symbol] = now_ns;
    }

private:
    EventBus* bus_;
    MetricsManager* metrics_;
    FuturesPosition* positions_;

    std::array<uint64_t, 64> last_eval_time_; // Per-symbol cooldown

    /**
     * @brief Mode 1: Hedge (Cash-and-Carry)
     *
     * Strategy:
     * - If spot position exists AND basis > threshold
     * - Short futures to lock basis premium
     * - Exit when basis shrinks or spot position closed
     *
     * Logic:
     * 1. Check if spot position exists
     * 2. Check if basis is profitable (> 20 bps)
     * 3. Check if already hedged
     * 4. Publish FuturesSellEvent to short futures
     * 5. Track position in positions_
     *
     * @param now_ns Wall-clock time (passed to avoid syscall)
     */
    void evaluate_hedge_mode(Symbol symbol, const MarketSnapshot& market, const StrategyPosition& spot_position,
                             const FuturesMetrics::Metrics& fm, uint64_t now_ns) {
        // Check if we have spot position to hedge
        if (!spot_position.has_position())
            return;

        // Check if basis is profitable (positive = futures trading at premium)
        if (fm.basis_bps < HEDGE_BASIS_THRESHOLD)
            return; // Not profitable

        // Check if already hedged
        auto existing = positions_->get_position(symbol, PositionSource::Hedge, Side::Sell);
        if (existing && existing->quantity > 0)
            return; // Already hedged

        // Publish FuturesSellEvent to short futures and lock basis premium
        double qty = spot_position.quantity; // Match spot position size
        bus_->publish(FuturesSellEvent{.symbol = symbol,
                                       .qty = qty,
                                       .strength = 1.0, // Always max strength for hedge
                                       .reason = "hedge_cash_carry",
                                       .timestamp_ns = now_ns});

        // Track position (use same now_ns - all timestamps consistent)
        positions_->open_position(symbol, PositionSource::Hedge, Side::Sell, qty, market.ask, now_ns);
    }

    /**
     * @brief Mode 2: Directional (Leveraged) - STUB
     *
     * Phase 5.2 will implement signal sharing from StrategyEvaluator.
     *
     * Future logic:
     * - Get strategy signal from StrategyEvaluator
     * - If signal.strength == Strong and spot position limited by capital
     * - Open leveraged futures position (2x-5x) in signal direction
     * - Exit when signal reverses or stop loss hit
     *
     * @param now_ns Wall-clock time (passed to avoid syscall)
     */
    void evaluate_directional_mode(Symbol symbol, const MarketSnapshot& market, const FuturesMetrics::Metrics& fm,
                                   uint64_t now_ns) {
        (void)symbol;
        (void)market;
        (void)fm;
        (void)now_ns;

        // STUB: Phase 5.2 will implement signal sharing from StrategyEvaluator
        // For now, directional mode is disabled
    }

    /**
     * @brief Mode 3: Funding Farming
     *
     * Strategy:
     * - If funding rate extreme (> 0.05%)
     * - Take contrarian position: positive funding → short, negative funding → long
     * - Hold until funding reverses or PostFunding phase
     * - Capture funding payments
     *
     * Logic:
     * 1. Get funding phase from FundingScheduler
     * 2. Check if can enter new position
     * 3. Check if funding rate extreme enough
     * 4. Determine contrarian side
     * 5. Check if already farming in this direction
     * 6. Publish FuturesBuyEvent or FuturesSellEvent
     * 7. Track position
     * 8. Check existing positions for exit conditions
     *
     * @param now_ns Wall-clock time (passed to avoid syscall)
     */
    void evaluate_funding_farming_mode(Symbol symbol, const MarketSnapshot& market, const FuturesMetrics::Metrics& fm,
                                       uint64_t now_ns) {
        // Get current funding phase (use passed now_ns, no syscall)
        FundingPhase phase = FundingScheduler::get_phase(fm.next_funding_time_ms, now_ns);

        // Check if can enter new farming position
        if (!FundingScheduler::can_enter_farming(phase))
            return;

        // Check if funding rate is extreme enough to farm
        if (std::abs(fm.funding_rate) < FARMING_FUNDING_THRESHOLD)
            return;

        // Determine contrarian side: positive funding → short, negative funding → long
        Side farm_side = (fm.funding_rate > 0) ? Side::Sell : Side::Buy;

        // Check if already farming in this direction
        auto existing = positions_->get_position(symbol, PositionSource::Farming, farm_side);
        if (existing && existing->quantity > 0)
            return; // Already farming

        // Publish event to open contrarian position
        double qty = 1.0; // Fixed size for now (can be dynamic based on risk)

        if (farm_side == Side::Buy) {
            bus_->publish(FuturesBuyEvent{.symbol = symbol,
                                          .qty = qty,
                                          .strength = 1.0,
                                          .reason = "funding_farming_negative",
                                          .timestamp_ns = now_ns});
        } else {
            bus_->publish(FuturesSellEvent{.symbol = symbol,
                                           .qty = qty,
                                           .strength = 1.0,
                                           .reason = "funding_farming_positive",
                                           .timestamp_ns = now_ns});
        }

        // Track position (use same now_ns - all timestamps consistent)
        Price entry_price = (farm_side == Side::Buy) ? market.ask : market.bid;
        positions_->open_position(symbol, PositionSource::Farming, farm_side, qty, entry_price, now_ns);
    }

    /**
     * @brief Evaluate exit conditions for all active positions
     *
     * Called BEFORE entry logic to ensure positions are closed when conditions met.
     *
     * Exit conditions:
     * - Hedge: Basis turned negative (backwardation) - no longer profitable
     * - Farming: PostFunding phase (safe to exit) OR funding rate reversed (crossed zero)
     * - Directional: Not implemented (stub)
     *
     * @param now_ns Wall-clock time (passed to avoid syscall)
     */
    void evaluate_exits(Symbol symbol, const MarketSnapshot& market, const StrategyPosition& spot_position,
                        const FuturesMetrics::Metrics& fm, uint64_t now_ns) {
        (void)market;        // Reserved for future price checks
        (void)spot_position; // Reserved for hedge exit when spot closed

        // Get current funding phase
        FundingPhase phase = FundingScheduler::get_phase(fm.next_funding_time_ms, now_ns);

        // Check all 4 position slots for this symbol
        for (size_t slot = 0; slot < FuturesPosition::MAX_POSITIONS_PER_SYMBOL; ++slot) {
            const auto* pos_ptr = get_position_by_slot(symbol, slot);
            if (!pos_ptr || !pos_ptr->is_active())
                continue;

            const auto& pos = *pos_ptr;
            bool should_exit = false;
            const char* exit_reason = nullptr;

            // Hedge exit logic
            if (pos.source == PositionSource::Hedge) {
                // Exit if basis turned negative (backwardation = futures cheaper than spot)
                if (fm.basis_bps < -HEDGE_BASIS_THRESHOLD) {
                    should_exit = true;
                    exit_reason = "hedge_backwardation";
                }
                // Also exit if spot position no longer exists
                else if (!spot_position.has_position()) {
                    should_exit = true;
                    exit_reason = "hedge_spot_closed";
                }
            }

            // Farming exit logic
            else if (pos.source == PositionSource::Farming) {
                // Exit during PostFunding phase (safe exit window)
                if (phase == FundingPhase::PostFunding) {
                    should_exit = true;
                    exit_reason = "farming_postfunding";
                }
                // Exit if funding rate reversed (crossed zero)
                else if ((pos.side == Side::Buy && fm.funding_rate > 0) ||
                         (pos.side == Side::Sell && fm.funding_rate < 0)) {
                    should_exit = true;
                    exit_reason = "farming_reversed";
                }
            }

            // Directional exit logic (STUB - Phase 5.2)
            // Will implement when signal sharing added

            // Execute exit
            if (should_exit) {
                if (pos.side == Side::Buy) {
                    // Close long = Market sell
                    bus_->publish(FuturesCloseLongEvent{
                        .symbol = symbol, .qty = pos.quantity, .reason = exit_reason, .timestamp_ns = now_ns});
                } else {
                    // Close short = Market buy
                    bus_->publish(FuturesCloseShortEvent{
                        .symbol = symbol, .qty = pos.quantity, .reason = exit_reason, .timestamp_ns = now_ns});
                }

                // Remove from position tracker
                positions_->close_position(symbol, static_cast<int>(slot));
            }
        }
    }

    /**
     * @brief Get position by slot index (helper for exit logic)
     */
    const FuturesPositionEntry* get_position_by_slot(Symbol symbol, size_t slot) const {
        return positions_->get_by_slot(symbol, slot);
    }
};

} // namespace core
} // namespace hft
