#include <cassert>
#include <iostream>
#include <atomic>
#include <csignal>

#include "../include/util/system.hpp"

using namespace hft::util;

#define TEST(name) void name()
#define RUN_TEST(name) do { \
    std::cout << "Running " << #name << "... "; \
    name(); \
    std::cout << "PASSED\n"; \
} while(0)

#define ASSERT_TRUE(x) assert(x)
#define ASSERT_FALSE(x) assert(!(x))

static bool g_callback_invoked = false;

void test_callback() {
    g_callback_invoked = true;
}

TEST(test_install_sets_up_handler) {
    std::atomic<bool> running{true};
    install_shutdown_handler(running, nullptr);

    ASSERT_TRUE(running.load());
}

TEST(test_signal_sets_running_to_false) {
    std::atomic<bool> running{true};
    install_shutdown_handler(running, nullptr);

    ASSERT_TRUE(running.load());

    // Trigger SIGINT
    std::raise(SIGINT);

    ASSERT_FALSE(running.load());
}

TEST(test_callback_is_invoked) {
    std::atomic<bool> running{true};
    g_callback_invoked = false;
    install_shutdown_handler(running, test_callback);

    std::raise(SIGTERM);

    ASSERT_TRUE(g_callback_invoked);
    ASSERT_FALSE(running.load());
}

int main() {
    std::cout << "=== Signal Handler Tests ===\n";
    RUN_TEST(test_install_sets_up_handler);
    RUN_TEST(test_signal_sets_running_to_false);
    RUN_TEST(test_callback_is_invoked);
    std::cout << "All tests passed!\n";
    return 0;
}
