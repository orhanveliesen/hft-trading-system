#pragma once

/**
 * SymbolStrategy - Per-symbol strategy state for trading
 *
 * Tracks regime detection, technical indicators, and spread dynamics
 * for each symbol being traded. Uses fixed-size char array for ticker
 * to avoid std::string allocation in hot path.
 */

#include "../types.hpp"
#include "regime_detector.hpp"
#include "technical_indicators.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace hft {
namespace strategy {

struct SymbolStrategy {
    RegimeDetector regime{RegimeConfig{}};
    TechnicalIndicators indicators{TechnicalIndicators::Config{}};
    MarketRegime current_regime = MarketRegime::Unknown;
    Price last_mid = 0;
    uint64_t last_signal_time = 0;
    char ticker[16] = {0};  // Fixed size, no std::string allocation
    bool active = false;    // Is this slot in use?

    // Dynamic spread tracking (EMA of spread)
    double ema_spread_pct = 0.001;  // Start with 0.1% default
    static constexpr double SPREAD_ALPHA = 0.1;  // EMA decay

    void init(const std::string& symbol) {
        active = true;
        std::strncpy(ticker, symbol.c_str(), sizeof(ticker) - 1);
        ticker[sizeof(ticker) - 1] = '\0';
    }

    void update_spread(Price bid, Price ask) {
        if (bid > 0 && ask > bid) {
            double spread_pct = static_cast<double>(ask - bid) / static_cast<double>(bid);
            ema_spread_pct = SPREAD_ALPHA * spread_pct + (1.0 - SPREAD_ALPHA) * ema_spread_pct;
        }
    }

    // Threshold = 3x spread with 0.02% (2 bps) minimum floor
    // This ensures we only trade when expected profit > spread cost
    // Math: entry spread + exit spread = 2x spread, so need >2x to profit
    double buy_threshold() const {
        double threshold = ema_spread_pct * 3.0;
        return -std::max(threshold, 0.0002);  // At least -0.02%
    }
    double sell_threshold() const {
        double threshold = ema_spread_pct * 3.0;
        return std::max(threshold, 0.0002);   // At least +0.02%
    }
};

}  // namespace strategy
}  // namespace hft
