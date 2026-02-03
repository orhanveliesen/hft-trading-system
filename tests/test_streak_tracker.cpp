/**
 * Tests for StreakTracker class
 */

#include "../include/strategy/streak_tracker.hpp"
#include <cassert>
#include <iostream>

using namespace hft::strategy;

void test_initial_state() {
    StreakTracker tracker;
    assert(tracker.current_win_streak() == 0);
    assert(tracker.current_loss_streak() == 0);
    assert(tracker.total_wins() == 0);
    assert(tracker.total_losses() == 0);
    assert(tracker.win_rate() == 0.0);
    std::cout << "  [PASS] initial_state\n";
}

void test_record_win() {
    StreakTracker tracker;
    tracker.record_win();
    assert(tracker.current_win_streak() == 1);
    assert(tracker.current_loss_streak() == 0);
    assert(tracker.total_wins() == 1);

    tracker.record_win();
    assert(tracker.current_win_streak() == 2);
    assert(tracker.total_wins() == 2);
    std::cout << "  [PASS] record_win\n";
}

void test_record_loss() {
    StreakTracker tracker;
    tracker.record_loss();
    assert(tracker.current_loss_streak() == 1);
    assert(tracker.current_win_streak() == 0);
    assert(tracker.total_losses() == 1);

    tracker.record_loss();
    assert(tracker.current_loss_streak() == 2);
    assert(tracker.total_losses() == 2);
    std::cout << "  [PASS] record_loss\n";
}

void test_streak_resets() {
    StreakTracker tracker;

    // Build win streak
    tracker.record_win();
    tracker.record_win();
    tracker.record_win();
    assert(tracker.current_win_streak() == 3);
    assert(tracker.max_win_streak() == 3);

    // Loss resets win streak
    tracker.record_loss();
    assert(tracker.current_win_streak() == 0);
    assert(tracker.current_loss_streak() == 1);
    assert(tracker.max_win_streak() == 3);  // Max preserved

    // Build loss streak
    tracker.record_loss();
    tracker.record_loss();
    assert(tracker.current_loss_streak() == 3);

    // Win resets loss streak
    tracker.record_win();
    assert(tracker.current_loss_streak() == 0);
    assert(tracker.max_loss_streak() == 3);  // Max preserved
    std::cout << "  [PASS] streak_resets\n";
}

void test_win_rate() {
    StreakTracker tracker;

    // 3 wins, 1 loss = 75%
    tracker.record_win();
    tracker.record_win();
    tracker.record_win();
    tracker.record_loss();
    assert(tracker.win_rate() == 0.75);
    assert(tracker.total_trades() == 4);
    std::cout << "  [PASS] win_rate\n";
}

void test_loss_streak_thresholds() {
    StreakTracker tracker;

    // Record losses incrementally and check thresholds
    tracker.record_loss();
    assert(!tracker.is_loss_streak_cautious());

    tracker.record_loss();  // 2
    assert(tracker.is_loss_streak_cautious());
    assert(!tracker.is_loss_streak_tighten_signal());

    tracker.record_loss();  // 3
    assert(tracker.is_loss_streak_tighten_signal());
    assert(!tracker.is_loss_streak_defensive());

    tracker.record_loss();  // 4
    assert(tracker.is_loss_streak_defensive());
    assert(!tracker.is_loss_streak_pause());

    tracker.record_loss();  // 5
    assert(tracker.is_loss_streak_pause());
    assert(!tracker.is_loss_streak_exit_only());

    tracker.record_loss();  // 6
    assert(tracker.is_loss_streak_exit_only());
    std::cout << "  [PASS] loss_streak_thresholds\n";
}

void test_win_streak_threshold() {
    StreakTracker tracker;

    tracker.record_win();
    assert(!tracker.is_win_streak_aggressive());

    tracker.record_win();
    assert(!tracker.is_win_streak_aggressive());

    tracker.record_win();  // 3
    assert(tracker.is_win_streak_aggressive());
    std::cout << "  [PASS] win_streak_threshold\n";
}

void test_reset() {
    StreakTracker tracker;

    tracker.record_win();
    tracker.record_win();
    tracker.record_loss();
    tracker.record_loss();
    tracker.record_loss();

    tracker.reset();

    assert(tracker.current_win_streak() == 0);
    assert(tracker.current_loss_streak() == 0);
    assert(tracker.total_wins() == 0);
    assert(tracker.total_losses() == 0);
    assert(tracker.max_win_streak() == 0);
    assert(tracker.max_loss_streak() == 0);
    std::cout << "  [PASS] reset\n";
}

int main() {
    std::cout << "StreakTracker Tests:\n";

    test_initial_state();
    test_record_win();
    test_record_loss();
    test_streak_resets();
    test_win_rate();
    test_loss_streak_thresholds();
    test_win_streak_threshold();
    test_reset();

    std::cout << "\nAll StreakTracker tests passed!\n";
    return 0;
}
