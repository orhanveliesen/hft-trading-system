#pragma once

/**
 * Strategy constants for HFT system
 *
 * Centralizes magic numbers used in trading strategy components.
 * These constants control:
 * - When to switch trading modes based on win/loss streaks
 * - How aggressively to adjust position sizes
 * - EMA deviation thresholds for different market conditions
 */

#include <cstdint>

namespace hft {
namespace strategy {

/**
 * Streak thresholds for mode transitions.
 * Determines when to become more cautious or aggressive based on
 * consecutive wins/losses.
 */
struct StreakThresholds {
    // Loss streak levels - progressively more defensive
    static constexpr int LOSSES_TO_CAUTIOUS = 2;      // Start being careful
    static constexpr int LOSSES_TO_TIGHTEN_SIGNAL = 3; // Require stronger signals
    static constexpr int LOSSES_TO_DEFENSIVE = 4;      // Reduce position sizes
    static constexpr int LOSSES_TO_PAUSE = 5;          // Stop trading temporarily
    static constexpr int LOSSES_TO_EXIT_ONLY = 6;      // Only close positions

    // Win streak levels - can become more aggressive
    static constexpr int WINS_TO_AGGRESSIVE = 3;       // Allow larger positions
    static constexpr int WINS_MAX_AGGRESSIVE = 5;      // Cap on aggression
};

/**
 * Auto-tune multipliers for dynamic parameter adjustment.
 * Applied when performance changes to protect capital or capture momentum.
 */
struct AutoTuneMultipliers {
    // Tighten on losses - reduce exposure
    static constexpr double TIGHTEN_FACTOR = 1.5;     // Increase min trade value by 50%

    // Relax on wins - allow more exposure
    static constexpr double RELAX_FACTOR = 0.9;       // Decrease min trade value by 10%

    // Bounds for position sizing adjustments
    static constexpr double MIN_POSITION_RATIO = 0.1; // Never go below 10% of base
    static constexpr double MAX_POSITION_RATIO = 2.0; // Never exceed 200% of base
};

/**
 * EMA deviation thresholds for different market regimes.
 * Controls how far price can deviate from EMA before we skip entries.
 */
struct EmaThresholds {
    static constexpr double TRENDING_UP = 0.01;       // 1% - allow larger deviation in uptrend
    static constexpr double RANGING = 0.005;          // 0.5% - tighter in sideways market
    static constexpr double HIGH_VOL = 0.002;         // 0.2% - very tight in high volatility
    static constexpr double DEFAULT = 0.005;          // 0.5% - default threshold
};

/**
 * Drawdown thresholds for risk mode transitions.
 * Controls when to switch to defensive or exit-only modes based on portfolio drawdown.
 */
struct DrawdownThresholds {
    static constexpr double TO_DEFENSIVE = 0.03;  // 3% drawdown → DEFENSIVE mode
    static constexpr double TO_EXIT_ONLY = 0.05;  // 5% drawdown → EXIT_ONLY mode
};

/**
 * Minimum position threshold - below this, consider position closed.
 * Prevents dust positions from triggering unnecessary trades.
 */
static constexpr double MIN_POSITION_THRESHOLD = 1e-8;

}  // namespace strategy
}  // namespace hft
