#pragma once

#include "../types.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace hft {
namespace strategy {

struct MarketMakerConfig {
    uint32_t spread_bps = 10;    // Spread in basis points (10 = 0.1%)
    Quantity quote_size = 100;   // Default quote size
    int64_t max_position = 1000; // Position limit
    double skew_factor = 0.5;    // How much to skew quotes based on position
};

struct Quote {
    bool has_bid = false;
    bool has_ask = false;
    Price bid_price = 0;
    Price ask_price = 0;
    Quantity bid_size = 0;
    Quantity ask_size = 0;
};

// Simple market making strategy
// - Quotes two-sided around mid price
// - Skews quotes based on inventory
// - Reduces size when near position limits
class MarketMaker {
public:
    explicit MarketMaker(const MarketMakerConfig& config) : config_(config) {}

    // Generate quotes based on mid price and current position
    Quote generate_quotes(Price mid_price, int64_t position) const {
        Quote quote;

        // Calculate half-spread in price units
        // spread_bps is in basis points (1 bp = 0.01%)
        // half_spread = mid_price * (spread_bps / 2) / 10000
        Price half_spread = (mid_price * config_.spread_bps) / 20000;
        if (half_spread == 0)
            half_spread = 1; // Minimum 1 tick

        // Calculate skew based on position
        // Positive position -> lower bid (less willing to buy)
        // Negative position -> higher ask (less willing to sell)
        double position_ratio = static_cast<double>(position) / config_.max_position;
        Price skew = static_cast<Price>(half_spread * position_ratio * config_.skew_factor);

        // Calculate prices
        quote.bid_price = mid_price - half_spread - skew;
        quote.ask_price = mid_price + half_spread - skew;

        // Calculate sizes based on position limits
        int64_t room_to_buy = config_.max_position - position;
        int64_t room_to_sell = config_.max_position + position;

        quote.bid_size =
            static_cast<Quantity>(std::min(static_cast<int64_t>(config_.quote_size), std::max(0L, room_to_buy)));
        quote.ask_size =
            static_cast<Quantity>(std::min(static_cast<int64_t>(config_.quote_size), std::max(0L, room_to_sell)));

        quote.has_bid = quote.bid_size > 0;
        quote.has_ask = quote.ask_size > 0;

        return quote;
    }

    const MarketMakerConfig& config() const { return config_; }

private:
    MarketMakerConfig config_;
};

} // namespace strategy
} // namespace hft
