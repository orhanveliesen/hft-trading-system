#pragma once

/**
 * StreakTracker - Tracks consecutive wins and losses for strategy adaptation
 *
 * Centralizes streak tracking logic used for:
 * - Mode transitions (cautious, defensive, exit-only)
 * - Auto-tuning parameters based on performance
 * - Win rate calculation
 *
 * Usage:
 *   StreakTracker tracker;
 *   tracker.record_win();
 *   tracker.record_loss();
 *   if (tracker.is_loss_streak_critical()) { ... }
 *
 * NOTE: Header-only by design.
 * All methods are trivial (1-4 lines, simple increments/comparisons).
 * Separating to .cpp would:
 * - Prevent inlining at call sites
 * - Add ~15-25 cycle function call overhead per invocation
 * - For a simple ++counter operation that takes ~1-3 cycles, this is unacceptable
 * See: https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#Rf-inline
 */

#include "strategy_constants.hpp"

#include <algorithm>
#include <cstdint>

namespace hft {
namespace strategy {

class StreakTracker {
public:
    StreakTracker() = default;

    // Record outcomes
    void record_win() {
        ++total_wins_;
        ++current_win_streak_;
        current_loss_streak_ = 0;
        update_max_streaks();
    }

    void record_loss() {
        ++total_losses_;
        ++current_loss_streak_;
        current_win_streak_ = 0;
        update_max_streaks();
    }

    void reset() {
        total_wins_ = 0;
        total_losses_ = 0;
        current_win_streak_ = 0;
        current_loss_streak_ = 0;
        max_win_streak_ = 0;
        max_loss_streak_ = 0;
    }

    // Streak accessors
    int current_win_streak() const { return current_win_streak_; }
    int current_loss_streak() const { return current_loss_streak_; }
    int max_win_streak() const { return max_win_streak_; }
    int max_loss_streak() const { return max_loss_streak_; }

    // Totals
    int total_wins() const { return total_wins_; }
    int total_losses() const { return total_losses_; }
    int total_trades() const { return total_wins_ + total_losses_; }

    // Win rate (0.0 - 1.0)
    double win_rate() const {
        int total = total_wins_ + total_losses_;
        return total > 0 ? static_cast<double>(total_wins_) / total : 0.0;
    }

    // Streak-based checks using StreakThresholds constants
    bool is_loss_streak_cautious() const { return current_loss_streak_ >= StreakThresholds::LOSSES_TO_CAUTIOUS; }

    bool is_loss_streak_tighten_signal() const {
        return current_loss_streak_ >= StreakThresholds::LOSSES_TO_TIGHTEN_SIGNAL;
    }

    bool is_loss_streak_defensive() const { return current_loss_streak_ >= StreakThresholds::LOSSES_TO_DEFENSIVE; }

    bool is_loss_streak_pause() const { return current_loss_streak_ >= StreakThresholds::LOSSES_TO_PAUSE; }

    bool is_loss_streak_exit_only() const { return current_loss_streak_ >= StreakThresholds::LOSSES_TO_EXIT_ONLY; }

    bool is_win_streak_aggressive() const { return current_win_streak_ >= StreakThresholds::WINS_TO_AGGRESSIVE; }

private:
    void update_max_streaks() {
        max_win_streak_ = std::max(max_win_streak_, current_win_streak_);
        max_loss_streak_ = std::max(max_loss_streak_, current_loss_streak_);
    }

    int total_wins_ = 0;
    int total_losses_ = 0;
    int current_win_streak_ = 0;
    int current_loss_streak_ = 0;
    int max_win_streak_ = 0;
    int max_loss_streak_ = 0;
};

} // namespace strategy
} // namespace hft
