#pragma once

#include "../types.hpp"
#include "execution_engine.hpp"
#include "order_request.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace hft {
namespace execution {

/**
 * TradingEngine<Venue> - Template-based execution engine for event-driven architecture
 *
 * Key difference from ExecutionEngine:
 * - Compile-time polymorphism via template (not runtime)
 * - Simpler API: execute OrderRequest directly
 * - Venue-specific behavior via if constexpr
 *
 * Position checking:
 * - Spot: Enforce position checks (don't sell if no position)
 * - Futures: No position checks (can have both long + short)
 *
 * Design pattern from Phase 5.0:
 * EventBus → SpotEngine.execute() → IExchangeAdapter
 */
template <Venue V>
class TradingEngine {
public:
    using PositionCallback = std::function<double(Symbol)>; // Returns current position
    using OrderCallback =
        std::function<void(uint64_t order_id, Symbol symbol, Side side, double qty, Price price, OrderType type)>;

    static constexpr double MIN_POSITION_THRESHOLD = 0.0001;    // Dust threshold
    static constexpr uint64_t LIMIT_TIMEOUT_NS = 5'000'000'000; // 5 seconds
    static constexpr uint8_t MAX_CANCEL_ATTEMPTS = 3;
    static constexpr uint64_t CANCEL_RETRY_INTERVAL_NS = 1'000'000'000; // 1 second

    explicit TradingEngine(IExchangeAdapter* exchange) : exchange_(exchange) { pending_orders_.resize(64); }

    void set_position_callback(PositionCallback cb) { get_position_ = std::move(cb); }
    void set_order_callback(OrderCallback cb) { on_order_ = std::move(cb); }

    /**
     * Execute an OrderRequest
     *
     * Spot: Check position before selling
     * Futures: No position check (can have both long + short)
     *
     * @param req Order request from pipeline
     * @param market Current market snapshot
     * @return Order ID if sent, 0 if rejected
     */
    uint64_t execute(const OrderRequest& req, const MarketSnapshot& market) {
        if (!req.is_valid() || !exchange_) {
            return 0;
        }

        Side side = req.side;
        double qty = req.qty;
        Price expected_price = (side == Side::Buy) ? market.ask : market.bid;

        // Position check ONLY for Spot venue
        if constexpr (V == Venue::Spot) {
            if (side == Side::Sell && get_position_) {
                double current_position = get_position_(req.symbol);

                // If position is dust (below threshold), reject the order
                if (current_position < MIN_POSITION_THRESHOLD) {
                    return 0; // No position to sell
                }

                // Limit qty to available position
                if (qty > current_position) {
                    qty = current_position;
                }
            }
        }
        // Futures: No position check (compile-time branch eliminated)

        // Send order based on type
        uint64_t order_id = 0;
        if (req.type == OrderType::Market) {
            order_id = exchange_->send_market_order(req.symbol, side, qty, expected_price);
        } else {
            Price limit_price = req.limit_price > 0 ? req.limit_price : expected_price;
            order_id = exchange_->send_limit_order(req.symbol, side, qty, limit_price);
            if (order_id > 0) {
                track_pending_order(order_id, req.symbol, side, qty, limit_price, expected_price);
            }
        }

        // Notify callback
        if (order_id > 0 && on_order_) {
            Price order_price = (req.type == OrderType::Market) ? expected_price : req.limit_price;
            on_order_(order_id, req.symbol, side, qty, order_price, req.type);
        }

        return order_id;
    }

    /**
     * Cancel a specific order
     *
     * @param order_id Order ID to cancel
     * @return Cancel result
     */
    CancelResult cancel_order(uint64_t order_id) {
        if (!exchange_) {
            return CancelResult::NetworkError;
        }

        // Find pending order
        for (auto& po : pending_orders_) {
            if (po.is_active() && po.order_id == order_id) {
                return try_cancel(po);
            }
        }

        // Not found in pending list → ask exchange
        return exchange_->cancel_order(order_id);
    }

    /**
     * Cancel all pending orders for a symbol
     *
     * @param symbol Symbol ID
     * @return Number of orders resolved (Success + NotFound)
     */
    int cancel_pending_for_symbol(Symbol symbol) {
        int resolved = 0;
        for (auto& po : pending_orders_) {
            if (po.is_active() && po.symbol == symbol) {
                auto result = try_cancel(po);
                if (result == CancelResult::Success || result == CancelResult::NotFound) {
                    resolved++;
                }
            }
        }
        return resolved;
    }

    /**
     * Cancel stale pending orders (call periodically)
     *
     * From PR #60: Fault-tolerant cancellation
     */
    void cancel_stale_orders(uint64_t current_time_ns) {
        for (auto& po : pending_orders_) {
            if (!po.is_active())
                continue;

            uint64_t age = current_time_ns - po.submit_time_ns;

            if (po.state == OrderState::Active && age > LIMIT_TIMEOUT_NS) {
                // Stale active order → try cancel
                try_cancel(po);
            }
        }
    }

    /**
     * Recover stuck orders (call periodically from heartbeat)
     *
     * From PR #60: Retry failed cancels, query exchange for ground truth
     */
    void recover_stuck_orders() {
        if (!exchange_)
            return;

        uint64_t now = util::now_ns();

        for (auto& po : pending_orders_) {
            if (po.state != OrderState::CancelFailed)
                continue;

            // Respect retry interval
            if (now - po.last_cancel_attempt_ns < CANCEL_RETRY_INTERVAL_NS)
                continue;

            if (po.cancel_attempts < MAX_CANCEL_ATTEMPTS) {
                // Retry cancel
                try_cancel(po);
            } else {
                // Max retries exceeded → query exchange for ground truth
                if (exchange_->is_order_pending(po.order_id)) {
                    // Still there — one final cancel attempt
                    po.cancel_attempts = 0; // Reset for final round
                    try_cancel(po);
                } else {
                    // Gone (filled or expired without our knowledge)
                    po.clear();
                }
            }
        }
    }

    /**
     * Called when fill occurs (to clear pending order)
     */
    void on_fill(uint64_t order_id) {
        for (auto& po : pending_orders_) {
            if (po.is_active() && po.order_id == order_id) {
                po.clear(); // Order filled, remove from pending
                return;
            }
        }
    }

    /**
     * Get count of pending orders
     */
    size_t pending_order_count() const {
        size_t count = 0;
        for (const auto& po : pending_orders_) {
            if (po.is_active())
                count++;
        }
        return count;
    }

private:
    IExchangeAdapter* exchange_;
    std::vector<PendingOrder> pending_orders_;
    PositionCallback get_position_;
    OrderCallback on_order_;

    /**
     * Track a pending limit order
     */
    void track_pending_order(uint64_t order_id, Symbol symbol, Side side, double qty, Price limit_price,
                             Price expected_price) {
        // Find free slot
        for (auto& po : pending_orders_) {
            if (!po.is_active()) {
                po.order_id = order_id;
                po.symbol = symbol;
                po.side = side;
                po.quantity = qty;
                po.limit_price = limit_price;
                po.expected_fill_price = expected_price;
                po.submit_time_ns = util::now_ns();
                po.state = OrderState::Active;
                po.cancel_attempts = 0;
                po.last_cancel_attempt_ns = 0;
                return;
            }
        }
        // No free slot - should not happen if max_pending_orders is set correctly
    }

    /**
     * Attempt to cancel a single pending order with state tracking
     *
     * From PR #60: State machine for cancel retry
     */
    CancelResult try_cancel(PendingOrder& po) {
        if (!exchange_)
            return CancelResult::NetworkError;

        po.state = OrderState::CancelSent;
        po.cancel_attempts++;
        po.last_cancel_attempt_ns = util::now_ns();

        auto result = exchange_->cancel_order(po.order_id);

        switch (result) {
        case CancelResult::Success:
            po.clear();
            return CancelResult::Success;

        case CancelResult::NotFound:
            // Order already gone (filled or expired) — clear local state
            po.clear();
            return CancelResult::NotFound;

        case CancelResult::NetworkError:
        case CancelResult::RateLimited:
            po.state = OrderState::CancelFailed;
            return result;
        }

        return CancelResult::NetworkError; // LCOV_EXCL_LINE - Unreachable, compiler happy
    }
};

// Type aliases for convenience
using SpotEngine = TradingEngine<Venue::Spot>;
using FuturesEngine = TradingEngine<Venue::Futures>;

} // namespace execution
} // namespace hft
