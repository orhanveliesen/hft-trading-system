#pragma once

#include "../order_sender.hpp"
#include "binance_client.hpp"
#include <unordered_map>
#include <string>
#include <memory>

namespace hft {
namespace exchange {

/**
 * BinanceOrderSender - OrderSender implementation for Binance
 *
 * Wraps BinanceClient to satisfy the OrderSender concept.
 * Handles Symbol ID -> ticker string mapping.
 */
class BinanceOrderSender {
public:
    explicit BinanceOrderSender(BinanceClient& client) : client_(client) {}

    // Register symbol mapping (Symbol ID -> ticker string)
    void register_symbol(Symbol id, const std::string& ticker) {
        symbol_to_ticker_[id] = ticker;
    }

    // OrderSender interface
    bool send_order(Symbol symbol, Side side, Quantity qty, bool is_market) {
        auto it = symbol_to_ticker_.find(symbol);
        if (it == symbol_to_ticker_.end()) {
            return false;  // Unknown symbol
        }

        OrderRequest req;
        req.symbol = it->second;
        req.side = side;
        req.type = is_market ? OrderType::Market : OrderType::Limit;
        req.tif = TimeInForce::IOC;  // Default to IOC for HFT
        req.quantity = qty;
        req.price = 0;  // Market orders don't need price

        OrderResponse resp = client_.place_order(req);
        return resp.success;
    }

    bool cancel_order(Symbol symbol, OrderId order_id) {
        auto it = symbol_to_ticker_.find(symbol);
        if (it == symbol_to_ticker_.end()) {
            return false;
        }

        return client_.cancel_order(it->second, order_id);
    }

private:
    BinanceClient& client_;
    std::unordered_map<Symbol, std::string> symbol_to_ticker_;
};

// Verify concept satisfaction
static_assert(is_order_sender_v<BinanceOrderSender>, "BinanceOrderSender must satisfy OrderSender concept");

}  // namespace exchange
}  // namespace hft
