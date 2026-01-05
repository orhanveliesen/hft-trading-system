#pragma once

#include "types.hpp"
#include "itch_messages.hpp"
#include <cstddef>

namespace hft {

/**
 * Feed Handler - Generic market data parser
 *
 * Template-based for compile-time callback binding (no vtable overhead).
 * Parses binary protocol and emits events via primitive parameters.
 *
 * Callback must implement:
 *   void on_add_order(OrderId, Side, Price, Quantity)
 *   void on_order_executed(OrderId, Quantity)
 *   void on_order_cancelled(OrderId, Quantity)  // partial cancel
 *   void on_order_deleted(OrderId)              // full cancel
 *
 * This is the ITCH implementation. Same callback interface can be used
 * with other feed handlers (Binance, Coinbase, etc.)
 */
template<typename Callback>
class FeedHandler {
public:
    explicit FeedHandler(Callback& callback) : callback_(callback) {}

    // Process a single ITCH message
    // Returns true if message was parsed successfully
    bool process_message(const uint8_t* data, size_t len) {
        if (len < 1) return false;

        char msg_type = static_cast<char>(data[0]);

        switch (msg_type) {
            case itch::MSG_ADD_ORDER:
            case itch::MSG_ADD_ORDER_MPID:
                return parse_add_order(data, len);

            case itch::MSG_ORDER_EXECUTED:
                return parse_order_executed(data, len);

            case itch::MSG_ORDER_CANCEL:
                return parse_order_cancel(data, len);

            case itch::MSG_ORDER_DELETE:
                return parse_order_delete(data, len);

            default:
                // Unknown message type - skip
                return true;
        }
    }

private:
    Callback& callback_;

    // Add Order: 36 bytes
    bool parse_add_order(const uint8_t* data, size_t len) {
        if (len < 36) return false;

        OrderId order_id = itch::read_be64(&data[11]);
        Side side = (data[19] == 'B') ? Side::Buy : Side::Sell;
        Quantity qty = itch::read_be32(&data[20]);
        Price price = itch::read_be32(&data[32]);

        callback_.on_add_order(order_id, side, price, qty);
        return true;
    }

    // Order Executed: 31 bytes
    bool parse_order_executed(const uint8_t* data, size_t len) {
        if (len < 31) return false;

        OrderId order_id = itch::read_be64(&data[11]);
        Quantity qty = itch::read_be32(&data[19]);

        callback_.on_order_executed(order_id, qty);
        return true;
    }

    // Order Cancel (partial): 23 bytes
    bool parse_order_cancel(const uint8_t* data, size_t len) {
        if (len < 23) return false;

        OrderId order_id = itch::read_be64(&data[11]);
        Quantity qty = itch::read_be32(&data[19]);

        callback_.on_order_cancelled(order_id, qty);
        return true;
    }

    // Order Delete (full): 19 bytes
    bool parse_order_delete(const uint8_t* data, size_t len) {
        if (len < 19) return false;

        OrderId order_id = itch::read_be64(&data[11]);

        callback_.on_order_deleted(order_id);
        return true;
    }
};

}  // namespace hft
