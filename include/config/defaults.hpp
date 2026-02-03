#pragma once

#include <cstdint>

/**
 * Centralized configuration defaults for the trading system.
 *
 * All default values are defined here to avoid duplication across:
 * - SharedConfig
 * - SymbolTuningConfig
 * - Portfolio
 * - Strategy configs
 *
 * Values are organized by category and use consistent naming:
 * - _PCT suffix: percentage as decimal (0.02 = 2%)
 * - _X100 suffix: percentage * 100 for fixed-point storage (200 = 2%)
 * - _BPS suffix: basis points (100 bps = 1%)
 */

namespace hft::config {

// =============================================================================
// Trading Costs (basis for target/stop calculation)
// =============================================================================
namespace costs {
    // Commission rate (Binance taker fee)
    constexpr double COMMISSION_PCT = 0.001;           // 0.1%
    constexpr int32_t COMMISSION_X10000 = 10;          // 0.1% * 10000

    // Slippage estimate
    constexpr double SLIPPAGE_BPS = 5.0;               // 5 basis points
    constexpr int32_t SLIPPAGE_BPS_X100 = 500;         // 5 bps * 100

    // Round-trip cost = 2 * (commission + slippage)
    // = 2 * (0.1% + 0.05%) = 0.3%
    constexpr double ROUND_TRIP_PCT = 0.003;           // 0.3%
    constexpr int32_t ROUND_TRIP_X100 = 30;            // 0.3% * 100
}

// =============================================================================
// Target & Stop Loss (derived from round-trip costs)
// =============================================================================
namespace targets {
    // Multipliers for calculating from round-trip cost
    constexpr int32_t TARGET_MULTIPLIER = 5;           // Target = 5x round trip
    constexpr int32_t RISK_REWARD_RATIO = 2;           // Stop = 2x target

    // Calculated defaults:
    // Target = 5 * 0.3% = 1.5%
    // Stop = 2 * 1.5% = 3%
    constexpr double TARGET_PCT = 0.015;               // 1.5%
    constexpr int32_t TARGET_X100 = 150;               // 1.5% * 100

    constexpr double STOP_PCT = 0.03;                  // 3%
    constexpr int32_t STOP_X100 = 300;                 // 3% * 100

    // Pullback for trend exit
    constexpr double PULLBACK_PCT = 0.005;             // 0.5%
    constexpr int32_t PULLBACK_X100 = 50;              // 0.5% * 100
}

// =============================================================================
// Position Sizing
// =============================================================================
namespace position {
    constexpr double BASE_PCT = 0.02;                  // 2% base position
    constexpr int32_t BASE_X100 = 200;                 // 2% * 100

    constexpr double MAX_PCT = 0.05;                   // 5% max position
    constexpr int32_t MAX_X100 = 500;                  // 5% * 100

    constexpr double MIN_TRADE_VALUE = 100.0;          // $100 minimum
    constexpr int32_t MIN_TRADE_VALUE_X100 = 10000;    // $100 * 100

    // Unit-based mode
    constexpr int32_t MAX_UNITS = 10;                  // Max 10 units when unit-based
}

// =============================================================================
// EMA Deviation Thresholds (by market regime)
// =============================================================================
namespace ema {
    // How far above EMA is acceptable for buying
    constexpr double DEV_TRENDING_PCT = 0.01;          // 1% in uptrend
    constexpr int32_t DEV_TRENDING_X100 = 100;         // 1% * 100
    constexpr int32_t DEV_TRENDING_X1000 = 10;         // For SharedConfig format

    constexpr double DEV_RANGING_PCT = 0.005;          // 0.5% in ranging
    constexpr int32_t DEV_RANGING_X100 = 50;           // 0.5% * 100
    constexpr int32_t DEV_RANGING_X1000 = 5;           // For SharedConfig format

    constexpr double DEV_HIGHVOL_PCT = 0.002;          // 0.2% in high volatility
    constexpr int32_t DEV_HIGHVOL_X100 = 20;           // 0.2% * 100
    constexpr int32_t DEV_HIGHVOL_X1000 = 2;           // For SharedConfig format
}

// =============================================================================
// Risk Management
// =============================================================================
namespace risk {
    constexpr double SPREAD_MULTIPLIER = 1.5;          // 1.5x spread threshold
    constexpr int32_t SPREAD_MULTIPLIER_X10 = 15;      // 1.5 * 10

    constexpr double DRAWDOWN_THRESHOLD_PCT = 0.02;    // 2% max drawdown
    constexpr int32_t DRAWDOWN_THRESHOLD_X100 = 200;   // 2% * 100

    constexpr int32_t LOSS_STREAK_THRESHOLD = 2;       // Stop after 2 consecutive losses
}

// =============================================================================
// Spike Detection
// =============================================================================
namespace spike {
    constexpr double THRESHOLD_SIGMA = 3.0;            // 3σ statistical significance
    constexpr int32_t THRESHOLD_X100 = 300;            // 3.0 * 100

    constexpr int32_t LOOKBACK_BARS = 10;              // 10 bars for averaging

    constexpr double MIN_MOVE_PCT = 0.005;             // 0.5% minimum move
    constexpr int32_t MIN_MOVE_X10000 = 50;            // 0.5% * 10000

    constexpr int32_t COOLDOWN_BARS = 5;               // 5 bars between detections
}

// =============================================================================
// Order Execution
// =============================================================================
namespace execution {
    constexpr int32_t COOLDOWN_MS = 2000;              // 2 second cooldown
    constexpr int32_t SIGNAL_STRENGTH = 1;             // 1 = Medium signals

    // Limit order settings
    constexpr double LIMIT_OFFSET_BPS = 2.0;           // 2 bps inside spread
    constexpr int32_t LIMIT_OFFSET_BPS_X100 = 200;     // 2 * 100
    constexpr int32_t LIMIT_TIMEOUT_MS = 500;          // 500ms before market order

    // Order type: 0 = Auto, 1 = Market, 2 = Limit
    constexpr uint8_t ORDER_TYPE_AUTO = 0;
    constexpr uint8_t ORDER_TYPE_MARKET = 1;
    constexpr uint8_t ORDER_TYPE_LIMIT = 2;
}

// =============================================================================
// Feature Flags
// =============================================================================
namespace flags {
    constexpr bool AUTO_TUNE_ENABLED = true;
    constexpr bool TRADING_ENABLED = true;
    constexpr bool PAPER_TRADING = true;               // Default to paper
    constexpr uint8_t USE_GLOBAL_ALL = 0x0F;           // Use global for all config groups
}

} // namespace hft::config
