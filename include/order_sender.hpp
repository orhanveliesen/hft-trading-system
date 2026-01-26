#pragma once

/**
 * OrderSender - Order Sending Interface
 *
 * Defines the OrderSender concept and provides default implementations.
 * Uses C++23 concepts for compile-time type checking.
 *
 * See concepts.hpp for the full concept definition.
 */

#include "types.hpp"
#include "concepts.hpp"

namespace hft {

/**
 * NullOrderSender - No-op implementation
 *
 * Used when order sending is not needed (e.g., backtest, market data only).
 * All operations succeed but do nothing.
 */
struct NullOrderSender {
    bool send_order(Symbol /*symbol*/, Side /*side*/, Quantity /*qty*/, bool /*is_market*/) {
        return true;
    }

    bool cancel_order(Symbol /*symbol*/, OrderId /*order_id*/) {
        return true;
    }
};

// Static assertion to verify NullOrderSender satisfies the concept
static_assert(concepts::OrderSender<NullOrderSender>,
              "NullOrderSender must satisfy OrderSender concept");

}  // namespace hft
