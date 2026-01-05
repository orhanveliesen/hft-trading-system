#pragma once

#include "types.hpp"
#include "book_side.hpp"
#include <array>
#include <memory>
#include <functional>

namespace hft {

// Matching Engine with price-time priority
// Wraps order book and adds matching logic
class MatchingEngine {
public:
    using TradeCallback = std::function<void(const Trade&)>;

    MatchingEngine(Price base_price, size_t price_range);

    // Set callback for trade notifications
    void set_trade_callback(TradeCallback callback);

    // Add order - may trigger matching
    // Returns OrderResult indicating success or failure reason
    OrderResult add_order(OrderId id, Side side, Price price, Quantity quantity,
                          TraderId trader_id = NO_TRADER);

    // Cancel resting order
    bool cancel_order(OrderId id);

    // Market data queries
    Price best_bid() const;
    Price best_ask() const;
    Quantity bid_quantity_at(Price price) const;
    Quantity ask_quantity_at(Price price) const;

private:
    // Pre-allocated pools
    std::unique_ptr<std::array<Order, MAX_ORDERS>> order_pool_;
    std::unique_ptr<std::array<PriceLevel, MAX_PRICE_LEVELS>> level_pool_;

    // Free lists
    Order* free_orders_;
    PriceLevel* free_levels_;

    // Order lookup
    std::unique_ptr<std::array<Order*, MAX_ORDERS>> order_index_;

    // Book sides
    BidSide bids_;
    AskSide asks_;

    // Trade callback
    TradeCallback trade_callback_;

    // Timestamp counter
    Timestamp current_timestamp_;

    // Pool management - Orders
    Order* allocate_order();
    void deallocate_order(Order* order);

    // Pool management - Price Levels
    PriceLevel* allocate_level();
    void deallocate_level(PriceLevel* level);

    // Matching logic
    Quantity try_match_buy(Order* order);
    Quantity try_match_sell(Order* order);
    void execute_trade(Order* aggressive, Order* passive, Quantity qty);

    // Order placement
    void add_to_book(Order* order);
    void add_order_to_level(Order* order, PriceLevel* level);
    void remove_order_from_level(Order* order, PriceLevel* level);

    // Self-trade prevention
    bool would_self_trade(const Order* aggressive, const Order* passive) const;

    // Order index management
    __attribute__((always_inline))
    void clear_order_index(OrderId id) {
        if (id < MAX_ORDERS) {
            (*order_index_)[id] = nullptr;
        }
    }
};

}  // namespace hft
