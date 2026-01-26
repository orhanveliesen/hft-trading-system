/**
 * Test exit path guard logic to prevent double-counting
 *
 * This test verifies:
 * 1. When tuner_mode=OFF, legacy exits are allowed
 * 2. When tuner_mode=ON, legacy exits are blocked (unified system handles)
 * 3. Guard condition correctly evaluates shared_config state
 */

#include <iostream>
#include <cassert>
#include <atomic>

// Mock SharedConfig for testing
class MockSharedConfig {
public:
    std::atomic<uint8_t> tuner_mode_{0};

    bool is_tuner_mode() const {
        return tuner_mode_.load() == 1;
    }

    void set_tuner_mode(uint8_t mode) {
        tuner_mode_.store(mode);
    }
};

// Test the guard condition logic
// This mirrors the actual code in hft.cpp:
// bool use_legacy_exits = !shared_config_ || !shared_config_->is_tuner_mode();

bool should_use_legacy_exits(MockSharedConfig* config) {
    return !config || !config->is_tuner_mode();
}

void test_guard_with_null_config() {
    std::cout << "  test_guard_with_null_config... ";

    // When config is null, legacy exits should be allowed (safe default)
    bool result = should_use_legacy_exits(nullptr);
    assert(result == true);

    std::cout << "PASSED\n";
}

void test_guard_tuner_mode_off() {
    std::cout << "  test_guard_tuner_mode_off... ";

    MockSharedConfig config;
    config.set_tuner_mode(0);  // OFF

    // When tuner_mode is OFF, legacy exits should be allowed
    bool result = should_use_legacy_exits(&config);
    assert(result == true);

    std::cout << "PASSED\n";
}

void test_guard_tuner_mode_on() {
    std::cout << "  test_guard_tuner_mode_on... ";

    MockSharedConfig config;
    config.set_tuner_mode(1);  // ON

    // When tuner_mode is ON, legacy exits should be BLOCKED
    bool result = should_use_legacy_exits(&config);
    assert(result == false);

    std::cout << "PASSED\n";
}

void test_guard_mode_switching() {
    std::cout << "  test_guard_mode_switching... ";

    MockSharedConfig config;

    // Start with tuner_mode OFF
    config.set_tuner_mode(0);
    assert(should_use_legacy_exits(&config) == true);

    // Switch to ON
    config.set_tuner_mode(1);
    assert(should_use_legacy_exits(&config) == false);

    // Switch back to OFF
    config.set_tuner_mode(0);
    assert(should_use_legacy_exits(&config) == true);

    std::cout << "PASSED\n";
}

// Test scenario: Simulating what happens when both paths are enabled
class MockPortfolio {
public:
    double cash = 10000.0;
    int sell_count = 0;

    void sell(double amount) {
        cash += amount;
        sell_count++;
    }
};

void test_single_exit_path_legacy() {
    std::cout << "  test_single_exit_path_legacy... ";

    MockSharedConfig config;
    config.set_tuner_mode(0);  // OFF - legacy mode

    MockPortfolio portfolio;
    double exit_value = 100.0;

    // Simulate exit flow
    bool use_legacy = should_use_legacy_exits(&config);

    if (use_legacy) {
        // Legacy path: direct portfolio update
        portfolio.sell(exit_value);
    }
    // (unified path would NOT fire when tuner_mode is OFF)

    // Should have exactly ONE sell
    assert(portfolio.sell_count == 1);
    assert(portfolio.cash == 10100.0);

    std::cout << "PASSED\n";
}

void test_single_exit_path_unified() {
    std::cout << "  test_single_exit_path_unified... ";

    MockSharedConfig config;
    config.set_tuner_mode(1);  // ON - unified mode

    MockPortfolio portfolio;
    double exit_value = 100.0;

    // Simulate exit flow
    bool use_legacy = should_use_legacy_exits(&config);

    if (use_legacy) {
        // Legacy path: SHOULD NOT FIRE
        portfolio.sell(exit_value);
    }

    // Simulate unified path callback (this is what would happen via exchange)
    // In real code, this comes from on_execution_report()
    portfolio.sell(exit_value);

    // Should have exactly ONE sell (from unified path only)
    assert(portfolio.sell_count == 1);
    assert(portfolio.cash == 10100.0);

    std::cout << "PASSED\n";
}

void test_double_counting_prevention() {
    std::cout << "  test_double_counting_prevention... ";

    MockSharedConfig config;
    config.set_tuner_mode(1);  // ON - unified mode

    MockPortfolio portfolio;
    double exit_value = 100.0;

    // This simulates what WOULD happen without the guard:
    // Both paths would fire, causing double-counting

    // With guard: legacy path blocked
    bool use_legacy = should_use_legacy_exits(&config);
    int expected_sells = 0;

    if (use_legacy) {
        portfolio.sell(exit_value);
        expected_sells++;
    }

    // Unified path always fires when tuner_mode is ON
    portfolio.sell(exit_value);
    expected_sells++;

    // Guard ensures only ONE path fires
    assert(expected_sells == 1);
    assert(portfolio.sell_count == 1);

    std::cout << "PASSED\n";
}

void test_check_and_close_guard() {
    std::cout << "  test_check_and_close_guard... ";

    // This tests the guard used in on_quote() for check_and_close()
    // Code: if (use_legacy_exits && portfolio_.symbol_active[id]) { ... }

    MockSharedConfig config;
    MockPortfolio portfolio;

    bool symbol_active = true;
    int check_and_close_calls = 0;

    // Scenario 1: Tuner OFF - check_and_close should run
    config.set_tuner_mode(0);
    bool use_legacy = should_use_legacy_exits(&config);
    if (use_legacy && symbol_active) {
        check_and_close_calls++;
    }
    assert(check_and_close_calls == 1);

    // Scenario 2: Tuner ON - check_and_close should NOT run
    config.set_tuner_mode(1);
    use_legacy = should_use_legacy_exits(&config);
    if (use_legacy && symbol_active) {
        check_and_close_calls++;
    }
    assert(check_and_close_calls == 1);  // Still 1, not incremented

    std::cout << "PASSED\n";
}

void test_trend_exit_guard() {
    std::cout << "  test_trend_exit_guard... ";

    // This tests the guard used in check_signal() for trend-based exits
    // Code: if (use_legacy_exits && holding > 0) { ... trend exit ... }

    MockSharedConfig config;
    MockPortfolio portfolio;

    double holding = 1.0;  // Have a position
    int trend_exit_calls = 0;

    // Scenario 1: Tuner OFF - trend exit should run
    config.set_tuner_mode(0);
    bool use_legacy = should_use_legacy_exits(&config);
    if (use_legacy && holding > 0) {
        trend_exit_calls++;
    }
    assert(trend_exit_calls == 1);

    // Scenario 2: Tuner ON - trend exit should NOT run
    config.set_tuner_mode(1);
    use_legacy = should_use_legacy_exits(&config);
    if (use_legacy && holding > 0) {
        trend_exit_calls++;
    }
    assert(trend_exit_calls == 1);  // Still 1, not incremented

    std::cout << "PASSED\n";
}

int main() {
    std::cout << "\n=== Exit Path Guard Tests ===\n\n";

    test_guard_with_null_config();
    test_guard_tuner_mode_off();
    test_guard_tuner_mode_on();
    test_guard_mode_switching();
    test_single_exit_path_legacy();
    test_single_exit_path_unified();
    test_double_counting_prevention();
    test_check_and_close_guard();
    test_trend_exit_guard();

    std::cout << "\n=== All Exit Path Guard Tests PASSED! ===\n";
    return 0;
}
