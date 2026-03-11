#include "../include/util/time_utils.hpp"

#include <cassert>
#include <iostream>
#include <thread>

using namespace hft::util;

#define TEST(name) void name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "Running " << #name << "... ";                                                                    \
        name();                                                                                                        \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

TEST(test_now_ns_returns_non_zero) {
    uint64_t ts = now_ns();
    assert(ts > 0);
}

TEST(test_now_ns_is_monotonic) {
    uint64_t ts1 = now_ns();
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    uint64_t ts2 = now_ns();

    // ts2 should be greater than ts1 (monotonic)
    assert(ts2 > ts1);
}

TEST(test_wall_clock_ns_returns_non_zero) {
    uint64_t ts = wall_clock_ns();
    assert(ts > 0);
}

TEST(test_wall_clock_ns_is_monotonic) {
    uint64_t ts1 = wall_clock_ns();
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    uint64_t ts2 = wall_clock_ns();

    // ts2 should be greater than ts1
    assert(ts2 > ts1);
}

TEST(test_now_ns_precision) {
    uint64_t ts1 = now_ns();
    uint64_t ts2 = now_ns();

    // Even back-to-back calls should show some difference (nanosecond precision)
    // Or at worst be equal, but not go backwards
    assert(ts2 >= ts1);
}

TEST(test_wall_clock_ns_is_realistic) {
    uint64_t ts = wall_clock_ns();

    // Should be a reasonable timestamp (after year 2020)
    // 2020-01-01 00:00:00 UTC = 1577836800 seconds = 1577836800000000000 ns
    uint64_t min_timestamp = 1577836800000000000ULL;
    assert(ts > min_timestamp);

    // Should be before year 2100
    // 2100-01-01 00:00:00 UTC = 4102444800 seconds = 4102444800000000000 ns
    uint64_t max_timestamp = 4102444800000000000ULL;
    assert(ts < max_timestamp);
}

int main() {
    RUN_TEST(test_now_ns_returns_non_zero);
    RUN_TEST(test_now_ns_is_monotonic);
    RUN_TEST(test_wall_clock_ns_returns_non_zero);
    RUN_TEST(test_wall_clock_ns_is_monotonic);
    RUN_TEST(test_now_ns_precision);
    RUN_TEST(test_wall_clock_ns_is_realistic);

    std::cout << "\nAll 6 time_utils tests passed!\n";
    return 0;
}
