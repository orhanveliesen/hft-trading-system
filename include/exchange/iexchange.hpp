#pragma once

#include "../types.hpp"
#include "../execution/execution_engine.hpp"
#include <functional>
#include <cstdint>

namespace hft {
namespace exchange {

/**
 * IExchange - Unified exchange interface for both paper and production
 *
 * This interface abstracts away the differences between:
 * - Paper trading (simulated fills, slippage simulation)
 * - Production trading (real API, real fills)
 *
 * All exchange adapters implement this interface, allowing the rest of
 * the system to work identically regardless of whether it's paper or prod.
 */
class IExchange : public execution::IExchangeAdapter {
public:
    using FillCallback = std::function<void(
        uint64_t order_id,
        const char* symbol_name,  // Symbol name directly, no ID conversion needed
        Side side,
        Quantity qty,
        Price fill_price,
        double commission
    )>;

    using SlippageCallback = std::function<void(double slippage_cost)>;

    ~IExchange() override = default;

    // =========================================================================
    // Order Operations (from IExchangeAdapter)
    // =========================================================================

    /// Send market order - fills immediately at current price + slippage
    uint64_t send_market_order(
        Symbol symbol, Side side, Quantity qty, Price expected_price
    ) override = 0;

    /// Send limit order - pending until price crosses limit
    uint64_t send_limit_order(
        Symbol symbol, Side side, Quantity qty, Price limit_price
    ) override = 0;

    /// Cancel a pending order
    bool cancel_order(uint64_t order_id) override = 0;

    /// Check if order is still pending
    bool is_order_pending(uint64_t order_id) const override = 0;

    // =========================================================================
    // Price Updates (for limit order matching)
    // =========================================================================

    /// Called on each price update to check for limit fills
    virtual void on_price_update(
        Symbol symbol,
        Price bid,
        Price ask,
        uint64_t timestamp_ns
    ) = 0;

    // =========================================================================
    // Callbacks
    // =========================================================================

    /// Set callback for when orders fill
    virtual void set_fill_callback(FillCallback cb) = 0;

    /// Set callback for slippage tracking (paper trading only)
    virtual void set_slippage_callback(SlippageCallback cb) = 0;

    // =========================================================================
    // Configuration
    // =========================================================================

    /// Set commission rate (decimal, e.g., 0.001 = 0.1%)
    virtual void set_commission_rate(double rate) = 0;

    /// Set slippage in basis points (paper only)
    virtual void set_slippage_bps(double bps) = 0;

    // =========================================================================
    // Statistics
    // =========================================================================

    /// Get count of pending orders
    virtual size_t pending_order_count() const = 0;

    /// Get total orders sent
    virtual uint64_t total_orders() const = 0;

    /// Get total fills received
    virtual uint64_t total_fills() const = 0;

    /// Get total slippage cost (paper only)
    virtual double total_slippage() const = 0;

    /// Get total commission paid
    virtual double total_commission() const = 0;
};

}  // namespace exchange
}  // namespace hft
