#pragma once

#include "types.hpp"

#include <cstdint>

namespace hft::core {

using hft::OrderId;
using hft::Price;
using hft::Side;
using hft::Symbol;

/**
 * @brief Action events published by StrategyEvaluator and consumed by execution engines
 *
 * These events represent trading actions (buy, sell, limit orders, cancellations).
 * Events are published to EventBus and handled synchronously by subscribers.
 *
 * Phase 5.0: Spot events + LimitCancelEvent
 * Phase 5.1: Futures events (defined here for type registration)
 */

/**
 * @brief Event type enumeration for array-indexed EventBus lookup
 *
 * Replaces hash table with O(1) constant-time array access.
 * Each event struct has a static constexpr type_id member.
 */
enum class EventType : uint8_t {
    SpotBuy = 0,
    SpotSell = 1,
    SpotLimitBuy = 2,
    SpotLimitSell = 3,
    LimitCancel = 4,
    FuturesBuy = 5,
    FuturesSell = 6,
    FuturesCloseLong = 7,
    FuturesCloseShort = 8,
    COUNT = 9 // Total number of event types
};

// ============================================================================
// Spot Market Events (Phase 5.0)
// ============================================================================

/**
 * @brief Spot market buy order (immediate execution)
 */
struct SpotBuyEvent {
    static constexpr EventType type_id = EventType::SpotBuy;

    Symbol symbol;
    double qty;         // Quantity (double for crypto fractional amounts)
    double strength;    // Signal strength (0.0 - 1.0)
    const char* reason; // Human-readable reason for debugging (no heap allocation)
    uint64_t timestamp_ns;
};

/**
 * @brief Spot market sell order (immediate execution)
 */
struct SpotSellEvent {
    static constexpr EventType type_id = EventType::SpotSell;

    Symbol symbol;
    double qty;
    double strength;
    const char* reason;
    uint64_t timestamp_ns;
};

/**
 * @brief Spot limit buy order (passive order at specific price)
 */
struct SpotLimitBuyEvent {
    static constexpr EventType type_id = EventType::SpotLimitBuy;

    Symbol symbol;
    double qty;
    Price limit_price;
    double strength;   // Signal strength
    double exec_score; // Execution score (higher = prefer limit over market)
    const char* reason;
    uint64_t timestamp_ns;
};

/**
 * @brief Spot limit sell order (passive order at specific price)
 */
struct SpotLimitSellEvent {
    static constexpr EventType type_id = EventType::SpotLimitSell;

    Symbol symbol;
    double qty;
    Price limit_price;
    double strength;
    double exec_score;
    const char* reason;
    uint64_t timestamp_ns;
};

/**
 * @brief Cancel a pending limit order (or all orders for a symbol)
 */
struct LimitCancelEvent {
    static constexpr EventType type_id = EventType::LimitCancel;

    Symbol symbol;
    OrderId order_id;   // 0 = cancel all orders for symbol
    const char* reason; // "timeout", "signal_reversal", etc.
    uint64_t timestamp_ns;
};

// ============================================================================
// Futures Market Events (Phase 5.1 - Defined now for type registration)
// ============================================================================

/**
 * @brief Futures market buy order (open long or reduce short)
 */
struct FuturesBuyEvent {
    static constexpr EventType type_id = EventType::FuturesBuy;

    Symbol symbol;
    double qty;
    double strength;
    const char* reason;
    uint64_t timestamp_ns;
};

/**
 * @brief Futures market sell order (open short or reduce long)
 */
struct FuturesSellEvent {
    static constexpr EventType type_id = EventType::FuturesSell;

    Symbol symbol;
    double qty;
    double strength;
    const char* reason;
    uint64_t timestamp_ns;
};

/**
 * @brief Close long futures position (full or partial)
 */
struct FuturesCloseLongEvent {
    static constexpr EventType type_id = EventType::FuturesCloseLong;

    Symbol symbol;
    double qty; // 0 = close full position
    const char* reason;
    uint64_t timestamp_ns;
};

/**
 * @brief Close short futures position (full or partial)
 */
struct FuturesCloseShortEvent {
    static constexpr EventType type_id = EventType::FuturesCloseShort;

    Symbol symbol;
    double qty; // 0 = close full position
    const char* reason;
    uint64_t timestamp_ns;
};

} // namespace hft::core
