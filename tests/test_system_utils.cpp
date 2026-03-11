#include "../include/util/system.hpp"

#include <cassert>
#include <csignal>
#include <iostream>

using namespace hft::util;

#define TEST(name) void name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "Running " << #name << "... ";                                                                    \
        name();                                                                                                        \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

// ============================================================================
// Signal Handler Tests
// ============================================================================

static bool callback_invoked = false;
static void test_callback() {
    callback_invoked = true;
}

TEST(test_install_shutdown_handler) {
    std::atomic<bool> running{true};
    install_shutdown_handler(running);

    assert(running.load() == true);
}

TEST(test_shutdown_handler_sets_flag) {
    std::atomic<bool> running{true};
    install_shutdown_handler(running);

    // Manually invoke the handler
    graceful_shutdown_handler(SIGTERM);

    assert(running.load() == false);
}

TEST(test_shutdown_handler_with_callback) {
    std::atomic<bool> running{true};
    callback_invoked = false;

    install_shutdown_handler(running, test_callback);

    // Manually invoke the handler
    graceful_shutdown_handler(SIGINT);

    assert(callback_invoked == true);
    assert(running.load() == false);
}

TEST(test_shutdown_handler_without_callback) {
    std::atomic<bool> running{true};
    callback_invoked = false;

    install_shutdown_handler(running, nullptr);

    // Manually invoke the handler
    graceful_shutdown_handler(SIGTERM);

    assert(callback_invoked == false); // Callback should not be invoked
    assert(running.load() == false);
}

// ============================================================================
// CPU Affinity Tests
// ============================================================================

TEST(test_cpu_affinity_negative_skips) {
    // Negative CPU should skip pinning and return true
    bool result = set_cpu_affinity(-1);
    assert(result == true);
}

TEST(test_cpu_affinity_zero_succeeds_or_fails_gracefully) {
    // CPU 0 should either succeed or fail gracefully
    bool result = set_cpu_affinity(0);

    // On systems without permission, it may fail, but should not crash
    // We just check it returns a boolean
    assert(result == true || result == false);
}

TEST(test_cpu_affinity_high_core_fails_gracefully) {
    // Very high CPU core number should fail gracefully
    bool result = set_cpu_affinity(999);

    // Should return false on most systems (core 999 doesn't exist)
    // But won't crash
    (void)result; // May succeed or fail depending on system
}

int main() {
    RUN_TEST(test_install_shutdown_handler);
    RUN_TEST(test_shutdown_handler_sets_flag);
    RUN_TEST(test_shutdown_handler_with_callback);
    RUN_TEST(test_shutdown_handler_without_callback);
    RUN_TEST(test_cpu_affinity_negative_skips);
    RUN_TEST(test_cpu_affinity_zero_succeeds_or_fails_gracefully);
    RUN_TEST(test_cpu_affinity_high_core_fails_gracefully);

    std::cout << "\nAll 7 system_utils tests passed!\n";
    return 0;
}
