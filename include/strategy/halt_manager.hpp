#pragma once

#include "../types.hpp"
#include <atomic>
#include <functional>
#include <vector>
#include <string>
#include <chrono>
#include <iostream>

namespace hft {
namespace strategy {

enum class HaltReason : uint8_t {
    None = 0,
    PoolExhausted,      // Order pool ran out
    PoolCritical,       // Pool below critical threshold
    MaxLossExceeded,    // Risk limit hit
    ManualHalt,         // Operator initiated (kill switch)
    SystemError,        // Unexpected error
    ConnectionLost,     // Market data/exchange connection lost
    ExchangeHalt,       // Exchange halted trading
    CircuitBreaker      // Internal circuit breaker triggered
};

inline const char* halt_reason_to_string(HaltReason reason) {
    switch (reason) {
        case HaltReason::None: return "None";
        case HaltReason::PoolExhausted: return "PoolExhausted";
        case HaltReason::PoolCritical: return "PoolCritical";
        case HaltReason::MaxLossExceeded: return "MaxLossExceeded";
        case HaltReason::ManualHalt: return "ManualHalt";
        case HaltReason::SystemError: return "SystemError";
        case HaltReason::ConnectionLost: return "ConnectionLost";
        case HaltReason::ExchangeHalt: return "ExchangeHalt";
        case HaltReason::CircuitBreaker: return "CircuitBreaker";
        default: return "Unknown";
    }
}

enum class HaltState : uint8_t {
    Running = 0,        // Normal trading
    Halting,            // Flatten in progress
    Halted,             // Safe state, all positions closed
    Error               // Flatten failed, manual intervention needed
};

inline const char* halt_state_to_string(HaltState state) {
    switch (state) {
        case HaltState::Running: return "Running";
        case HaltState::Halting: return "Halting";
        case HaltState::Halted: return "Halted";
        case HaltState::Error: return "Error";
        default: return "Unknown";
    }
}

// Position info for flattening
struct PositionInfo {
    Symbol symbol;
    std::string ticker;
    int64_t position;
    Price last_price;  // For logging
};

// Callback types
using GetPositionsCallback = std::function<std::vector<PositionInfo>()>;
using CancelAllOrdersCallback = std::function<void()>;
using SendOrderCallback = std::function<bool(Symbol symbol, Side side, Quantity qty, bool is_market)>;
using AlertCallback = std::function<void(HaltReason reason, const std::string& message)>;
using LogCallback = std::function<void(const std::string& message)>;

/**
 * HaltManager - Complete halt/flatten control
 *
 * Single point of control for emergency situations:
 * 1. Cancel all open orders
 * 2. Flatten all positions (market orders to close)
 * 3. Stop accepting new orders
 * 4. Log everything
 * 5. Alert operations
 */
class HaltManager {
public:
    HaltManager()
        : state_(HaltState::Running)
        , reason_(HaltReason::None)
        , flatten_attempts_(0)
        , max_flatten_attempts_(3)
    {
        // Default logger to stdout
        log_callback_ = [](const std::string& msg) {
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&time));
            std::cout << "[" << buf << "] [HALT] " << msg << "\n";
        };
    }

    // ========================================
    // Callback Registration
    // ========================================

    void set_get_positions_callback(GetPositionsCallback cb) {
        get_positions_callback_ = std::move(cb);
    }

    void set_cancel_all_callback(CancelAllOrdersCallback cb) {
        cancel_all_callback_ = std::move(cb);
    }

    void set_send_order_callback(SendOrderCallback cb) {
        send_order_callback_ = std::move(cb);
    }

    void set_alert_callback(AlertCallback cb) {
        alert_callback_ = std::move(cb);
    }

    void set_log_callback(LogCallback cb) {
        log_callback_ = std::move(cb);
    }

    // ========================================
    // State Queries (hot path safe)
    // ========================================

    __attribute__((always_inline))
    bool is_halted() const {
        HaltState s = state_.load(std::memory_order_acquire);
        return s != HaltState::Running;
    }

    __attribute__((always_inline))
    bool can_trade() const {
        return state_.load(std::memory_order_acquire) == HaltState::Running;
    }

    HaltState state() const {
        return state_.load(std::memory_order_acquire);
    }

    HaltReason reason() const {
        return reason_.load(std::memory_order_acquire);
    }

    // ========================================
    // Halt Control
    // ========================================

    /**
     * Trigger halt - HaltManager takes full control
     *
     * Sequence:
     * 1. Set state to Halting
     * 2. Log halt reason
     * 3. Alert operations
     * 4. Cancel all open orders
     * 5. Get all positions
     * 6. Flatten each position
     * 7. Set state to Halted (or Error if failed)
     */
    bool halt(HaltReason reason, const std::string& message = "") {
        // Atomic state transition: Running -> Halting
        HaltState expected = HaltState::Running;
        if (!state_.compare_exchange_strong(expected, HaltState::Halting,
                                            std::memory_order_acq_rel)) {
            log("Halt requested but already in state: " +
                std::string(halt_state_to_string(expected)));
            return false;
        }

        reason_.store(reason, std::memory_order_release);

        log("═══════════════════════════════════════════════════");
        log("HALT INITIATED");
        log("Reason: " + std::string(halt_reason_to_string(reason)));
        if (!message.empty()) {
            log("Message: " + message);
        }
        log("═══════════════════════════════════════════════════");

        // 1. Alert operations
        if (alert_callback_) {
            alert_callback_(reason, message);
        }

        // 2. Cancel all open orders FIRST
        log("Step 1: Cancelling all open orders...");
        if (cancel_all_callback_) {
            cancel_all_callback_();
            log("  All orders cancelled");
        } else {
            log("  WARNING: No cancel callback registered");
        }

        // 3. Get all positions
        log("Step 2: Getting all positions...");
        std::vector<PositionInfo> positions;
        if (get_positions_callback_) {
            positions = get_positions_callback_();
            log("  Found " + std::to_string(positions.size()) + " open positions");
        } else {
            log("  WARNING: No get_positions callback registered");
        }

        // 4. Flatten each position
        log("Step 3: Flattening positions...");
        bool all_flattened = flatten_positions(positions);

        // 5. Set final state
        if (all_flattened) {
            state_.store(HaltState::Halted, std::memory_order_release);
            log("═══════════════════════════════════════════════════");
            log("HALT COMPLETE - System in safe state");
            log("═══════════════════════════════════════════════════");
        } else {
            state_.store(HaltState::Error, std::memory_order_release);
            log("═══════════════════════════════════════════════════");
            log("HALT ERROR - Manual intervention required!");
            log("═══════════════════════════════════════════════════");
        }

        return true;
    }

    // Retry flattening if previous attempt failed
    bool retry_flatten() {
        if (state_.load(std::memory_order_acquire) != HaltState::Error) {
            log("Cannot retry flatten - not in Error state");
            return false;
        }

        if (flatten_attempts_ >= max_flatten_attempts_) {
            log("Max flatten attempts reached (" +
                std::to_string(max_flatten_attempts_) + ")");
            return false;
        }

        log("Retrying flatten (attempt " +
            std::to_string(flatten_attempts_ + 1) + "/" +
            std::to_string(max_flatten_attempts_) + ")...");

        state_.store(HaltState::Halting, std::memory_order_release);

        std::vector<PositionInfo> positions;
        if (get_positions_callback_) {
            positions = get_positions_callback_();
        }

        bool success = flatten_positions(positions);

        if (success) {
            state_.store(HaltState::Halted, std::memory_order_release);
            log("Retry successful - system in safe state");
        } else {
            state_.store(HaltState::Error, std::memory_order_release);
            log("Retry failed");
        }

        return success;
    }

    // Reset halt state (for testing or manual recovery after investigation)
    void reset() {
        log("Resetting halt state...");
        state_.store(HaltState::Running, std::memory_order_release);
        reason_.store(HaltReason::None, std::memory_order_release);
        flatten_attempts_ = 0;
        log("System back to Running state");
    }

    // ========================================
    // Configuration
    // ========================================

    void set_max_flatten_attempts(uint32_t attempts) {
        max_flatten_attempts_ = attempts;
    }

private:
    void log(const std::string& message) {
        if (log_callback_) {
            log_callback_(message);
        }
    }

    bool flatten_positions(const std::vector<PositionInfo>& positions) {
        ++flatten_attempts_;

        if (positions.empty()) {
            log("  No positions to flatten");
            return true;
        }

        if (!send_order_callback_) {
            log("  ERROR: No send_order callback registered!");
            return false;
        }

        bool all_success = true;

        for (const auto& pos : positions) {
            if (pos.position == 0) continue;

            Side side = (pos.position > 0) ? Side::Sell : Side::Buy;
            Quantity qty = static_cast<Quantity>(std::abs(pos.position));
            const char* side_str = (side == Side::Buy) ? "BUY" : "SELL";

            log("  Flattening " + pos.ticker + ": " + side_str + " " +
                std::to_string(qty) + " @ MARKET");

            bool success = send_order_callback_(pos.symbol, side, qty, true);

            if (success) {
                log("    -> Order sent successfully");
            } else {
                log("    -> ERROR: Failed to send order!");
                all_success = false;
            }
        }

        return all_success;
    }

    std::atomic<HaltState> state_;
    std::atomic<HaltReason> reason_;

    uint32_t flatten_attempts_;
    uint32_t max_flatten_attempts_;

    GetPositionsCallback get_positions_callback_;
    CancelAllOrdersCallback cancel_all_callback_;
    SendOrderCallback send_order_callback_;
    AlertCallback alert_callback_;
    LogCallback log_callback_;
};

}  // namespace strategy
}  // namespace hft
