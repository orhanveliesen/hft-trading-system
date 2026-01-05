#pragma once

#include "../itch_messages.hpp"
#include "../market_events.hpp"
#include <cstddef>
#include <cstring>

namespace hft {
namespace feed {

/**
 * ITCH 5.0 Feed Handler
 *
 * Parses NASDAQ ITCH binary protocol and emits generic market events.
 * Template-based for zero-overhead callback dispatch.
 *
 * Callback must implement:
 *   void on_order_add(const OrderAdd&)
 *   void on_order_execute(const OrderExecute&)
 *   void on_order_reduce(const OrderReduce&)
 *   void on_order_delete(const OrderDelete&)
 */
template<typename Callback>
class ItchFeedHandler {
public:
    explicit ItchFeedHandler(Callback& callback) : callback_(callback) {}

    // Process a single ITCH message, returns true if parsed successfully
    bool process_message(const uint8_t* data, size_t len) {
        if (len < 1) return false;

        char msg_type = static_cast<char>(data[0]);

        switch (msg_type) {
            case itch::MSG_ADD_ORDER:
            case itch::MSG_ADD_ORDER_MPID:
                return parse_add_order(data, len);

            case itch::MSG_ORDER_EXECUTED:
            case itch::MSG_ORDER_EXECUTED_PRICE:
                return parse_order_executed(data, len);

            case itch::MSG_ORDER_CANCEL:
                return parse_order_cancel(data, len);

            case itch::MSG_ORDER_DELETE:
                return parse_order_delete(data, len);

            case itch::MSG_ORDER_REPLACE:
                return parse_order_replace(data, len);

            default:
                // Unknown/unsupported message type - skip
                return true;
        }
    }

    // Process MoldUDP64 packet (may contain multiple messages)
    size_t process_packet(const uint8_t* data, size_t len) {
        if (len < 20) return 0;  // MoldUDP64 header is 20 bytes

        uint16_t msg_count = itch::read_be16(&data[18]);
        size_t offset = 20;
        size_t processed = 0;

        for (uint16_t i = 0; i < msg_count && offset < len; ++i) {
            if (offset + 2 > len) break;

            uint16_t msg_len = itch::read_be16(&data[offset]);
            offset += 2;

            if (offset + msg_len > len) break;

            if (process_message(&data[offset], msg_len)) {
                ++processed;
            }
            offset += msg_len;
        }

        return processed;
    }

private:
    Callback& callback_;

    // Add Order: 36 bytes minimum
    bool parse_add_order(const uint8_t* data, size_t len) {
        if (len < 36) return false;

        OrderAdd event;
        event.symbol_id = itch::read_be16(&data[1]);  // stock_locate as symbol_id
        event.timestamp = itch::read_be48(&data[5]);
        event.order_id = itch::read_be64(&data[11]);
        event.side = (data[19] == 'B') ? Side::Buy : Side::Sell;
        event.quantity = itch::read_be32(&data[20]);
        event.price = itch::read_be32(&data[32]);

        callback_.on_order_add(event);
        return true;
    }

    // Order Executed: 31 bytes
    bool parse_order_executed(const uint8_t* data, size_t len) {
        if (len < 31) return false;

        OrderExecute event;
        event.timestamp = itch::read_be48(&data[5]);
        event.order_id = itch::read_be64(&data[11]);
        event.quantity = itch::read_be32(&data[19]);
        event.exec_price = 0;  // Not in basic execute message

        // If this is ORDER_EXECUTED_PRICE (type 'C'), price is at offset 32
        if (data[0] == itch::MSG_ORDER_EXECUTED_PRICE && len >= 36) {
            event.exec_price = itch::read_be32(&data[32]);
        }

        callback_.on_order_execute(event);
        return true;
    }

    // Order Cancel (partial): 23 bytes
    bool parse_order_cancel(const uint8_t* data, size_t len) {
        if (len < 23) return false;

        OrderReduce event;
        event.timestamp = itch::read_be48(&data[5]);
        event.order_id = itch::read_be64(&data[11]);
        event.reduce_by = itch::read_be32(&data[19]);

        callback_.on_order_reduce(event);
        return true;
    }

    // Order Delete (full cancel): 19 bytes
    bool parse_order_delete(const uint8_t* data, size_t len) {
        if (len < 19) return false;

        OrderDelete event;
        event.timestamp = itch::read_be48(&data[5]);
        event.order_id = itch::read_be64(&data[11]);

        callback_.on_order_delete(event);
        return true;
    }

    // Order Replace: 35 bytes
    bool parse_order_replace(const uint8_t* data, size_t len) {
        if (len < 35) return false;

        // Replace = Delete old + Add new
        OrderId old_order_id = itch::read_be64(&data[11]);
        OrderId new_order_id = itch::read_be64(&data[19]);
        Quantity new_qty = itch::read_be32(&data[27]);
        Price new_price = itch::read_be32(&data[31]);
        Timestamp ts = itch::read_be48(&data[5]);

        // Delete the old order
        OrderDelete del_event;
        del_event.order_id = old_order_id;
        del_event.timestamp = ts;
        callback_.on_order_delete(del_event);

        // We don't have side info in replace message, so we can't emit full OrderAdd
        // The OrderBook must track side from original add
        // For now, emit a special "replace" that downstream must handle
        // TODO: Consider adding on_order_replace callback

        (void)new_order_id;
        (void)new_qty;
        (void)new_price;

        return true;
    }
};

}  // namespace feed
}  // namespace hft
