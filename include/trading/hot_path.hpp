#pragma once

/**
 * Hot Path Module - Price Processing Pipeline
 *
 * This is the critical path for price updates. Every function here must be:
 * - Allocation-free (no new/malloc)
 * - Branch-optimized (branchless where possible)
 * - Cache-friendly (sequential SoA access)
 * - Inline for maximum optimization
 *
 * Flow:
 * 1. Check global halt
 * 2. Update current price
 * 3. Check tuner signals (priority)
 * 4. Check symbol flags (exit/pause)
 * 5. Check stop/target
 * 6. Strategy scoring -> signal generation
 *
 * Performance target: < 100ns per symbol
 */

#include "../strategy/scorers.hpp"
#include "halt.hpp"
#include "risk_check.hpp"
#include "trading_state.hpp"

#include <algorithm>
#include <cmath>

namespace hft {
namespace trading {

// =============================================================================
// Constants
// =============================================================================

/// Score threshold for generating buy/sell signals
static constexpr double SCORE_THRESHOLD = 0.3;

// =============================================================================
// Result Types
// =============================================================================

enum class HotPathExitReason : uint8_t { NONE = 0, STOP = 1, TARGET = 2, FLAG = 3, SIGNAL = 4 };

struct ExitResult {
    bool should_exit;
    HotPathExitReason reason;
};

enum class TunerActionType : uint8_t { NONE = 0, BUY = 1, SELL = 2 };

struct TunerAction {
    TunerActionType action;
    double quantity;
};

enum class FlagActionType : uint8_t {
    CONTINUE = 0, // Normal processing
    SKIP = 1,     // Skip this symbol (paused)
    EXIT = 2      // Exit position requested
};

struct FlagAction {
    FlagActionType action;
};

enum class SignalAction : uint8_t { HOLD = 0, BUY = 1, SELL = 2 };

struct TradeSignal {
    SignalAction action;
    double quantity;
};

// =============================================================================
// Position Sizing
// =============================================================================

/**
 * Calculate position size based on config and risk limits.
 *
 * @param sym Symbol index
 * @param price Current price
 * @param state Reference to TradingState
 * @return Position size in units
 */
__attribute__((always_inline)) inline double calculate_position_size(size_t sym, double price,
                                                                     const TradingState& state) {
    if (price <= 0.0) [[unlikely]] {
        return 0.0;
    }

    // Get available cash
    int64_t cash_x8 = state.cash_x8.load(std::memory_order_relaxed);
    double cash = static_cast<double>(cash_x8) / FIXED_POINT_SCALE;

    if (cash <= 0.0) [[unlikely]] {
        return 0.0;
    }

    // Calculate base size from position_size_pct
    double pct = state.common.position_size_pct[sym];
    double notional = cash * pct;

    // Check against max_position limit
    int64_t max_pos = state.risk_limits.max_position[sym];
    if (max_pos > 0) {
        double max_notional = static_cast<double>(max_pos);
        notional = std::min(notional, max_notional);
    }

    // Convert to units
    return notional / price;
}

// =============================================================================
// Stop/Target Checking
// =============================================================================

/**
 * Check if stop or target has been hit.
 * Branchless calculation of P&L percentage.
 *
 * @param sym Symbol index
 * @param price Current price
 * @param state Reference to TradingState
 * @return ExitResult with should_exit and reason
 */
__attribute__((always_inline)) inline ExitResult check_stop_target(size_t sym, double price,
                                                                   const TradingState& state) {
    ExitResult result{false, HotPathExitReason::NONE};

    double qty = state.positions.quantity[sym];
    if (qty <= 0.0) {
        return result;
    }

    double entry = state.positions.avg_entry[sym];
    if (entry <= 0.0) [[unlikely]] {
        return result;
    }

    // Calculate P&L percentage (branchless)
    double pnl_pct = (price - entry) / entry;

    double stop = state.common.stop_pct[sym];
    double target = state.common.target_pct[sym];

    // Check stop (pnl_pct <= -stop)
    bool stop_hit = pnl_pct <= -stop;
    // Check target (pnl_pct >= target)
    bool target_hit = pnl_pct >= target;

    // Branchless result assignment
    result.should_exit = stop_hit | target_hit;
    // Stop has priority if both hit (unlikely edge case)
    result.reason =
        stop_hit ? HotPathExitReason::STOP : (target_hit ? HotPathExitReason::TARGET : HotPathExitReason::NONE);

    return result;
}

// =============================================================================
// Tuner Signal Checking
// =============================================================================

/**
 * Check for tuner-injected signals.
 * Signals expire after TTL.
 *
 * @param sym Symbol index
 * @param state Reference to TradingState
 * @return TunerAction with action type and quantity
 */
__attribute__((always_inline)) inline TunerAction check_tuner_signal(size_t sym, const TradingState& state) {
    TunerAction result{TunerActionType::NONE, 0.0};

    int8_t sig = state.signals.signal[sym];
    if (sig == 0) {
        return result;
    }

    // Check TTL
    uint64_t ts = state.signals.timestamp_ns[sym];
    uint64_t age = now_ns() - ts;
    if (age >= TunerSignals::SIGNAL_TTL_NS) {
        return result;
    }

    // Valid signal
    result.quantity = state.signals.quantity[sym];
    result.action = (sig > 0) ? TunerActionType::BUY : TunerActionType::SELL;

    return result;
}

// =============================================================================
// Flag Checking
// =============================================================================

/**
 * Check symbol flags for special handling.
 *
 * @param sym Symbol index
 * @param state Reference to TradingState
 * @return FlagAction indicating what to do
 */
__attribute__((always_inline)) inline FlagAction check_flags(size_t sym, const TradingState& state) {
    uint8_t f = state.flags.flags[sym];

    // Exit requested takes priority
    if (f & SymbolFlags::FLAG_EXIT_REQUESTED) {
        return FlagAction{FlagActionType::EXIT};
    }

    // Trading paused - skip this symbol
    if (f & SymbolFlags::FLAG_TRADING_PAUSED) {
        return FlagAction{FlagActionType::SKIP};
    }

    return FlagAction{FlagActionType::CONTINUE};
}

// =============================================================================
// Signal Generation
// =============================================================================

/**
 * Generate trade signal from strategy score.
 *
 * @param sym Symbol index
 * @param score Strategy score in [-1, +1]
 * @param price Current price
 * @param state Reference to TradingState
 * @return TradeSignal with action and quantity
 */
__attribute__((always_inline)) inline TradeSignal generate_signal(size_t sym, double score, double price,
                                                                  const TradingState& state) {
    TradeSignal result{SignalAction::HOLD, 0.0};

    double qty = state.positions.quantity[sym];

    // Positive score = bullish
    if (score > SCORE_THRESHOLD) {
        // Only buy if no position
        if (qty <= 0.0) {
            result.action = SignalAction::BUY;
            result.quantity = calculate_position_size(sym, price, state);
        }
        return result;
    }

    // Negative score = bearish
    if (score < -SCORE_THRESHOLD) {
        // Only sell if have position
        if (qty > 0.0) {
            double entry = state.positions.avg_entry[sym];
            double pnl_pct = (price - entry) / entry;
            double min_profit = state.common.min_profit_for_exit[sym];

            // Only exit if above minimum profit threshold
            if (pnl_pct >= min_profit) {
                result.action = SignalAction::SELL;
                result.quantity = qty;
            }
        }
    }

    return result;
}

// =============================================================================
// Execution Helpers
// =============================================================================

/**
 * Execute a buy order (update position state).
 * Note: Actual order sending is handled by execution layer.
 *
 * @param sym Symbol index
 * @param qty Quantity to buy
 * @param price Execution price
 * @param state Reference to TradingState
 */
inline void execute_buy(size_t sym, double qty, double price, TradingState& state) {
    if (qty <= 0.0 || price <= 0.0)
        return;

    double old_qty = state.positions.quantity[sym];
    double old_entry = state.positions.avg_entry[sym];

    // Update average entry price
    double new_qty = old_qty + qty;
    double new_entry = old_qty > 0.0 ? (old_entry * old_qty + price * qty) / new_qty : price;

    state.positions.quantity[sym] = new_qty;
    state.positions.avg_entry[sym] = new_entry;
    state.positions.current_price[sym] = price;
    state.positions.open_time_ns[sym] = now_ns();

    // Set position flag
    state.flags.flags[sym] |= SymbolFlags::FLAG_HAS_POSITION;

    // Update cash
    int64_t cost_x8 = static_cast<int64_t>(qty * price * FIXED_POINT_SCALE);
    state.cash_x8.fetch_sub(cost_x8, std::memory_order_relaxed);

    // Update fill counter
    state.total_fills.fetch_add(1, std::memory_order_relaxed);
}

/**
 * Execute a sell order (update position state).
 *
 * @param sym Symbol index
 * @param qty Quantity to sell
 * @param price Execution price
 * @param state Reference to TradingState
 */
inline void execute_sell(size_t sym, double qty, double price, TradingState& state) {
    if (qty <= 0.0 || price <= 0.0)
        return;

    double old_qty = state.positions.quantity[sym];
    if (old_qty <= 0.0)
        return;

    double entry = state.positions.avg_entry[sym];
    double sell_qty = std::min(qty, old_qty);

    // Calculate realized P&L
    double pnl = (price - entry) * sell_qty;

    // Update position
    double new_qty = old_qty - sell_qty;
    state.positions.quantity[sym] = new_qty;
    state.positions.current_price[sym] = price;

    // Clear position flag if fully exited
    if (new_qty <= 0.0) {
        state.flags.flags[sym] &= ~SymbolFlags::FLAG_HAS_POSITION;
        state.positions.avg_entry[sym] = 0.0;
    }

    // Update cash (add proceeds)
    int64_t proceeds_x8 = static_cast<int64_t>(sell_qty * price * FIXED_POINT_SCALE);
    state.cash_x8.fetch_add(proceeds_x8, std::memory_order_relaxed);

    // Update realized P&L
    int64_t pnl_x8 = static_cast<int64_t>(pnl * FIXED_POINT_SCALE);
    state.total_realized_pnl_x8.fetch_add(pnl_x8, std::memory_order_relaxed);

    // Update fill counter
    state.total_fills.fetch_add(1, std::memory_order_relaxed);
}

/**
 * Execute exit (close entire position).
 *
 * @param sym Symbol index
 * @param price Execution price
 * @param state Reference to TradingState
 */
inline void execute_exit(size_t sym, double price, TradingState& state) {
    double qty = state.positions.quantity[sym];
    if (qty > 0.0) {
        execute_sell(sym, qty, price, state);
    }
}

// =============================================================================
// Main Hot Path Entry Point
// =============================================================================

/**
 * Process a price update for a symbol.
 * This is the main hot path entry point.
 *
 * @param sym Symbol index
 * @param price New price
 * @param state Reference to TradingState
 */
inline void process_price_update(size_t sym, double price, TradingState& state) {
    // 0. Check global halt first
    if (!can_trade(state)) [[unlikely]] {
        return;
    }

    // 1. Capture previous price BEFORE update (for momentum calculation)
    double prev_price = state.positions.current_price[sym];

    // 2. Update current price
    state.positions.current_price[sym] = price;

    // 3. Check tuner signals first (priority)
    TunerAction tuner = check_tuner_signal(sym, state);
    if (tuner.action != TunerActionType::NONE) {
        // Consume signal
        state.signals.signal[sym] = 0;

        if (tuner.action == TunerActionType::BUY) {
            Side side = Side::Buy;
            if (check_risk(sym, side, tuner.quantity, price, state)) {
                execute_buy(sym, tuner.quantity, price, state);
            }
        } else if (tuner.action == TunerActionType::SELL) {
            execute_sell(sym, tuner.quantity, price, state);
        }
        return;
    }

    // 4. Check flags
    FlagAction flag_action = check_flags(sym, state);
    if (flag_action.action == FlagActionType::EXIT) {
        execute_exit(sym, price, state);
        state.flags.flags[sym] &= ~SymbolFlags::FLAG_EXIT_REQUESTED;
        return;
    }
    if (flag_action.action == FlagActionType::SKIP) {
        return;
    }

    // 5. Check stop/target
    ExitResult exit = check_stop_target(sym, price, state);
    if (exit.should_exit) {
        execute_exit(sym, price, state);
        if (exit.reason == HotPathExitReason::STOP) {
            state.total_stops.fetch_add(1, std::memory_order_relaxed);
        } else if (exit.reason == HotPathExitReason::TARGET) {
            state.total_targets.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    // 6. Strategy scoring - calculate momentum from price change
    strategy::Indicators ind{};

    // Calculate momentum if we have a valid previous price
    // momentum = (current - previous) / previous
    if (prev_price > 0.0) {
        ind.momentum = (price - prev_price) / prev_price;
    }

    double score = strategy::dispatch_score(sym, state, ind);

    // 7. Generate signal from score
    TradeSignal signal = generate_signal(sym, score, price, state);

    if (signal.action == SignalAction::BUY && signal.quantity > 0.0) {
        if (check_risk(sym, Side::Buy, signal.quantity, price, state)) {
            execute_buy(sym, signal.quantity, price, state);
        }
    } else if (signal.action == SignalAction::SELL) {
        execute_sell(sym, signal.quantity, price, state);
    }
}

} // namespace trading
} // namespace hft
