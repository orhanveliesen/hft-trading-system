#pragma once

#include "types.hpp"
#include "orderbook.hpp"

namespace hft {

/**
 * Market Data Handler
 *
 * Exchange-agnostic adapter between feed handlers and order book.
 * Implements the callback interface expected by FeedHandler template.
 *
 * Works with any feed handler that calls:
 *   on_add_order(OrderId, Side, Price, Quantity)
 *   on_order_executed(OrderId, Quantity)
 *   on_order_cancelled(OrderId, Quantity)
 *   on_order_deleted(OrderId)
 */
class MarketDataHandler {
public:
    explicit MarketDataHandler(OrderBook& book) : book_(book) {}

    // New order added to book
    void on_add_order(OrderId order_id, Side side, Price price, Quantity qty) {
        book_.add_order(order_id, side, price, qty);
    }

    // Order executed (partial or full)
    void on_order_executed(OrderId order_id, Quantity qty) {
        book_.execute_order(order_id, qty);
    }

    // Order partially cancelled (reduce quantity)
    void on_order_cancelled(OrderId order_id, Quantity qty) {
        book_.execute_order(order_id, qty);  // Same effect as partial execute
    }

    // Order fully removed from book
    void on_order_deleted(OrderId order_id) {
        book_.cancel_order(order_id);
    }

    // Direct book access
    const OrderBook& book() const { return book_; }
    OrderBook& book() { return book_; }

private:
    OrderBook& book_;
};

}  // namespace hft
