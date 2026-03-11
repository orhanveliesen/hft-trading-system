#include "../include/strategy/market_health_monitor.hpp"

#include <cassert>
#include <iostream>

using namespace hft::strategy;

void test_out_of_bounds_symbol_id() {
    // Test that out-of-bounds symbol_id is handled gracefully
    // Covers line 47: if (symbol_id >= MAX_SYMBOLS) return;
    MarketHealthMonitor monitor(10, 0.5, 60);

    // Try to update with symbol_id >= MAX_SYMBOLS (64)
    monitor.update_symbol(64, true);  // Exactly at MAX_SYMBOLS
    monitor.update_symbol(100, true); // Way beyond MAX_SYMBOLS

    // Should not crash and should not trigger liquidation
    assert(!monitor.should_liquidate());
}

void test_basic_spike_tracking() {
    MarketHealthMonitor monitor(10, 0.5, 60);

    // Update a few symbols: 2 spikes out of 5 active = 40% < 50%
    monitor.update_symbol(0, true);  // spike
    monitor.update_symbol(1, true);  // spike
    monitor.update_symbol(2, false); // no spike
    monitor.update_symbol(3, false); // no spike
    monitor.update_symbol(4, false); // no spike

    // Not enough for crash (2/5 = 40% < 50%)
    assert(!monitor.should_liquidate());
}

void test_crash_detection() {
    MarketHealthMonitor monitor(10, 0.5, 60);

    // Trigger spikes on 5/10 symbols (50% threshold)
    for (size_t i = 0; i < 5; i++) {
        monitor.update_symbol(i, true);
    }

    // Should trigger liquidation at 50% threshold
    assert(monitor.should_liquidate());
}

void test_cooldown_period() {
    MarketHealthMonitor monitor(10, 0.5, 60);

    // Trigger crash
    for (size_t i = 0; i < 5; i++) {
        monitor.update_symbol(i, true);
    }

    assert(monitor.should_liquidate());

    // Even if spikes clear, should stay in cooldown
    for (size_t i = 0; i < 10; i++) {
        monitor.update_symbol(i, false);
    }

    // Still in cooldown
    monitor.tick();
    assert(monitor.in_cooldown());
}

void test_reset() {
    MarketHealthMonitor monitor(10, 0.5, 60);

    // Trigger crash
    for (size_t i = 0; i < 5; i++) {
        monitor.update_symbol(i, true);
    }

    assert(monitor.should_liquidate());

    // Reset
    monitor.reset();

    assert(!monitor.should_liquidate());
    assert(!monitor.in_cooldown());
}

int main() {
    test_out_of_bounds_symbol_id();
    test_basic_spike_tracking();
    test_crash_detection();
    test_cooldown_period();
    test_reset();

    std::cout << "All market_health_monitor tests passed!\n";
    return 0;
}
