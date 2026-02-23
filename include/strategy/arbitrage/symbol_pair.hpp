#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hft {
namespace strategy {
namespace arbitrage {

/**
 * SymbolPair - Parsed trading pair
 *
 * Parses symbols like "BTC/USDT", "BTCUSDT", "ETH-BTC" into base/quote components.
 */
struct SymbolPair {
    std::string base;     // e.g., "BTC"
    std::string quote;    // e.g., "USDT"
    std::string original; // Original symbol string

    SymbolPair() = default;
    SymbolPair(std::string b, std::string q, std::string orig = "")
        : base(std::move(b)), quote(std::move(q)), original(std::move(orig)) {}

    // Reconstruct symbol in standard format
    std::string to_string() const { return base + "/" + quote; }

    bool operator==(const SymbolPair& other) const { return base == other.base && quote == other.quote; }

    bool operator!=(const SymbolPair& other) const { return !(*this == other); }

    bool is_valid() const { return !base.empty() && !quote.empty(); }

    /**
     * Parse a symbol string into base/quote pair
     *
     * Supports formats:
     *   - "BTC/USDT" (with separator)
     *   - "BTC-USDT" (with separator)
     *   - "BTC_USDT" (with separator)
     *   - "BTCUSDT"  (no separator, uses known quote currencies)
     */
    static std::optional<SymbolPair> parse(const std::string& symbol) {
        if (symbol.empty()) {
            return std::nullopt;
        }

        // Try separator-based parsing first
        for (char sep : {'/', '-', '_'}) {
            size_t pos = symbol.find(sep);
            if (pos != std::string::npos && pos > 0 && pos < symbol.size() - 1) {
                return SymbolPair{symbol.substr(0, pos), symbol.substr(pos + 1), symbol};
            }
        }

        // No separator - try known quote currencies
        static const std::vector<std::string> known_quotes = {"USDT", "USDC", "BUSD", "USD",  "EUR", "GBP",
                                                              "BTC",  "ETH",  "BNB",  "TUSD", "DAI"};

        std::string upper = symbol;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

        for (const auto& quote : known_quotes) {
            if (upper.size() > quote.size() && upper.substr(upper.size() - quote.size()) == quote) {
                std::string base = upper.substr(0, upper.size() - quote.size());
                return SymbolPair(base, quote, symbol);
            }
        }

        return std::nullopt;
    }

    /**
     * Check if two pairs share a common currency
     */
    static bool shares_currency(const SymbolPair& a, const SymbolPair& b) {
        return a.base == b.base || a.base == b.quote || a.quote == b.base || a.quote == b.quote;
    }

    /**
     * Get the common currency between two pairs (if any)
     */
    static std::optional<std::string> common_currency(const SymbolPair& a, const SymbolPair& b) {
        if (a.base == b.base)
            return a.base;
        if (a.base == b.quote)
            return a.base;
        if (a.quote == b.base)
            return a.quote;
        if (a.quote == b.quote)
            return a.quote;
        return std::nullopt;
    }
};

} // namespace arbitrage
} // namespace strategy
} // namespace hft
