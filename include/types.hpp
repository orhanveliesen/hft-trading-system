#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace hft {

// Fixed-point price: 4 decimal places (e.g., 12345 = $1.2345)
using Price = uint32_t;
using Quantity = uint32_t;
using OrderId = uint64_t;
using TraderId = uint32_t;
using Symbol = uint32_t; // Numeric symbol ID for speed
using Timestamp = uint64_t;

// Risk management types (signed for negative values: short positions, losses)
using Position = int64_t; // Net position (negative = short)
using PnL = int64_t;      // Profit/Loss (negative = loss)
using Notional = int64_t; // Notional value (position * price)
using Capital = int64_t;  // Capital/Equity

constexpr Price INVALID_PRICE = std::numeric_limits<Price>::max();
constexpr OrderId INVALID_ORDER_ID = 0;
constexpr TraderId NO_TRADER = 0;

// Order pool limits
constexpr size_t MAX_ORDERS = 1'000'000;
constexpr size_t MAX_PRICE_LEVELS = 100'000;

// Order ID validation
__attribute__((always_inline)) inline bool is_valid_order_id(OrderId id) {
    return id != INVALID_ORDER_ID && id < MAX_ORDERS;
}

// Matching result signals
constexpr Quantity FULLY_FILLED = 0;         // Order completely matched
constexpr Quantity CANCELLED_SELF_TRADE = 0; // Cancelled due to self-trade prevention

enum class Side : uint8_t { Buy = 0, Sell = 1 };

// Order operation result
enum class OrderResult : uint8_t {
    Success = 0,
    PoolExhausted,     // No orders available in pool
    InvalidOrderId,    // Order ID out of range or invalid
    InvalidPrice,      // Price out of valid range
    InvalidQuantity,   // Zero or negative quantity
    OrderNotFound,     // Order not found for cancel/modify
    SystemHalted,      // Trading system is halted
    DuplicateOrderId,  // Order ID already exists
    RateLimitExceeded, // Trader exceeded rate limit
    MaxOrdersExceeded  // Trader has too many active orders
};

inline const char* order_result_to_string(OrderResult result) {
    switch (result) {
    case OrderResult::Success:
        return "Success";
    case OrderResult::PoolExhausted:
        return "PoolExhausted";
    case OrderResult::InvalidOrderId:
        return "InvalidOrderId";
    case OrderResult::InvalidPrice:
        return "InvalidPrice";
    case OrderResult::InvalidQuantity:
        return "InvalidQuantity";
    case OrderResult::OrderNotFound:
        return "OrderNotFound";
    case OrderResult::SystemHalted:
        return "SystemHalted";
    case OrderResult::DuplicateOrderId:
        return "DuplicateOrderId";
    case OrderResult::RateLimitExceeded:
        return "RateLimitExceeded";
    case OrderResult::MaxOrdersExceeded:
        return "MaxOrdersExceeded";
    default:
        return "Unknown";
    }
}

struct Order {
    OrderId id;
    TraderId trader_id;
    Timestamp timestamp;
    Symbol symbol;
    Price price;
    Quantity quantity;
    Side side;

    // Intrusive list pointers (for O(1) removal)
    Order* prev;
    Order* next;

    // Initialize order - no allocation, just field assignment
    __attribute__((always_inline)) void init(OrderId id_, TraderId trader_, Timestamp ts_, Symbol sym_, Price price_,
                                             Quantity qty_, Side side_) {
        id = id_;
        trader_id = trader_;
        timestamp = ts_;
        symbol = sym_;
        price = price_;
        quantity = qty_;
        side = side_;
        prev = nullptr;
        next = nullptr;
    }

    // Reset for reuse (when returning to pool)
    __attribute__((always_inline)) void reset() {
        prev = nullptr;
        next = nullptr;
    }

    // Quantity operations (encapsulated for clarity)
    __attribute__((always_inline)) void reduce_quantity(Quantity amount) { quantity -= amount; }

    __attribute__((always_inline)) bool is_fully_filled() const { return quantity == 0; }
};

// Trade execution report
struct Trade {
    OrderId aggressive_order_id;
    OrderId passive_order_id;
    TraderId aggressive_trader_id;
    TraderId passive_trader_id;
    Price price;
    Quantity quantity;
    Side aggressor_side;
    Timestamp timestamp;
};

// Price level: all orders at same price
struct PriceLevel {
    Price price;
    Quantity total_quantity;
    Order* head;
    Order* tail;

    // For price level list
    PriceLevel* prev;
    PriceLevel* next;

    // Quantity management
    __attribute__((always_inline)) void reduce_quantity(Quantity amount) { total_quantity -= amount; }

    __attribute__((always_inline)) void add_quantity(Quantity amount) { total_quantity += amount; }

    __attribute__((always_inline)) bool is_empty() const { return total_quantity == 0; }
};

} // namespace hft
