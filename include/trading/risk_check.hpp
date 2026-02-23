#pragma once

/**
 * Risk Check Module - Hot Path Risk Validation
 *
 * Provides branchless-optimized risk checking functions for the hot path.
 * All functions operate on TradingState SoA data structures.
 *
 * Functions:
 * - check_risk(): Pre-trade validation (hot path)
 * - update_risk_on_fill(): Post-fill risk state update
 * - calculate_drawdown(): Drawdown calculation with peak update
 * - check_drawdown_halt(): Check if drawdown threshold exceeded
 *
 * Performance:
 * - check_risk: < 20ns target
 * - update_risk_on_fill: < 50ns target
 */

#include "../types.hpp"
#include "trading_state.hpp"

#include <cmath>
#include <cstdint>

namespace hft {
namespace trading {

/**
 * Side enum for buy/sell (matches types.hpp)
 */
enum class Side : uint8_t { Buy = 0, Sell = 1 };

/**
 * Hot path risk check - validates if an order can be placed.
 *
 * Checks:
 * 1. Global halt status
 * 2. Risk halt status
 * 3. Per-symbol position limit
 * 4. Per-symbol notional limit
 *
 * @param sym Symbol index
 * @param side Buy or Sell
 * @param qty Quantity to trade
 * @param price Current price
 * @param state Reference to TradingState
 * @return true if order passes all risk checks
 */
__attribute__((always_inline)) inline bool check_risk(size_t sym, Side side, double qty, double price,
                                                      const TradingState& state) {
    // 1. Global halt check
    if (state.halt.halted.load(std::memory_order_relaxed) != static_cast<uint8_t>(HaltStatus::RUNNING)) [[unlikely]] {
        return false;
    }

    // 2. Risk halt check
    if (state.risk_state.risk_halted.load(std::memory_order_relaxed) != 0) [[unlikely]] {
        return false;
    }

    // 3. Per-symbol position limit (if set)
    int64_t max_pos = state.risk_limits.max_position[sym];
    if (max_pos > 0) {
        double current = state.positions.quantity[sym];
        double new_pos;
        if (side == Side::Buy) {
            new_pos = current + qty;
        } else {
            new_pos = current - qty;
        }

        if (std::abs(new_pos) > static_cast<double>(max_pos)) [[unlikely]] {
            return false;
        }
    }

    // 4. Per-symbol notional limit (if set)
    int64_t max_notional = state.risk_limits.max_notional[sym];
    if (max_notional > 0 && price > 0) {
        int64_t order_notional = static_cast<int64_t>(qty * price * FIXED_POINT_SCALE);
        int64_t current = state.risk_limits.current_notional[sym];

        // Only check for buys - sells reduce notional
        if (side == Side::Buy) {
            if (current + order_notional > max_notional) [[unlikely]] {
                return false;
            }
        }
    }

    return true;
}

/**
 * Trigger halt with reason.
 * Internal helper function.
 */
inline void trigger_halt_internal(TradingState& state, HaltReason reason) {
    uint8_t expected = static_cast<uint8_t>(HaltStatus::RUNNING);
    uint8_t halting = static_cast<uint8_t>(HaltStatus::HALTING);

    if (state.halt.halted.compare_exchange_strong(expected, halting, std::memory_order_acq_rel)) {
        state.halt.reason.store(static_cast<uint8_t>(reason), std::memory_order_release);
    }
}

/**
 * Update risk state after a fill.
 *
 * @param sym Symbol index
 * @param side Buy or Sell
 * @param qty Filled quantity
 * @param price Fill price
 * @param realized_pnl Realized P&L from this fill (can be 0)
 * @param state Reference to TradingState (mutable)
 */
inline void update_risk_on_fill(size_t sym, Side side, double qty, double price, double realized_pnl,
                                TradingState& state) {
    // Update notional
    int64_t order_notional = static_cast<int64_t>(qty * price * FIXED_POINT_SCALE);

    if (side == Side::Buy) {
        state.risk_limits.current_notional[sym] += order_notional;
    } else {
        state.risk_limits.current_notional[sym] -= order_notional;
    }

    // Update daily P&L if realized
    if (realized_pnl != 0.0) {
        int64_t pnl_x8 = static_cast<int64_t>(realized_pnl * FIXED_POINT_SCALE);
        state.risk_state.daily_pnl_x8.fetch_add(pnl_x8, std::memory_order_relaxed);

        // Check daily loss limit
        int64_t daily_pnl = state.risk_state.daily_pnl_x8.load(std::memory_order_relaxed);
        int64_t limit = state.risk_state.daily_loss_limit_x8.load(std::memory_order_relaxed);

        // Trigger halt if daily loss exceeds limit
        if (limit > 0 && daily_pnl < -limit) {
            state.risk_state.risk_halted.store(1, std::memory_order_release);
            trigger_halt_internal(state, HaltReason::RISK_LIMIT);
        }
    }
}

/**
 * Calculate current drawdown percentage.
 * Updates peak equity if current equity is higher.
 *
 * @param current_equity_x8 Current equity in fixed point
 * @param state Reference to TradingState (mutable for peak update)
 * @return Drawdown as decimal (0.10 = 10% drawdown)
 */
inline double calculate_drawdown(int64_t current_equity_x8, TradingState& state) {
    int64_t peak = state.risk_state.peak_equity_x8.load(std::memory_order_relaxed);

    // Update peak if we have a new high
    if (current_equity_x8 > peak) {
        state.risk_state.peak_equity_x8.store(current_equity_x8, std::memory_order_relaxed);
        return 0.0; // No drawdown at new peak
    }

    // Calculate drawdown
    if (peak <= 0)
        return 0.0;

    double drawdown = static_cast<double>(peak - current_equity_x8) / static_cast<double>(peak);
    return drawdown;
}

/**
 * Check if drawdown exceeds threshold and trigger halt if needed.
 *
 * @param current_equity_x8 Current equity in fixed point
 * @param state Reference to TradingState (mutable)
 * @return true if halt was triggered
 */
inline bool check_drawdown_halt(int64_t current_equity_x8, TradingState& state) {
    double max_dd = state.risk_state.max_drawdown_pct.load(std::memory_order_relaxed);

    // No limit set
    if (max_dd <= 0.0)
        return false;

    double current_dd = calculate_drawdown(current_equity_x8, state);

    if (current_dd > max_dd) {
        state.risk_state.risk_halted.store(1, std::memory_order_release);
        trigger_halt_internal(state, HaltReason::RISK_LIMIT);
        return true;
    }

    return false;
}

/**
 * Reset daily risk counters.
 * Call at start of new trading day.
 */
inline void reset_daily_risk(TradingState& state) {
    state.risk_state.daily_pnl_x8.store(0, std::memory_order_relaxed);

    // Only reset halt if not due to drawdown
    // (drawdown persists across days until recovered)
}

/**
 * Reset all risk state.
 * Use with caution - typically only at system restart.
 */
inline void reset_all_risk(TradingState& state) {
    state.risk_state.daily_pnl_x8.store(0, std::memory_order_relaxed);
    state.risk_state.risk_halted.store(0, std::memory_order_relaxed);

    // Reset notional for all symbols
    for (size_t i = 0; i < MAX_SYMBOLS; ++i) {
        state.risk_limits.current_notional[i] = 0;
    }
}

} // namespace trading
} // namespace hft
