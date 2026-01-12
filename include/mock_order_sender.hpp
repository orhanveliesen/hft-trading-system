#pragma once

#include "order_sender.hpp"
#include <vector>
#include <utility>

namespace hft {

/**
 * MockOrderSender - Test implementation
 *
 * Records all orders for verification in tests.
 * Can be configured to simulate failures.
 */
class MockOrderSender {
public:
    struct OrderRecord {
        Symbol symbol;
        Side side;
        Quantity quantity;
        bool is_market;
    };

    struct CancelRecord {
        Symbol symbol;
        OrderId order_id;
    };

    MockOrderSender() : fail_next_send_(false), fail_next_cancel_(false) {}

    // OrderSender interface
    bool send_order(Symbol symbol, Side side, Quantity qty, bool is_market) {
        if (fail_next_send_) {
            fail_next_send_ = false;
            return false;
        }

        sent_orders_.push_back({symbol, side, qty, is_market});
        return true;
    }

    bool cancel_order(Symbol symbol, OrderId order_id) {
        if (fail_next_cancel_) {
            fail_next_cancel_ = false;
            return false;
        }

        cancelled_orders_.push_back({symbol, order_id});
        return true;
    }

    // Test helpers
    const std::vector<OrderRecord>& sent_orders() const { return sent_orders_; }
    const std::vector<CancelRecord>& cancelled_orders() const { return cancelled_orders_; }

    size_t send_count() const { return sent_orders_.size(); }
    size_t cancel_count() const { return cancelled_orders_.size(); }

    void clear() {
        sent_orders_.clear();
        cancelled_orders_.clear();
    }

    // Simulate failures
    void fail_next_send() { fail_next_send_ = true; }
    void fail_next_cancel() { fail_next_cancel_ = true; }

    // Get last order
    const OrderRecord& last_order() const { return sent_orders_.back(); }
    const CancelRecord& last_cancel() const { return cancelled_orders_.back(); }

private:
    std::vector<OrderRecord> sent_orders_;
    std::vector<CancelRecord> cancelled_orders_;
    bool fail_next_send_;
    bool fail_next_cancel_;
};

// Verify implementations satisfy the concept
static_assert(concepts::OrderSender<MockOrderSender>,
              "MockOrderSender must satisfy OrderSender concept");
static_assert(concepts::OrderSender<NullOrderSender>,
              "NullOrderSender must satisfy OrderSender concept");

}  // namespace hft
