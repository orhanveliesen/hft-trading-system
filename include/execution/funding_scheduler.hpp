#pragma once

#include <cstdint>

namespace hft {
namespace execution {

/**
 * @brief Funding phase relative to next funding time
 *
 * Binance futures funding schedule:
 * - 8-hour cycles: 00:00, 08:00, 16:00 UTC
 * - PreFunding: 15 minutes before funding (risky to exit Farming positions)
 * - PostFunding: 15 minutes after funding (safe to exit Farming positions)
 * - Normal: Outside the 30-minute window around funding events
 */
enum class FundingPhase {
    Normal,     ///< Not near funding event
    PreFunding, ///< 0-15 minutes before funding (can't exit Farming positions)
    PostFunding ///< 0-15 minutes after funding (safe to exit Farming positions)
};

/**
 * @brief Timing-aware funding schedule manager for futures trading
 *
 * Determines funding phase based on next_funding_time from exchange.
 * Used by FuturesEvaluator to make timing-aware decisions for Funding Farming mode.
 *
 * Design:
 * - Header-only (inline methods)
 * - Static methods (no state)
 * - No heap allocation
 *
 * Usage:
 *   auto phase = FundingScheduler::get_phase(fm.next_funding_time_ms, wall_clock_ns());
 *   if (!FundingScheduler::can_exit_farming(phase, position_age_ns)) { return; }
 */
class FundingScheduler {
public:
    static constexpr uint64_t PREFUNDING_WINDOW_MS = 15 * 60 * 1000;  ///< 15 minutes in ms
    static constexpr uint64_t POSTFUNDING_WINDOW_MS = 15 * 60 * 1000; ///< 15 minutes in ms

    /**
     * @brief Get current funding phase based on next funding time
     *
     * @param next_funding_time_ms Next funding time in milliseconds (from FuturesMetrics)
     * @param current_time_ns Current wall-clock time in nanoseconds (use wall_clock_ns())
     * @return Current funding phase
     *
     * Algorithm:
     * - time_to_funding = next_funding_time_ms - current_time_ms
     * - If 0 < time_to_funding <= 15 min: PreFunding
     * - If -15 min <= time_to_funding < 0: PostFunding
     * - Otherwise: Normal
     */
    static inline FundingPhase get_phase(uint64_t next_funding_time_ms, uint64_t current_time_ns) {
        if (next_funding_time_ms == 0)
            return FundingPhase::Normal; // No funding data yet

        uint64_t current_time_ms = current_time_ns / 1'000'000;
        int64_t time_to_funding_ms = static_cast<int64_t>(next_funding_time_ms) - static_cast<int64_t>(current_time_ms);

        // PreFunding: 0 to 15 minutes before funding
        if (time_to_funding_ms > 0 && static_cast<uint64_t>(time_to_funding_ms) <= PREFUNDING_WINDOW_MS) {
            return FundingPhase::PreFunding;
        }

        // PostFunding: 0 to 15 minutes after funding (negative time_to_funding)
        if (time_to_funding_ms < 0 && static_cast<uint64_t>(-time_to_funding_ms) <= POSTFUNDING_WINDOW_MS) {
            return FundingPhase::PostFunding;
        }

        return FundingPhase::Normal;
    }

    /**
     * @brief Check if safe to exit a Farming position
     *
     * @param phase Current funding phase
     * @param position_age_ns Time since position opened (reserved for future min holding period check)
     * @return true if safe to exit
     *
     * Logic:
     * - Can exit during PostFunding or Normal
     * - Cannot exit during PreFunding (would forfeit funding payment)
     */
    static inline bool can_exit_farming(FundingPhase phase, uint64_t position_age_ns) {
        (void)position_age_ns; // Reserved for future min holding period check

        // Can exit during PostFunding or Normal, NOT during PreFunding
        return phase != FundingPhase::PreFunding;
    }

    /**
     * @brief Check if safe to enter a new Farming position
     *
     * @param phase Current funding phase
     * @return true if safe to enter
     *
     * Logic:
     * - Can enter anytime (can be refined later to avoid deep PreFunding)
     */
    static inline bool can_enter_farming(FundingPhase phase) {
        // Can enter anytime (can be refined later to avoid deep PreFunding window)
        (void)phase;
        return true;
    }
};

} // namespace execution
} // namespace hft
