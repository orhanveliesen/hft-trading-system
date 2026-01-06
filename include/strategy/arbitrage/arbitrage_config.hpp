#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace hft {
namespace strategy {
namespace arbitrage {

/**
 * Configuration for a single triangular arbitrage relationship
 */
struct TriangularArbConfig {
    std::string leg1;  // e.g., "BTC/USDT"
    std::string leg2;  // e.g., "ETH/BTC"
    std::string leg3;  // e.g., "ETH/USDT"

    // Minimum spread to trigger (percentage, 0.001 = 0.1%)
    double min_spread_pct = 0.001;

    // Maximum position size per leg
    double max_quantity = 1.0;

    // Enable/disable this relationship
    bool enabled = true;
};

/**
 * Global arbitrage configuration
 */
struct ArbitrageConfig {
    // Auto-detect triangular relationships from available symbols
    bool auto_detect = true;

    // Default minimum spread for auto-detected relationships
    double default_min_spread_pct = 0.001;

    // Default max quantity for auto-detected relationships
    double default_max_quantity = 1.0;

    // Manually configured relationships (override auto-detect)
    std::vector<TriangularArbConfig> manual_configs;

    // Symbols to exclude from auto-detection
    std::vector<std::string> excluded_symbols;

    // Quote currencies to prioritize (for triangular base)
    std::vector<std::string> priority_quotes = {"USDT", "USDC", "BTC", "ETH"};

    // Maximum number of auto-detected relationships (to limit overhead)
    size_t max_auto_relationships = 100;

    // Minimum liquidity (24h volume) to consider a pair (0 = no filter)
    double min_liquidity = 0.0;

    // Enable logging of detected opportunities
    bool log_opportunities = false;

    // Cooldown between executions on same relationship (microseconds)
    uint64_t execution_cooldown_us = 1000000;  // 1 second default
};

/**
 * Runtime state for a triangular relationship
 */
struct TriangularArbState {
    // Leg prices (updated on market data)
    double leg1_bid = 0.0;
    double leg1_ask = 0.0;
    double leg2_bid = 0.0;
    double leg2_ask = 0.0;
    double leg3_bid = 0.0;
    double leg3_ask = 0.0;

    // Computed spreads
    double forward_spread = 0.0;   // Buy path
    double reverse_spread = 0.0;   // Sell path

    // Last execution timestamp
    uint64_t last_execution_ns = 0;

    // Statistics
    uint64_t opportunities_detected = 0;
    uint64_t opportunities_executed = 0;
    double total_profit = 0.0;

    bool has_all_prices() const {
        return leg1_bid > 0 && leg1_ask > 0 &&
               leg2_bid > 0 && leg2_ask > 0 &&
               leg3_bid > 0 && leg3_ask > 0;
    }
};

}  // namespace arbitrage
}  // namespace strategy
}  // namespace hft
