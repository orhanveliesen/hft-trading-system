#pragma once

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
 *
 * NOTE: Header-only by design for HFT performance.
 * All methods inline to eliminate function call overhead on hot path.
 */

#include "types.hpp"
#include "book_side.hpp"
#include <array>
#include <memory>

namespace hft {

class OrderBook {
public:
    // Default values for backward compatibility
    static constexpr size_t DEFAULT_PRICE_RANGE = 200'000;
    static constexpr Price DEFAULT_BASE_PRICE = 90'000;

    inline OrderBook()
        : OrderBook(DEFAULT_BASE_PRICE, DEFAULT_PRICE_RANGE)
    {}

    inline explicit OrderBook(Price base_price)
        : OrderBook(base_price, DEFAULT_PRICE_RANGE)
    {}

    inline OrderBook(Price base_price, size_t price_range)
        : order_pool_(std::make_unique<std::array<Order, MAX_ORDERS>>())
        , level_pool_(std::make_unique<std::array<PriceLevel, MAX_PRICE_LEVELS>>())
        , free_orders_(nullptr)
        , free_levels_(nullptr)
        , order_index_(std::make_unique<std::array<Order*, MAX_ORDERS>>())
        , bids_(base_price, price_range)
        , asks_(base_price, price_range)
    {
        // Initialize order free list
        for (size_t i = 0; i < MAX_ORDERS - 1; ++i) {
            (*order_pool_)[i].next = &(*order_pool_)[i + 1];
        }
        (*order_pool_)[MAX_ORDERS - 1].next = nullptr;
        free_orders_ = &(*order_pool_)[0];

        // Initialize level free list
        for (size_t i = 0; i < MAX_PRICE_LEVELS - 1; ++i) {
            (*level_pool_)[i].next = &(*level_pool_)[i + 1];
        }
        (*level_pool_)[MAX_PRICE_LEVELS - 1].next = nullptr;
        free_levels_ = &(*level_pool_)[0];

        // Initialize order lookup
        order_index_->fill(nullptr);
    }

    // Core operations - all O(1) on hot path
    inline OrderResult add_order(OrderId id, Side side, Price price, Quantity quantity) {
        // Validate inputs
        if (!is_valid_order_id(id)) {
            return OrderResult::InvalidOrderId;
        }

        if (price == INVALID_PRICE || price == 0) {
            return OrderResult::InvalidPrice;
        }

        if (quantity == 0) {
            return OrderResult::InvalidQuantity;
        }

        // Check for duplicate order ID
        if ((*order_index_)[id] != nullptr) {
            return OrderResult::DuplicateOrderId;
        }

        // Allocate order from pool
        Order* order = allocate_order();
        if (!order) {
            return OrderResult::PoolExhausted;
        }

        order->init(id, NO_TRADER, 0, 0, price, quantity, side);

        // Index the order
        (*order_index_)[id] = order;

        // Get or create price level
        PriceLevel* level;

        if (side == Side::Buy) {
            level = bids_.find_level(price);
            if (!level) {
                level = allocate_level();
                if (!level) {
                    clear_order_index(id);
                    deallocate_order(order);
                    return OrderResult::PoolExhausted;
                }
                level->price = price;
                bids_.insert_level(level);
            }
        } else {
            level = asks_.find_level(price);
            if (!level) {
                level = allocate_level();
                if (!level) {
                    clear_order_index(id);
                    deallocate_order(order);
                    return OrderResult::PoolExhausted;
                }
                level->price = price;
                asks_.insert_level(level);
            }
        }

        add_order_to_level(order, level);

        return OrderResult::Success;
    }

    inline bool cancel_order(OrderId id) {
        if (!is_valid_order_id(id)) return false;

        Order* order = (*order_index_)[id];
        if (!order) return false;

        // Find the price level via BookSide - O(1)
        PriceLevel* level = (order->side == Side::Buy)
            ? bids_.find_level(order->price)
            : asks_.find_level(order->price);

        if (level) {
            remove_order_from_level(order, level);
            PriceLevel* removed = (order->side == Side::Buy)
                ? bids_.remove_level_if_empty(level)
                : asks_.remove_level_if_empty(level);
            if (removed) {
                deallocate_level(removed);
            }
        }

        // Remove from index and deallocate
        clear_order_index(id);
        deallocate_order(order);
        return true;
    }

    inline bool execute_order(OrderId id, Quantity quantity) {
        if (!is_valid_order_id(id)) return false;

        Order* order = (*order_index_)[id];
        if (!order) return false;

        // Find the price level via BookSide - O(1)
        PriceLevel* level = (order->side == Side::Buy)
            ? bids_.find_level(order->price)
            : asks_.find_level(order->price);

        if (!level) return false;

        if (quantity >= order->quantity) {
            // Full execution - remove order
            remove_order_from_level(order, level);
            PriceLevel* removed = (order->side == Side::Buy)
                ? bids_.remove_level_if_empty(level)
                : asks_.remove_level_if_empty(level);
            if (removed) {
                deallocate_level(removed);
            }
            clear_order_index(id);
            deallocate_order(order);
        } else {
            // Partial execution - reduce quantity
            order->reduce_quantity(quantity);
            level->reduce_quantity(quantity);
        }

        return true;
    }

    // Queries - delegated to BookSide
    inline Price best_bid() const {
        return bids_.best_price();
    }

    inline Price best_ask() const {
        return asks_.best_price();
    }

    inline Quantity bid_quantity_at(Price price) const {
        return bids_.quantity_at(price);
    }

    inline Quantity ask_quantity_at(Price price) const {
        return asks_.quantity_at(price);
    }

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
    inline Order* allocate_order() {
        if (!free_orders_) return nullptr;
        Order* order = free_orders_;
        free_orders_ = free_orders_->next;
        order->prev = nullptr;
        order->next = nullptr;
        return order;
    }

    inline void deallocate_order(Order* order) {
        order->next = free_orders_;
        free_orders_ = order;
    }

    // Level pool management
    inline PriceLevel* allocate_level() {
        if (!free_levels_) return nullptr;
        PriceLevel* level = free_levels_;
        free_levels_ = free_levels_->next;
        level->prev = nullptr;
        level->next = nullptr;
        level->head = nullptr;
        level->tail = nullptr;
        level->total_quantity = 0;
        return level;
    }

    inline void deallocate_level(PriceLevel* level) {
        level->next = free_levels_;
        free_levels_ = level;
    }

    // Order-level list operations
    inline void add_order_to_level(Order* order, PriceLevel* level) {
        // Add to tail (FIFO)
        order->prev = level->tail;
        order->next = nullptr;

        if (level->tail) {
            level->tail->next = order;
        } else {
            level->head = order;
        }
        level->tail = order;

        level->add_quantity(order->quantity);
    }

    inline void remove_order_from_level(Order* order, PriceLevel* level) {
        if (order->prev) {
            order->prev->next = order->next;
        } else {
            level->head = order->next;
        }

        if (order->next) {
            order->next->prev = order->prev;
        } else {
            level->tail = order->prev;
        }

        level->reduce_quantity(order->quantity);
    }

    // Order index management
    __attribute__((always_inline))
    void clear_order_index(OrderId id) {
        if (id < MAX_ORDERS) {
            (*order_index_)[id] = nullptr;
        }
    }
};

}  // namespace hft
