#pragma once

#include "types.hpp"
#include "book_side.hpp"
#include <array>
#include <memory>

namespace hft {

/**
 * OrderBook - Full order book with individual order tracking
 *
 * Use cases:
 * - Exchange/matching engine implementation
 * - Market making (need to track own orders by ID)
 * - Backtesting with full order-level simulation
 * - Research requiring complete order flow reconstruction
 *
 * NOT optimal for:
 * - Aggressive trading (use TopOfBook instead - 88 bytes vs 160MB)
 * - Signal generation (only need top 5 levels)
 *
 * Memory: ~160MB per symbol (pre-allocated pools)
 * Operations: O(1) add/cancel/execute via intrusive linked lists
 *
 * For aggressive trading, prefer TopOfBook (include/top_of_book.hpp)
 */
class OrderBook {
public:
    // Default values for backward compatibility
    static constexpr size_t DEFAULT_PRICE_RANGE = 200'000;
    static constexpr Price DEFAULT_BASE_PRICE = 90'000;

    OrderBook();
    explicit OrderBook(Price base_price);
    OrderBook(Price base_price, size_t price_range);

    // Core operations - all O(1) on hot path
    // Returns OrderResult indicating success or failure reason
    OrderResult add_order(OrderId id, Side side, Price price, Quantity quantity);
    bool cancel_order(OrderId id);
    bool execute_order(OrderId id, Quantity quantity);

    // Queries - delegated to BookSide
    Price best_bid() const;
    Price best_ask() const;
    Quantity bid_quantity_at(Price price) const;
    Quantity ask_quantity_at(Price price) const;

private:
    // Pre-allocated pools
    std::unique_ptr<std::array<Order, MAX_ORDERS>> order_pool_;
    std::unique_ptr<std::array<PriceLevel, MAX_PRICE_LEVELS>> level_pool_;

    // Free lists for pool management
    Order* free_orders_;
    PriceLevel* free_levels_;

    // Order lookup: id -> order pointer
    std::unique_ptr<std::array<Order*, MAX_ORDERS>> order_index_;

    // Bid and Ask sides - template based, zero overhead
    BidSide bids_;
    AskSide asks_;

    // Order pool management
    Order* allocate_order();
    void deallocate_order(Order* order);

    // Level pool management
    PriceLevel* allocate_level();
    void deallocate_level(PriceLevel* level);

    // Order-level list operations
    void add_order_to_level(Order* order, PriceLevel* level);
    void remove_order_from_level(Order* order, PriceLevel* level);

    // Order index management
    __attribute__((always_inline))
    void clear_order_index(OrderId id) {
        if (id < MAX_ORDERS) {
            (*order_index_)[id] = nullptr;
        }
    }
};

}  // namespace hft
