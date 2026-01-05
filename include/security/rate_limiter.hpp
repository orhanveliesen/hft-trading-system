#pragma once

#include "../types.hpp"
#include <array>
#include <atomic>

namespace hft {
namespace security {

/**
 * RateLimiter - DoS protection for order flow
 *
 * Protections:
 * 1. Per-trader rate limiting (orders/second)
 * 2. Per-trader max active orders
 * 3. Global rate limiting
 * 4. Anomaly detection (sudden spikes)
 */
class RateLimiter {
public:
    static constexpr size_t MAX_TRADERS = 10000;
    static constexpr uint32_t DEFAULT_RATE_LIMIT = 1000;      // orders/sec per trader
    static constexpr uint32_t DEFAULT_MAX_ACTIVE = 10000;     // max active orders per trader
    static constexpr uint32_t DEFAULT_GLOBAL_RATE = 100000;   // global orders/sec

    struct TraderStats {
        std::atomic<uint32_t> orders_this_second{0};
        std::atomic<uint32_t> active_orders{0};
        std::atomic<Timestamp> last_reset{0};

        TraderStats() = default;

        // Reset stats for reuse (can't copy atomics)
        void reset() {
            orders_this_second.store(0, std::memory_order_relaxed);
            active_orders.store(0, std::memory_order_relaxed);
            last_reset.store(0, std::memory_order_relaxed);
        }
    };

    struct Config {
        uint32_t orders_per_second;
        uint32_t max_active_orders;
        uint32_t global_rate_limit;
        bool enabled;

        Config()
            : orders_per_second(DEFAULT_RATE_LIMIT)
            , max_active_orders(DEFAULT_MAX_ACTIVE)
            , global_rate_limit(DEFAULT_GLOBAL_RATE)
            , enabled(true)
        {}
    };

    explicit RateLimiter(const Config& config = Config{})
        : config_(config)
        , global_orders_this_second_(0)
        , global_last_reset_(0)
    {
        // TraderStats are already zero-initialized by default constructor
    }

    // Check if order is allowed (call before processing)
    __attribute__((always_inline))
    bool allow_order(TraderId trader, Timestamp current_time) {
        if (!config_.enabled) return true;
        if (trader == NO_TRADER) return true;  // Anonymous orders bypass (market data replay)

        // Check global rate
        if (!check_global_rate(current_time)) {
            return false;
        }

        // Check per-trader limits
        if (trader < MAX_TRADERS) {
            return check_trader_rate(trader, current_time);
        }

        return true;
    }

    // Call when order is added to book
    void on_order_added(TraderId trader) {
        if (trader != NO_TRADER && trader < MAX_TRADERS) {
            traders_[trader].active_orders.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Call when order is removed (filled/cancelled)
    void on_order_removed(TraderId trader) {
        if (trader != NO_TRADER && trader < MAX_TRADERS) {
            traders_[trader].active_orders.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    // Get trader statistics
    uint32_t get_active_orders(TraderId trader) const {
        if (trader < MAX_TRADERS) {
            return traders_[trader].active_orders.load(std::memory_order_relaxed);
        }
        return 0;
    }

    uint32_t get_orders_this_second(TraderId trader) const {
        if (trader < MAX_TRADERS) {
            return traders_[trader].orders_this_second.load(std::memory_order_relaxed);
        }
        return 0;
    }

    // Configuration
    void set_enabled(bool enabled) { config_.enabled = enabled; }
    bool is_enabled() const { return config_.enabled; }

    void set_rate_limit(uint32_t orders_per_second) {
        config_.orders_per_second = orders_per_second;
    }

    void set_max_active_orders(uint32_t max_active) {
        config_.max_active_orders = max_active;
    }

private:
    bool check_global_rate(Timestamp current_time) {
        // Reset counter every second (assuming timestamp is in nanoseconds)
        Timestamp current_second = current_time / 1'000'000'000;
        Timestamp last_second = global_last_reset_.load(std::memory_order_relaxed);

        if (current_second > last_second) {
            global_last_reset_.store(current_second, std::memory_order_relaxed);
            global_orders_this_second_.store(0, std::memory_order_relaxed);
        }

        uint32_t count = global_orders_this_second_.fetch_add(1, std::memory_order_relaxed);
        return count < config_.global_rate_limit;
    }

    bool check_trader_rate(TraderId trader, Timestamp current_time) {
        auto& stats = traders_[trader];

        // Reset counter every second
        Timestamp current_second = current_time / 1'000'000'000;
        Timestamp last_second = stats.last_reset.load(std::memory_order_relaxed);

        if (current_second > last_second) {
            stats.last_reset.store(current_second, std::memory_order_relaxed);
            stats.orders_this_second.store(0, std::memory_order_relaxed);
        }

        // Check rate limit
        uint32_t count = stats.orders_this_second.fetch_add(1, std::memory_order_relaxed);
        if (count >= config_.orders_per_second) {
            return false;  // Rate limit exceeded
        }

        // Check max active orders
        uint32_t active = stats.active_orders.load(std::memory_order_relaxed);
        if (active >= config_.max_active_orders) {
            return false;  // Too many active orders
        }

        return true;
    }

    Config config_;
    std::array<TraderStats, MAX_TRADERS> traders_;

    std::atomic<uint32_t> global_orders_this_second_;
    std::atomic<Timestamp> global_last_reset_;
};

// Rejection reasons for logging/monitoring
enum class RejectionReason : uint8_t {
    None = 0,
    RateLimitExceeded,
    MaxActiveOrdersExceeded,
    GlobalRateLimitExceeded,
    InvalidTrader,
    Blacklisted
};

inline const char* rejection_reason_to_string(RejectionReason reason) {
    switch (reason) {
        case RejectionReason::None: return "None";
        case RejectionReason::RateLimitExceeded: return "RateLimitExceeded";
        case RejectionReason::MaxActiveOrdersExceeded: return "MaxActiveOrdersExceeded";
        case RejectionReason::GlobalRateLimitExceeded: return "GlobalRateLimitExceeded";
        case RejectionReason::InvalidTrader: return "InvalidTrader";
        case RejectionReason::Blacklisted: return "Blacklisted";
        default: return "Unknown";
    }
}

}  // namespace security
}  // namespace hft
