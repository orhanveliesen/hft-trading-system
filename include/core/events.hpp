#pragma once

#include "types.hpp"

#include <cstdint>
#include <string>

namespace core {

using hft::OrderId;
using hft::Price;
using hft::Quantity;
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

// ============================================================================
// Spot Market Events (Phase 5.0)
// ============================================================================

/**
 * @brief Spot market buy order (immediate execution)
 */
struct SpotBuyEvent {
    Symbol symbol;
    Quantity qty;
    double strength;    // Signal strength (0.0 - 1.0)
    std::string reason; // Human-readable reason for debugging
    uint64_t timestamp_ns;
};

/**
 * @brief Spot market sell order (immediate execution)
 */
struct SpotSellEvent {
    Symbol symbol;
    Quantity qty;
    double strength;
    std::string reason;
    uint64_t timestamp_ns;
};

/**
 * @brief Spot limit buy order (passive order at specific price)
 */
struct SpotLimitBuyEvent {
    Symbol symbol;
    Quantity qty;
    Price limit_price;
    double strength;   // Signal strength
    double exec_score; // Execution score (higher = prefer limit over market)
    std::string reason;
    uint64_t timestamp_ns;
};

/**
 * @brief Spot limit sell order (passive order at specific price)
 */
struct SpotLimitSellEvent {
    Symbol symbol;
    Quantity qty;
    Price limit_price;
    double strength;
    double exec_score;
    std::string reason;
    uint64_t timestamp_ns;
};

/**
 * @brief Cancel a pending limit order (or all orders for a symbol)
 */
struct LimitCancelEvent {
    Symbol symbol;
    OrderId order_id;   // 0 = cancel all orders for symbol
    std::string reason; // "timeout", "signal_reversal", etc.
    uint64_t timestamp_ns;
};

// ============================================================================
// Futures Market Events (Phase 5.1 - Defined now for type registration)
// ============================================================================

/**
 * @brief Futures market buy order (open long or reduce short)
 */
struct FuturesBuyEvent {
    Symbol symbol;
    Quantity qty;
    double strength;
    std::string reason;
    uint64_t timestamp_ns;
};

/**
 * @brief Futures market sell order (open short or reduce long)
 */
struct FuturesSellEvent {
    Symbol symbol;
    Quantity qty;
    double strength;
    std::string reason;
    uint64_t timestamp_ns;
};

/**
 * @brief Close long futures position (full or partial)
 */
struct FuturesCloseLongEvent {
    Symbol symbol;
    Quantity qty; // 0 = close full position
    std::string reason;
    uint64_t timestamp_ns;
};

/**
 * @brief Close short futures position (full or partial)
 */
struct FuturesCloseShortEvent {
    Symbol symbol;
    Quantity qty; // 0 = close full position
    std::string reason;
    uint64_t timestamp_ns;
};

} // namespace core
