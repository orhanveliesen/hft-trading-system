#pragma once

#include "../types.hpp"

#include <cmath>

namespace hft {
namespace strategy {

/**
 * Trading Position
 *
 * Generic position tracking used across all trading contexts:
 * - Backtesting
 * - Paper trading
 * - Live trading
 */
struct TradingPosition {
    double quantity = 0;  // Positive = long, negative = short
    double avg_price = 0; // Average entry price (as double for precision)
    Timestamp entry_time = 0;

    bool is_flat() const { return quantity == 0; }
    bool is_long() const { return quantity > 0; }
    bool is_short() const { return quantity < 0; }
    double size() const { return std::abs(quantity); }

    // Calculate unrealized P&L at given price
    double unrealized_pnl(double current_price) const {
        if (is_flat())
            return 0;
        if (is_long()) {
            return (current_price - avg_price) * quantity;
        } else {
            return (avg_price - current_price) * (-quantity);
        }
    }

    // Calculate P&L percentage
    double pnl_percent(double current_price) const {
        if (is_flat() || avg_price == 0)
            return 0;
        if (is_long()) {
            return (current_price - avg_price) / avg_price * 100;
        } else {
            return (avg_price - current_price) / avg_price * 100;
        }
    }
};

} // namespace strategy
} // namespace hft
