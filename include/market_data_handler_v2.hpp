#pragma once

#include "market_events.hpp"
#include "orderbook.hpp"
#include "types.hpp"

namespace hft {

/**
 * Market Data Handler (v2)
 *
 * Exchange-agnostic adapter that receives generic market events
 * and updates the order book. Works with any feed handler that
 * emits standard events (ITCH, Binance, Coinbase, etc.)
 */
class MarketDataHandlerV2 {
public:
    explicit MarketDataHandlerV2(OrderBook& book) : book_(book) {}

    // Order lifecycle events
    void on_order_add(const OrderAdd& event) {
        book_.add_order(event.order_id, event.side, event.price, event.quantity);
    }

    void on_order_execute(const OrderExecute& event) { book_.execute_order(event.order_id, event.quantity); }

    void on_order_reduce(const OrderReduce& event) { book_.execute_order(event.order_id, event.reduce_by); }

    void on_order_delete(const OrderDelete& event) { book_.cancel_order(event.order_id); }

    // Direct book access (for strategies)
    const OrderBook& book() const { return book_; }
    OrderBook& book() { return book_; }

private:
    OrderBook& book_;
};

/**
 * Trade-only handler (for strategies that don't need full book)
 *
 * Lighter weight - just tracks trades and quotes, no order book.
 */
template <typename Strategy>
class TradeHandler {
public:
    explicit TradeHandler(Strategy& strategy) : strategy_(strategy) {}

    void on_trade(const Trade& event) { strategy_.on_trade(event); }

    void on_quote(const QuoteUpdate& event) { strategy_.on_quote(event); }

    // Ignore order-level events
    void on_order_add(const OrderAdd&) {}
    void on_order_execute(const OrderExecute&) {}
    void on_order_reduce(const OrderReduce&) {}
    void on_order_delete(const OrderDelete&) {}

private:
    Strategy& strategy_;
};

} // namespace hft
