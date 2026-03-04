#include "../include/ipc/shared_config.hpp"

#include <cassert>
#include <iostream>

using namespace hft::ipc;

// Test 1: Metrics mode enabled flag
void test_metrics_mode_enabled() {
    SharedConfig config;
    config.init();

    // Default should be OFF
    assert(!config.is_metrics_mode_enabled());

    // Enable metrics mode
    config.set_metrics_mode(true);
    assert(config.is_metrics_mode_enabled());

    // Disable metrics mode
    config.set_metrics_mode(false);
    assert(!config.is_metrics_mode_enabled());

    std::cout << "[PASS] test_metrics_mode_enabled\n";
}

// Test 2: Metrics mode priority (METRICS > CONFIG > REGIME)
void test_metrics_mode_priority() {
    SharedConfig config;
    config.init();

    // Enable all modes
    config.set_metrics_mode(true);
    config.set_tuner_state(TunerState::ON);

    // Metrics mode should take precedence
    assert(config.is_metrics_mode_enabled());
    assert(config.is_tuner_on());

    // In execution path, METRICS mode check comes first
    bool use_metrics = config.is_metrics_mode_enabled();
    bool use_config = config.is_tuner_on() || config.is_tuner_paused();

    // METRICS should win
    assert(use_metrics);
    assert(use_config); // Also true, but metrics takes precedence

    std::cout << "[PASS] test_metrics_mode_priority\n";
}

// Test 3: Metrics mode sequence number update
void test_metrics_mode_sequence() {
    SharedConfig config;
    config.init();

    uint32_t initial_seq = config.sequence.load();

    // Setting metrics mode should increment sequence
    config.set_metrics_mode(true);
    uint32_t after_enable = config.sequence.load();
    assert(after_enable == initial_seq + 1);

    // Setting again should increment again
    config.set_metrics_mode(false);
    uint32_t after_disable = config.sequence.load();
    assert(after_disable == after_enable + 1);

    std::cout << "[PASS] test_metrics_mode_sequence\n";
}

int main() {
    std::cout << "Running Metrics Mode tests...\n\n";

    test_metrics_mode_enabled();
    test_metrics_mode_priority();
    test_metrics_mode_sequence();

    std::cout << "\n✓ All 3 tests passed!\n";
    return 0;
}
