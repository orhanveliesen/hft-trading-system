#pragma once

#include "../core/event_bus.hpp"
#include "../core/events.hpp"
#include "../types.hpp"
#include "../util/time_utils.hpp"
#include "trading_engine.hpp"

#include <array>
#include <cstdint>

namespace hft {
namespace execution {

/**
 * LimitManager - Manages limit order lifecycle via events
 *
 * Responsibilities:
 * 1. Track pending limit orders
 * 2. Check for timeouts (call check_timeouts() periodically)
 * 3. Subscribe to LimitCancelEvent → cancel via SpotEngine
 * 4. Publish LimitCancelEvent on timeout
 *
 * Design pattern from Phase 5.0:
 * - Event-driven: subscribes to LimitCancelEvent, publishes on timeout
 * - Works with SpotEngine for actual cancel execution
 * - Timeout check called from heartbeat loop in trader.cpp
 */
class LimitManager {
public:
    struct PendingLimit {
        uint64_t order_id = 0;
        Symbol symbol = 0;
        Side side = Side::Buy;
        Price limit_price = 0;
        double qty = 0.0;
        uint64_t submit_time_ns = 0;
        bool active = false;

        void clear() {
            order_id = 0;
            symbol = 0;
            side = Side::Buy;
            limit_price = 0;
            qty = 0.0;
            submit_time_ns = 0;
            active = false;
        }
    };

    static constexpr size_t MAX_PENDING = 64;
    static constexpr uint64_t DEFAULT_TIMEOUT_NS = 5'000'000'000; // 5 seconds

    /**
     * Constructor subscribes to LimitCancelEvent
     *
     * @param bus EventBus for subscribing to cancel events
     * @param spot_engine SpotEngine for executing cancels
     */
    LimitManager(core::EventBus* bus, SpotEngine* spot_engine)
        : bus_(bus), spot_engine_(spot_engine), timeout_ns_(DEFAULT_TIMEOUT_NS) {
        // Subscribe to LimitCancelEvent
        bus_->subscribe<core::LimitCancelEvent>([this](const core::LimitCancelEvent& e) {
            if (e.order_id == 0) {
                // order_id == 0 means cancel all for symbol
                cancel_all_for_symbol(e.symbol);
            } else {
                // Cancel specific order
                cancel_specific(e.symbol, e.order_id);
            }
        });
    }

    /**
     * Track a new pending limit order
     *
     * @param id Symbol ID
     * @param order_id Order ID from exchange
     * @param side Order side
     * @param price Limit price
     * @param qty Order quantity
     */
    void track(Symbol id, uint64_t order_id, Side side, Price price, double qty) {
        auto& p = pending_[id];
        p.order_id = order_id;
        p.symbol = id;
        p.side = side;
        p.limit_price = price;
        p.qty = qty;
        p.submit_time_ns = util::now_ns();
        p.active = true;
    }

    /**
     * Check for timeout and publish LimitCancelEvent
     *
     * Call this periodically from heartbeat loop
     */
    void check_timeouts() {
        uint64_t now = util::now_ns();
        for (auto& p : pending_) {
            if (p.active && (now - p.submit_time_ns) > timeout_ns_) {
                // Timeout → publish cancel event
                bus_->publish(core::LimitCancelEvent{
                    .symbol = p.symbol, .order_id = p.order_id, .reason = "timeout", .timestamp_ns = now});
                p.active = false;
            }
        }
    }

    /**
     * Mark order as filled (stop tracking)
     */
    void on_fill(uint64_t order_id) {
        for (auto& p : pending_) {
            if (p.active && p.order_id == order_id) {
                p.clear();
                return;
            }
        }
    }

    /**
     * Get pending limit for a symbol (for testing/inspection)
     */
    const PendingLimit* get_pending(Symbol id) const {
        if (id >= MAX_PENDING)
            return nullptr;
        return pending_[id].active ? &pending_[id] : nullptr;
    }

    /**
     * Set custom timeout (for testing)
     */
    void set_timeout_ns(uint64_t timeout_ns) { timeout_ns_ = timeout_ns; }

    /**
     * Get count of active pending limits
     */
    size_t pending_count() const {
        size_t count = 0;
        for (const auto& p : pending_) {
            if (p.active)
                count++;
        }
        return count;
    }

private:
    core::EventBus* bus_;
    SpotEngine* spot_engine_;
    std::array<PendingLimit, MAX_PENDING> pending_;
    uint64_t timeout_ns_;

    /**
     * Cancel all pending limits for a symbol
     *
     * Called when LimitCancelEvent has order_id == 0
     */
    void cancel_all_for_symbol(Symbol id) {
        if (id >= MAX_PENDING)
            return;

        auto& p = pending_[id];
        if (p.active) {
            spot_engine_->cancel_order(p.order_id);
            p.active = false;
        }
    }

    /**
     * Cancel specific order
     *
     * Called when LimitCancelEvent has specific order_id
     */
    void cancel_specific(Symbol id, uint64_t order_id) {
        if (id >= MAX_PENDING)
            return;

        auto& p = pending_[id];
        if (p.active && p.order_id == order_id) {
            spot_engine_->cancel_order(order_id);
            p.active = false;
        }
    }
};

} // namespace execution
} // namespace hft
