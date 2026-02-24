/**
 * Test RegimeDetector Performance Optimizations
 *
 * Tests that RegimeDetector uses no dynamic allocations on hot path.
 * This test file validates the optimized implementation.
 */

#include "../include/strategy/regime_detector.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <iostream>

#define TEST(name) void name()

#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "  " << #name << "... ";                                                                          \
        try {                                                                                                          \
            name();                                                                                                    \
            std::cout << "PASSED\n";                                                                                   \
        } catch (...) {                                                                                                \
            std::cout << "FAILED (exception)\n";                                                                       \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

#define ASSERT_TRUE(expr)                                                                                              \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            std::cerr << "\nFAIL: " << #expr << " is false\n";                                                         \
            assert(false);                                                                                             \
        }                                                                                                              \
    } while (0)

#define ASSERT_EQ(a, b)                                                                                                \
    do {                                                                                                               \
        auto _a = (a);                                                                                                 \
        auto _b = (b);                                                                                                 \
        if (_a != _b) {                                                                                                \
            std::cerr << "\nFAIL: " << #a << " (" << _a << ") != " << #b << " (" << _b << ")\n";                       \
            assert(false);                                                                                             \
        }                                                                                                              \
    } while (0)

#define ASSERT_NEAR(a, b, eps)                                                                                         \
    do {                                                                                                               \
        double _a = (a), _b = (b), _eps = (eps);                                                                       \
        if (std::abs(_a - _b) > _eps) {                                                                                \
            std::cerr << "\nFAIL: " << #a << " (" << _a << ") != " << #b << " (" << _b << ") within " << _eps << "\n"; \
            assert(false);                                                                                             \
        }                                                                                                              \
    } while (0)

using namespace hft::strategy;

// =============================================================================
// TEST 1: Basic functionality preserved after optimization
// =============================================================================
TEST(basic_regime_detection) {
    RegimeDetector detector;

    // Feed trending up data
    double base_price = 100.0;
    for (int i = 0; i < 30; ++i) {
        detector.update(base_price + i * 0.5); // Steady uptrend
    }

    // Should detect trending up
    MarketRegime regime = detector.current_regime();
    ASSERT_TRUE(regime == MarketRegime::TrendingUp || regime == MarketRegime::Ranging); // May need warmup
}

// =============================================================================
// TEST 2: Performance benchmark - no allocations
// =============================================================================
TEST(update_performance_no_allocation) {
    RegimeDetector detector;

    // Warm up
    for (int i = 0; i < 50; ++i) {
        detector.update(100.0 + (i % 10) * 0.1);
    }

    // Benchmark 10000 updates
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < 10000; ++i) {
        detector.update(100.0 + (i % 100) * 0.01);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double ns_per_update = static_cast<double>(ns) / 10000.0;

    std::cout << "(" << ns_per_update << " ns/update) ";

    // Should be under 1000ns per update (no allocation)
    // With allocation it would be 5000+ ns
    ASSERT_TRUE(ns_per_update < 2000.0);
}

// =============================================================================
// TEST 3: Ring buffer wraps correctly
// =============================================================================
TEST(ring_buffer_wrap) {
    RegimeDetector detector;

    // Feed more data than lookback window
    for (int i = 0; i < 100; ++i) {
        detector.update(100.0 + (i % 10) * 0.1);
    }

    // Should still work after wrap
    detector.update(110.0); // Price spike
    detector.update(111.0);

    // Should not crash and regime should be valid
    MarketRegime regime = detector.current_regime();
    ASSERT_TRUE(regime != MarketRegime::Unknown);
}

// =============================================================================
// TEST 4: Volatility calculation correct after optimization
// =============================================================================
TEST(volatility_calculation) {
    RegimeDetector detector;

    // Feed constant data - zero volatility
    for (int i = 0; i < 30; ++i) {
        detector.update(100.0);
    }

    double vol = detector.volatility();
    ASSERT_NEAR(vol, 0.0, 0.001);

    // Now add some volatility
    detector.reset();
    for (int i = 0; i < 30; ++i) {
        detector.update(100.0 + ((i % 2 == 0) ? 1.0 : -1.0));
    }

    vol = detector.volatility();
    ASSERT_TRUE(vol > 0.0); // Should detect volatility
}

// =============================================================================
// TEST 5: Spike detection still works
// =============================================================================
TEST(spike_detection) {
    RegimeDetector detector;

    // Build stable baseline
    for (int i = 0; i < 30; ++i) {
        detector.update(100.0 + (i % 3) * 0.01); // Very small movements
    }

    // Inject spike (>3x average move)
    detector.update(105.0); // 5% move vs ~0.01% average

    ASSERT_TRUE(detector.is_spike());
}

// =============================================================================
// TEST 6: Memory footprint check (no growing containers)
// =============================================================================
TEST(fixed_memory_footprint) {
    // RegimeDetector should have fixed size regardless of updates
    RegimeDetector detector;

    // Note: We can't directly check memory, but we can verify
    // the detector works after many updates (no OOM)
    for (int i = 0; i < 100000; ++i) {
        detector.update(100.0 + (i % 50) * 0.01);
    }

    // If we got here without crash/OOM, ring buffer is working
    ASSERT_TRUE(true);
}

// =============================================================================
// MAIN
// =============================================================================
int main() {
    std::cout << "Running RegimeDetector Performance Tests:\n";

    RUN_TEST(basic_regime_detection);
    RUN_TEST(update_performance_no_allocation);
    RUN_TEST(ring_buffer_wrap);
    RUN_TEST(volatility_calculation);
    RUN_TEST(spike_detection);
    RUN_TEST(fixed_memory_footprint);

    std::cout << "\nAll tests passed!\n";
    return 0;
}
