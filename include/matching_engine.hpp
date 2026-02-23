#pragma once

/**
 * MatchingEngine - Order matching with price-time priority
 *
 * Wraps order book and adds matching logic for exchange simulation.
 *
 * NOTE: Header-only by design for HFT performance.
 * All methods inline to eliminate function call overhead on hot path.
 */

#include "book_side.hpp"
#include "types.hpp"

#include <array>
#include <functional>
#include <memory>

namespace hft {

class MatchingEngine {
public:
    using TradeCallback = std::function<void(const Trade&)>;

    inline MatchingEngine(Price base_price, size_t price_range)
        : order_pool_(std::make_unique<std::array<Order, MAX_ORDERS>>()),
          level_pool_(std::make_unique<std::array<PriceLevel, MAX_PRICE_LEVELS>>()), free_orders_(nullptr),
          free_levels_(nullptr), order_index_(std::make_unique<std::array<Order*, MAX_ORDERS>>()),
          bids_(base_price, price_range), asks_(base_price, price_range), trade_callback_(nullptr),
          current_timestamp_(0) {
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

    // Set callback for trade notifications
    inline void set_trade_callback(TradeCallback callback) { trade_callback_ = std::move(callback); }

    // Add order - may trigger matching
    inline OrderResult add_order(OrderId id, Side side, Price price, Quantity quantity,
                                 TraderId trader_id = NO_TRADER) {
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

        ++current_timestamp_;
        order->init(id, trader_id, current_timestamp_, 0, price, quantity, side);

        // Index the order
        (*order_index_)[id] = order;

        // Try to match
        Quantity remaining = quantity;
        if (side == Side::Buy) {
            remaining = try_match_buy(order);
        } else {
            remaining = try_match_sell(order);
        }

        // Add remaining to book
        if (remaining > 0) {
            order->quantity = remaining;
            add_to_book(order);
        } else {
            // Fully matched - deallocate
            clear_order_index(id);
            deallocate_order(order);
        }

        return OrderResult::Success;
    }

    // Cancel resting order
    inline bool cancel_order(OrderId id) {
        if (!is_valid_order_id(id))
            return false;

        Order* order = (*order_index_)[id];
        if (!order)
            return false;

        PriceLevel* level =
            (order->side == Side::Buy) ? bids_.find_level(order->price) : asks_.find_level(order->price);

        if (level) {
            remove_order_from_level(order, level);
            PriceLevel* removed =
                (order->side == Side::Buy) ? bids_.remove_level_if_empty(level) : asks_.remove_level_if_empty(level);
            if (removed) {
                deallocate_level(removed);
            }
        }

        clear_order_index(id);
        deallocate_order(order);
        return true;
    }

    // Market data queries
    inline Price best_bid() const { return bids_.best_price(); }

    inline Price best_ask() const { return asks_.best_price(); }

    inline Quantity bid_quantity_at(Price price) const { return bids_.quantity_at(price); }

    inline Quantity ask_quantity_at(Price price) const { return asks_.quantity_at(price); }

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
    inline Order* allocate_order() {
        if (!free_orders_)
            return nullptr;
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

    // Pool management - Price Levels
    inline PriceLevel* allocate_level() {
        if (!free_levels_)
            return nullptr;
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

    // Matching logic
    inline Quantity try_match_buy(Order* order) {
        Quantity remaining = order->quantity;
        Price limit_price = order->price;

        // Match against asks (lowest first)
        while (remaining > 0) {
            Price best_ask_price = asks_.best_price();
            if (best_ask_price == INVALID_PRICE || best_ask_price > limit_price) {
                break; // No more matchable asks
            }

            PriceLevel* level = asks_.find_level(best_ask_price);
            if (!level || !level->head)
                break;

            Order* passive = level->head;

            // Self-trade prevention
            if (would_self_trade(order, passive)) {
                return CANCELLED_SELF_TRADE;
            }

            // Execute
            Quantity fill_qty = std::min(remaining, passive->quantity);
            execute_trade(order, passive, fill_qty);

            remaining -= fill_qty;
            passive->reduce_quantity(fill_qty);
            level->reduce_quantity(fill_qty);

            // Remove fully filled passive order
            if (passive->is_fully_filled()) {
                remove_order_from_level(passive, level);
                clear_order_index(passive->id);
                deallocate_order(passive);
                PriceLevel* removed = asks_.remove_level_if_empty(level);
                if (removed) {
                    deallocate_level(removed);
                }
            }
        }

        return remaining;
    }

    inline Quantity try_match_sell(Order* order) {
        Quantity remaining = order->quantity;
        Price limit_price = order->price;

        // Match against bids (highest first)
        while (remaining > 0) {
            Price best_bid_price = bids_.best_price();
            if (best_bid_price == INVALID_PRICE || best_bid_price < limit_price) {
                break; // No more matchable bids
            }

            PriceLevel* level = bids_.find_level(best_bid_price);
            if (!level || !level->head)
                break;

            Order* passive = level->head;

            // Self-trade prevention
            if (would_self_trade(order, passive)) {
                return CANCELLED_SELF_TRADE;
            }

            // Execute
            Quantity fill_qty = std::min(remaining, passive->quantity);
            execute_trade(order, passive, fill_qty);

            remaining -= fill_qty;
            passive->reduce_quantity(fill_qty);
            level->reduce_quantity(fill_qty);

            // Remove fully filled passive order
            if (passive->is_fully_filled()) {
                remove_order_from_level(passive, level);
                clear_order_index(passive->id);
                deallocate_order(passive);
                PriceLevel* removed = bids_.remove_level_if_empty(level);
                if (removed) {
                    deallocate_level(removed);
                }
            }
        }

        return remaining;
    }

    inline void execute_trade(Order* aggressive, Order* passive, Quantity qty) {
        if (!trade_callback_)
            return;

        Trade trade{aggressive->id,
                    passive->id,
                    aggressive->trader_id,
                    passive->trader_id,
                    passive->price, // Trade at passive order's price
                    qty,
                    aggressive->side,
                    current_timestamp_};

        trade_callback_(trade);
    }

    // Self-trade prevention
    inline bool would_self_trade(const Order* aggressive, const Order* passive) const {
        // Self-trade if same non-zero trader
        return aggressive->trader_id != NO_TRADER && aggressive->trader_id == passive->trader_id;
    }

    // Order placement
    inline void add_to_book(Order* order) {
        PriceLevel* level;

        if (order->side == Side::Buy) {
            level = bids_.find_level(order->price);
            if (!level) {
                level = allocate_level();
                if (!level)
                    return; // Pool exhausted
                level->price = order->price;
                bids_.insert_level(level);
            }
        } else {
            level = asks_.find_level(order->price);
            if (!level) {
                level = allocate_level();
                if (!level)
                    return; // Pool exhausted
                level->price = order->price;
                asks_.insert_level(level);
            }
        }

        add_order_to_level(order, level);
    }

    inline void add_order_to_level(Order* order, PriceLevel* level) {
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
    __attribute__((always_inline)) void clear_order_index(OrderId id) {
        if (id < MAX_ORDERS) {
            (*order_index_)[id] = nullptr;
        }
    }
};

} // namespace hft
