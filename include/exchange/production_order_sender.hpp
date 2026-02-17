#pragma once

/**
 * ProductionOrderSender - Real order sender for Binance
 *
 * Placeholder implementation - actual order submission not yet implemented.
 * When implemented, this will:
 * - Sign requests with API key/secret
 * - Send orders via REST API
 * - Handle responses and track fills
 */

#include "../types.hpp"

#include <cstdint>
#include <iostream>

namespace hft {
namespace exchange {

class ProductionOrderSender {
public:
    ProductionOrderSender() : total_orders_(0) {}

    // 5-param version with expected_price for slippage tracking
    // NOTE: qty is double to support fractional crypto quantities (e.g., 0.01 BTC)
    bool send_order(Symbol /*symbol*/, Side /*side*/, double /*qty*/, Price /*expected_price*/, bool /*is_market*/) {
        // TODO: Implement real order submission
        // - Sign request with API key/secret
        // - Send via REST API
        // - Handle response
        // - Track expected_price for slippage calculation on fill
        total_orders_++;
        std::cerr << "[PRODUCTION] Order would be sent here\n";
        return false;  // Not implemented yet
    }

    // 4-param backward-compatible version (satisfies OrderSender concept)
    bool send_order(Symbol symbol, Side side, double qty, bool is_market) {
        return send_order(symbol, side, qty, 0, is_market);  // No expected_price tracking
    }

    bool cancel_order(Symbol /*symbol*/, OrderId /*id*/) {
        // TODO: Implement real cancel
        return false;
    }

    uint64_t total_orders() const { return total_orders_; }

private:
    uint64_t total_orders_;
};

// Local OrderSender concept with expected_price for slippage tracking
// NOTE: qty is double to support fractional crypto quantities (e.g., 0.01 BTC)
// Named "LocalOrderSender" to avoid conflict with hft::concepts::OrderSender
template<typename T>
concept LocalOrderSender = requires(T& sender, Symbol s, Side side, double qty, Price p, OrderId id, bool is_market) {
    { sender.send_order(s, side, qty, p, is_market) } -> std::convertible_to<bool>;
    { sender.cancel_order(s, id) } -> std::convertible_to<bool>;
};

}  // namespace exchange
}  // namespace hft
