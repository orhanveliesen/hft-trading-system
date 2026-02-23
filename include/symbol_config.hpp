#pragma once

#include <cstdint>
#include <limits>
#include <string>

namespace hft {

// Configuration for a tradeable symbol
struct SymbolConfig {
    // Identity
    std::string symbol; // e.g., "AAPL", "TSLA"

    // Order book configuration
    uint32_t base_price;  // Base price for O(1) lookup array
    uint32_t price_range; // Number of price ticks to cover

    // Market making configuration
    bool enable_market_making = false;
    int spread_bps = 10;         // Spread in basis points
    uint32_t quote_size = 100;   // Size per quote
    int32_t max_position = 1000; // Maximum position size

    // Risk configuration
    int64_t max_loss = std::numeric_limits<int64_t>::max(); // Max loss before halt

    SymbolConfig() = default;

    SymbolConfig(const std::string& sym, uint32_t base, uint32_t range)
        : symbol(sym), base_price(base), price_range(range) {}

    // Builder pattern for cleaner configuration
    SymbolConfig& with_market_making(int spread, uint32_t size, int32_t max_pos) {
        enable_market_making = true;
        spread_bps = spread;
        quote_size = size;
        max_position = max_pos;
        return *this;
    }

    SymbolConfig& with_risk_limit(int64_t loss_limit) {
        max_loss = loss_limit;
        return *this;
    }
};

// Utility: Trim ITCH-style padded symbol (8 chars, space-padded)
inline std::string trim_symbol(const char* data, size_t len) {
    size_t end = len;
    while (end > 0 && data[end - 1] == ' ') {
        --end;
    }
    return std::string(data, end);
}

} // namespace hft
