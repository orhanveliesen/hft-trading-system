#pragma once

#include "../types.hpp" // For hft::Side

#include <cstdint>
#include <cstring>

namespace hft {
namespace ipc {

/**
 * Execution type - what happened to the order
 * Matches FIX protocol ExecType (Tag 150)
 */
enum class ExecType : uint8_t {
    New = 0,       // Order accepted
    Trade = 1,     // Partial or full fill
    Cancelled = 2, // Order cancelled
    Rejected = 3,  // Order rejected
    Expired = 4    // Order expired (GTD/IOC)
};

/**
 * Order status - current state of the order
 * Matches FIX protocol OrdStatus (Tag 39)
 */
enum class OrderStatus : uint8_t {
    New = 0, // Order acknowledged
    PartiallyFilled = 1,
    Filled = 2,
    Cancelled = 3,
    Rejected = 4,
    Expired = 5
};

/**
 * Order type
 */
enum class OrderType : uint8_t { Market = 0, Limit = 1 };

// Use hft::Side from types.hpp (no duplicate definition)

/**
 * ExecutionReport - Message from exchange about order status
 *
 * This struct represents a standardized execution report that both
 * PaperExchange and real exchange adapters produce. TradingEngine
 * processes these without knowing the source.
 *
 * Design:
 * - POD struct for lock-free IPC
 * - Fixed 64 bytes (1 cache line)
 * - No dynamic allocation
 * - Commission included (from exchange, not calculated)
 */
struct alignas(64) ExecutionReport {
    // Identification (16 bytes)
    char symbol[8];    // Symbol string (e.g., "BTCUSDT\0")
    uint64_t order_id; // Internal order ID

    // Execution details (24 bytes)
    double filled_qty;   // Quantity filled in this execution
    double filled_price; // Execution price
    double commission;   // Commission charged (from exchange)

    // Timestamps (16 bytes)
    uint64_t order_timestamp_ns; // When order was placed
    uint64_t exec_timestamp_ns;  // When this execution occurred

    // Status (4 bytes)
    ExecType exec_type;   // What happened
    OrderStatus status;   // Current order state
    OrderType order_type; // Market or Limit
    hft::Side side;       // Buy or Sell

    // Cumulative info (8 bytes) - total fills for this order
    double cum_qty; // Total quantity filled so far

    // Commission asset (8 bytes)
    char commission_asset[8]; // Asset used for commission (e.g., "USDT\0")

    // Reject/Error info (24 bytes)
    char reject_reason[24]; // Reason if rejected

    // Helper methods
    void clear() { std::memset(this, 0, sizeof(ExecutionReport)); }

    bool is_fill() const { return exec_type == ExecType::Trade; }

    bool is_final() const {
        return status == OrderStatus::Filled || status == OrderStatus::Cancelled || status == OrderStatus::Rejected ||
               status == OrderStatus::Expired;
    }

    bool is_buy() const { return side == hft::Side::Buy; }
    bool is_sell() const { return side == hft::Side::Sell; }

    // Factory methods for common reports
    static ExecutionReport market_fill(const char* sym, uint64_t oid, hft::Side s, double qty, double price,
                                       double comm, uint64_t timestamp) {
        ExecutionReport r;
        r.clear();
        std::strncpy(r.symbol, sym, sizeof(r.symbol) - 1);
        r.order_id = oid;
        r.side = s;
        r.order_type = OrderType::Market;
        r.exec_type = ExecType::Trade;
        r.status = OrderStatus::Filled;
        r.filled_qty = qty;
        r.filled_price = price;
        r.cum_qty = qty;
        r.commission = comm;
        std::strncpy(r.commission_asset, "USDT", sizeof(r.commission_asset) - 1);
        r.order_timestamp_ns = timestamp;
        r.exec_timestamp_ns = timestamp;
        return r;
    }

    static ExecutionReport limit_accepted(const char* sym, uint64_t oid, hft::Side s, uint64_t timestamp) {
        ExecutionReport r;
        r.clear();
        std::strncpy(r.symbol, sym, sizeof(r.symbol) - 1);
        r.order_id = oid;
        r.side = s;
        r.order_type = OrderType::Limit;
        r.exec_type = ExecType::New;
        r.status = OrderStatus::New;
        r.order_timestamp_ns = timestamp;
        r.exec_timestamp_ns = timestamp;
        return r;
    }

    static ExecutionReport limit_fill(const char* sym, uint64_t oid, hft::Side s, double qty, double price, double comm,
                                      uint64_t order_ts, uint64_t exec_ts) {
        ExecutionReport r;
        r.clear();
        std::strncpy(r.symbol, sym, sizeof(r.symbol) - 1);
        r.order_id = oid;
        r.side = s;
        r.order_type = OrderType::Limit;
        r.exec_type = ExecType::Trade;
        r.status = OrderStatus::Filled;
        r.filled_qty = qty;
        r.filled_price = price;
        r.cum_qty = qty;
        r.commission = comm;
        std::strncpy(r.commission_asset, "USDT", sizeof(r.commission_asset) - 1);
        r.order_timestamp_ns = order_ts;
        r.exec_timestamp_ns = exec_ts;
        return r;
    }

    static ExecutionReport rejected(const char* sym, uint64_t oid, hft::Side s, OrderType ot, const char* reason,
                                    uint64_t timestamp) {
        ExecutionReport r;
        r.clear();
        std::strncpy(r.symbol, sym, sizeof(r.symbol) - 1);
        r.order_id = oid;
        r.side = s;
        r.order_type = ot;
        r.exec_type = ExecType::Rejected;
        r.status = OrderStatus::Rejected;
        std::strncpy(r.reject_reason, reason, sizeof(r.reject_reason) - 1);
        r.order_timestamp_ns = timestamp;
        r.exec_timestamp_ns = timestamp;
        return r;
    }
};

// Verify size - must fit in cache line considerations
// Note: alignas(64) means actual struct size is padded to 64-byte boundary
static_assert(sizeof(ExecutionReport) == 128, "ExecutionReport size mismatch");
static_assert(alignof(ExecutionReport) == 64, "ExecutionReport must be cache-line aligned");

} // namespace ipc
} // namespace hft
