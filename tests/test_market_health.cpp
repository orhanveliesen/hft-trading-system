/**
 * Test Market Health Monitor
 *
 * Detects market-wide crashes when multiple symbols spike simultaneously.
 * Triggers emergency liquidation when crash threshold is exceeded.
 *
 * Tests:
 * 1. Normal state - no crash when few symbols spike
 * 2. Crash detection when threshold exceeded
 * 3. Cooldown behavior after crash
 * 4. Reset and recovery
 */

#include <cassert>
#include <iostream>
#include "../include/strategy/market_health_monitor.hpp"

using namespace hft::strategy;

#define TEST(name) void name()
#define RUN_TEST(name) do { \
    std::cout << "Running " << #name << "... "; \
    name(); \
    std::cout << "PASSED\n"; \
} while(0)

#define ASSERT_EQ(a, b) assert((a) == (b))
#define ASSERT_TRUE(x) assert(x)
#define ASSERT_FALSE(x) assert(!(x))
#define ASSERT_GT(a, b) assert((a) > (b))

TEST(test_initial_state_healthy) {
    MarketHealthMonitor monitor(10, 0.5);  // 10 symbols, 50% threshold

    ASSERT_FALSE(monitor.is_crash());
    ASSERT_EQ(monitor.spike_count(), 0);
    ASSERT_EQ(monitor.spike_ratio(), 0.0);
}

TEST(test_no_crash_below_threshold) {
    MarketHealthMonitor monitor(10, 0.5);  // 10 symbols, 50% crash at 5+

    // 4 symbols spiking out of 10 = 40% < 50% threshold
    // Must update ALL 10 symbols since ratio = spike/active
    monitor.update_symbol(0, true);   // spike
    monitor.update_symbol(1, true);   // spike
    monitor.update_symbol(2, true);   // spike
    monitor.update_symbol(3, true);   // spike
    monitor.update_symbol(4, false);  // normal
    monitor.update_symbol(5, false);  // normal
    monitor.update_symbol(6, false);  // normal
    monitor.update_symbol(7, false);  // normal
    monitor.update_symbol(8, false);  // normal
    monitor.update_symbol(9, false);  // normal

    ASSERT_EQ(monitor.spike_count(), 4);
    ASSERT_FALSE(monitor.is_crash());
}

TEST(test_crash_at_threshold) {
    MarketHealthMonitor monitor(10, 0.5);  // 50% threshold

    // 5 symbols spiking = 50% = threshold
    for (int i = 0; i < 5; ++i) {
        monitor.update_symbol(i, true);
    }
    for (int i = 5; i < 10; ++i) {
        monitor.update_symbol(i, false);
    }

    ASSERT_EQ(monitor.spike_count(), 5);
    ASSERT_TRUE(monitor.is_crash());
}

TEST(test_crash_above_threshold) {
    MarketHealthMonitor monitor(10, 0.5);  // 50% threshold

    // 7 symbols spiking = 70% > 50%
    for (int i = 0; i < 7; ++i) {
        monitor.update_symbol(i, true);
    }
    for (int i = 7; i < 10; ++i) {
        monitor.update_symbol(i, false);
    }

    ASSERT_TRUE(monitor.is_crash());
    ASSERT_GT(monitor.spike_ratio(), 0.5);
}

TEST(test_symbol_recovery_clears_spike) {
    MarketHealthMonitor monitor(10, 0.5);

    // First, activate all 10 symbols
    // 5 symbols spike, 5 normal = 50% = crash
    for (int i = 0; i < 5; ++i) {
        monitor.update_symbol(i, true);  // spiking
    }
    for (int i = 5; i < 10; ++i) {
        monitor.update_symbol(i, false);  // normal
    }
    ASSERT_TRUE(monitor.is_crash());  // 5/10 = 50% >= threshold

    // 2 symbols recover → 3/10 = 30% < 50% → no longer crash
    monitor.update_symbol(0, false);
    monitor.update_symbol(1, false);

    ASSERT_EQ(monitor.spike_count(), 3);
    ASSERT_FALSE(monitor.is_crash());  // 3/10 = 30% < 50%
}

TEST(test_cooldown_after_crash) {
    MarketHealthMonitor monitor(10, 0.5, 5);  // 5 ticks cooldown

    // Trigger crash
    for (int i = 0; i < 6; ++i) {
        monitor.update_symbol(i, true);
    }
    ASSERT_TRUE(monitor.is_crash());
    ASSERT_TRUE(monitor.in_cooldown());

    // All symbols recover
    for (int i = 0; i < 10; ++i) {
        monitor.update_symbol(i, false);
    }

    // Still in cooldown even though no spikes
    ASSERT_EQ(monitor.spike_count(), 0);
    ASSERT_TRUE(monitor.in_cooldown());

    // Tick down cooldown
    for (int i = 0; i < 5; ++i) {
        monitor.tick();
    }

    ASSERT_FALSE(monitor.in_cooldown());
}

TEST(test_should_liquidate_only_once) {
    MarketHealthMonitor monitor(10, 0.5);

    // Trigger crash
    for (int i = 0; i < 6; ++i) {
        monitor.update_symbol(i, true);
    }

    // First check should return true (liquidate!)
    ASSERT_TRUE(monitor.should_liquidate());

    // Second check should return false (already triggered)
    ASSERT_FALSE(monitor.should_liquidate());

    // Even with more spikes, shouldn't trigger again until reset
    monitor.update_symbol(7, true);
    ASSERT_FALSE(monitor.should_liquidate());
}

TEST(test_reset_allows_new_liquidation) {
    MarketHealthMonitor monitor(10, 0.5);

    // Trigger crash and liquidate
    for (int i = 0; i < 6; ++i) {
        monitor.update_symbol(i, true);
    }
    ASSERT_TRUE(monitor.should_liquidate());

    // Reset
    monitor.reset();

    // New crash should trigger again
    for (int i = 0; i < 6; ++i) {
        monitor.update_symbol(i, true);
    }
    ASSERT_TRUE(monitor.should_liquidate());
}

TEST(test_active_symbol_count) {
    MarketHealthMonitor monitor(10, 0.5);

    // Only 5 symbols are active (have been updated)
    monitor.update_symbol(0, false);
    monitor.update_symbol(1, false);
    monitor.update_symbol(2, true);
    monitor.update_symbol(3, true);
    monitor.update_symbol(4, true);

    // 3 out of 5 active = 60% → crash
    ASSERT_EQ(monitor.active_count(), 5);
    ASSERT_EQ(monitor.spike_count(), 3);
    ASSERT_TRUE(monitor.is_crash());  // 3/5 = 60% > 50%
}

TEST(test_different_thresholds) {
    // Conservative: 30% threshold
    MarketHealthMonitor conservative(10, 0.3);
    for (int i = 0; i < 3; ++i) {
        conservative.update_symbol(i, true);
    }
    for (int i = 3; i < 10; ++i) {
        conservative.update_symbol(i, false);
    }
    ASSERT_TRUE(conservative.is_crash());  // 30% = threshold

    // Aggressive: 70% threshold
    MarketHealthMonitor aggressive(10, 0.7);
    for (int i = 0; i < 6; ++i) {
        aggressive.update_symbol(i, true);
    }
    for (int i = 6; i < 10; ++i) {
        aggressive.update_symbol(i, false);
    }
    ASSERT_FALSE(aggressive.is_crash());  // 60% < 70%
}

int main() {
    std::cout << "=== Market Health Monitor Tests ===\n\n";

    RUN_TEST(test_initial_state_healthy);
    RUN_TEST(test_no_crash_below_threshold);
    RUN_TEST(test_crash_at_threshold);
    RUN_TEST(test_crash_above_threshold);
    RUN_TEST(test_symbol_recovery_clears_spike);
    RUN_TEST(test_cooldown_after_crash);
    RUN_TEST(test_should_liquidate_only_once);
    RUN_TEST(test_reset_allows_new_liquidation);
    RUN_TEST(test_active_symbol_count);
    RUN_TEST(test_different_thresholds);

    std::cout << "\n=== All market health tests PASSED ===\n";
    return 0;
}
