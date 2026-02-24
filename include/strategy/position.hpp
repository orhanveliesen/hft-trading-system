#pragma once

#include "../types.hpp"

#include <cstdint>
#include <cstdlib>

namespace hft {
namespace strategy {

// Tracks position and P&L for a single symbol
// All values in fixed-point (4 decimal places for price)
class PositionTracker {
public:
    PositionTracker() : position_(0), avg_price_(0), realized_pnl_(0), total_bought_(0), total_sold_(0) {}

    // Called when a fill is received
    void on_fill(Side side, Quantity qty, Price price) {
        int64_t signed_qty = static_cast<int64_t>(qty);

        if (side == Side::Buy) {
            handle_buy(signed_qty, price);
            total_bought_ += qty;
        } else {
            handle_sell(signed_qty, price);
            total_sold_ += qty;
        }
    }

    // Current position (positive = long, negative = short)
    int64_t position() const { return position_; }

    // Average entry price
    Price avg_price() const { return avg_price_; }

    // Realized P&L (in price units)
    int64_t realized_pnl() const { return realized_pnl_; }

    // Unrealized P&L at given mark price
    int64_t unrealized_pnl(Price mark_price) const {
        if (position_ == 0)
            return 0;
        return position_ * (static_cast<int64_t>(mark_price) - static_cast<int64_t>(avg_price_));
    }

    // Total P&L
    int64_t total_pnl(Price mark_price) const { return realized_pnl_ + unrealized_pnl(mark_price); }

    bool is_flat() const { return position_ == 0; }
    bool is_long() const { return position_ > 0; }
    bool is_short() const { return position_ < 0; }

    uint64_t total_bought() const { return total_bought_; }
    uint64_t total_sold() const { return total_sold_; }

    void reset() {
        position_ = 0;
        avg_price_ = 0;
        realized_pnl_ = 0;
        total_bought_ = 0;
        total_sold_ = 0;
    }

private:
    void handle_buy(int64_t qty, Price price) {
        if (position_ >= 0) {
            // Adding to long or opening long
            int64_t new_position = position_ + qty;
            // Weighted average: (old_pos * old_price + qty * price) / new_pos
            if (new_position > 0) {
                avg_price_ = static_cast<Price>((position_ * avg_price_ + qty * price) / new_position);
            }
            position_ = new_position;
        } else {
            // Covering short
            int64_t cover_qty = std::min(qty, -position_);
            int64_t remaining = qty - cover_qty;

            // Realize P&L on covered portion
            // Short sold at avg_price_, buying back at price
            realized_pnl_ += cover_qty * (static_cast<int64_t>(avg_price_) - static_cast<int64_t>(price));

            position_ += cover_qty;

            // If we bought more than our short, we're now long
            if (remaining > 0) {
                position_ = remaining;
                avg_price_ = price;
            } else if (position_ == 0) {
                avg_price_ = 0;
            }
        }
    }

    void handle_sell(int64_t qty, Price price) {
        if (position_ <= 0) {
            // Adding to short or opening short
            int64_t new_position = position_ - qty;
            if (new_position < 0) {
                int64_t abs_old = -position_;
                int64_t abs_new = -new_position;
                avg_price_ = static_cast<Price>((abs_old * avg_price_ + qty * price) / abs_new);
            }
            position_ = new_position;
        } else {
            // Closing long
            int64_t close_qty = std::min(qty, position_);
            int64_t remaining = qty - close_qty;

            // Realize P&L on closed portion
            // Bought at avg_price_, selling at price
            realized_pnl_ += close_qty * (static_cast<int64_t>(price) - static_cast<int64_t>(avg_price_));

            position_ -= close_qty;

            // If we sold more than our long, we're now short
            if (remaining > 0) {
                position_ = -remaining;
                avg_price_ = price;
            } else if (position_ == 0) {
                avg_price_ = 0;
            }
        }
    }

    int64_t position_;
    Price avg_price_;
    int64_t realized_pnl_;
    uint64_t total_bought_;
    uint64_t total_sold_;
};

} // namespace strategy
} // namespace hft
