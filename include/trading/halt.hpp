#pragma once

/**
 * Halt Management Module - Unified Halt Control
 *
 * Provides halt management functions for the TradingState SoA structure.
 * Replaces callback-based HaltManager with simpler flags-based approach.
 *
 * Functions:
 * - trigger_halt(): Initiate halt sequence
 * - flatten_all_positions(): Set exit flags for all positions
 * - reset_halt(): Reset halt state
 * - is_halted(): Check if system is halted
 * - can_trade(): Check if trading is allowed
 *
 * Halt Sequence:
 * 1. RUNNING -> HALTING (trigger_halt)
 * 2. Set EXIT_REQUESTED flags for all positions
 * 3. HALTING -> HALTED (after positions flattened)
 * 4. HALTED -> RUNNING (reset_halt - manual operator action)
 */

#include "trading_state.hpp"

#include <chrono>

namespace hft {
namespace trading {

/**
 * Get current timestamp in nanoseconds.
 */
inline uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}

/**
 * Trigger halt - transitions system to HALTING state.
 *
 * Atomic transition: RUNNING -> HALTING.
 * If already halting/halted, this is a no-op (first halt wins).
 *
 * @param state Reference to TradingState
 * @param reason Reason for the halt
 */
inline void trigger_halt(TradingState& state, HaltReason reason) {
    uint8_t expected = static_cast<uint8_t>(HaltStatus::RUNNING);
    uint8_t halting = static_cast<uint8_t>(HaltStatus::HALTING);

    if (state.halt.halted.compare_exchange_strong(expected, halting, std::memory_order_acq_rel)) {
        // We won the race - set reason and timestamp
        state.halt.reason.store(static_cast<uint8_t>(reason), std::memory_order_release);
        state.halt.halt_time_ns.store(now_ns(), std::memory_order_release);
    }
    // If CAS failed, another thread already triggered halt - that's fine
}

/**
 * Set EXIT_REQUESTED flag for all symbols with positions.
 * Then transition from HALTING to HALTED.
 *
 * @param state Reference to TradingState
 */
inline void flatten_all_positions(TradingState& state) {
    // Set exit flag for all symbols with positions
    for (size_t sym = 0; sym < MAX_SYMBOLS; ++sym) {
        // Check if symbol has position (quantity > 0 or FLAG_HAS_POSITION set)
        bool has_position =
            state.positions.quantity[sym] > 0 || (state.flags.flags[sym] & SymbolFlags::FLAG_HAS_POSITION);

        if (has_position) {
            state.flags.flags[sym] |= SymbolFlags::FLAG_EXIT_REQUESTED;
        }
    }

    // Transition to HALTED status
    state.halt.halted.store(static_cast<uint8_t>(HaltStatus::HALTED), std::memory_order_release);
}

/**
 * Reset halt state back to RUNNING.
 * Only call this after positions are flattened and situation is resolved.
 *
 * @param state Reference to TradingState
 */
inline void reset_halt(TradingState& state) {
    state.halt.halted.store(static_cast<uint8_t>(HaltStatus::RUNNING), std::memory_order_release);
    state.halt.reason.store(static_cast<uint8_t>(HaltReason::NONE), std::memory_order_release);
    state.risk_state.risk_halted.store(0, std::memory_order_release);
}

/**
 * Check if system is halted (either HALTING or HALTED).
 * Hot path safe - use relaxed memory ordering.
 *
 * @param state Reference to TradingState
 * @return true if not in RUNNING state
 */
__attribute__((always_inline)) inline bool is_halted(const TradingState& state) {
    return state.halt.halted.load(std::memory_order_relaxed) != static_cast<uint8_t>(HaltStatus::RUNNING);
}

/**
 * Check if trading is allowed.
 * Hot path safe - checks both halt status and risk halt.
 *
 * @param state Reference to TradingState
 * @return true if trading is allowed
 */
__attribute__((always_inline)) inline bool can_trade(const TradingState& state) {
    // Check global halt
    if (state.halt.halted.load(std::memory_order_relaxed) != static_cast<uint8_t>(HaltStatus::RUNNING)) {
        return false;
    }

    // Check risk halt
    if (state.risk_state.risk_halted.load(std::memory_order_relaxed) != 0) {
        return false;
    }

    return true;
}

/**
 * Get human-readable halt reason string.
 */
inline const char* halt_reason_str(HaltReason reason) {
    switch (reason) {
    case HaltReason::NONE:
        return "None";
    case HaltReason::RISK_LIMIT:
        return "RiskLimit";
    case HaltReason::MANUAL:
        return "Manual";
    case HaltReason::SYSTEM_ERROR:
        return "SystemError";
    case HaltReason::CONNECTION_LOST:
        return "ConnectionLost";
    case HaltReason::POOL_EXHAUSTED:
        return "PoolExhausted";
    default:
        return "Unknown";
    }
}

/**
 * Get human-readable halt status string.
 */
inline const char* halt_status_str(HaltStatus status) {
    switch (status) {
    case HaltStatus::RUNNING:
        return "Running";
    case HaltStatus::HALTING:
        return "Halting";
    case HaltStatus::HALTED:
        return "Halted";
    default:
        return "Unknown";
    }
}

} // namespace trading
} // namespace hft
