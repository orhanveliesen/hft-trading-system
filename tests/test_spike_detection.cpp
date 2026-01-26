/**
 * Test Spike Detection in RegimeDetector
 *
 * Tests:
 * 1. Normal price movement - no spike
 * 2. Spike detection (3x normal move)
 * 3. Cooldown behavior after spike
 * 4. is_dangerous() helper
 */

#include <cassert>
#include <iostream>
#include <cmath>
#include "../include/strategy/regime_detector.hpp"

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
#define ASSERT_LT(a, b) assert((a) < (b))
#define ASSERT_NE(a, b) assert((a) != (b))

// Helper to create spike-tuned config
RegimeConfig create_spike_config() {
    RegimeConfig config;
    config.spike_threshold = 3.0;      // 3x average = spike
    config.spike_lookback = 10;         // Use 10 bars for average
    config.spike_min_move = 0.005;     // 0.5% minimum move
    config.spike_cooldown = 5;          // 5 bars cooldown
    config.lookback = 10;
    return config;
}

TEST(test_normal_movement_no_spike) {
    auto config = create_spike_config();
    RegimeDetector detector(config);

    // Feed normal price movements (~0.1% each)
    double price = 100.0;
    for (int i = 0; i < 20; ++i) {
        price *= 1.001;  // 0.1% move
        detector.update(price);
    }

    // Should NOT be a spike
    ASSERT_FALSE(detector.is_spike());
    ASSERT_NE(detector.current_regime(), MarketRegime::Spike);
    ASSERT_FALSE(detector.is_dangerous());
}

TEST(test_spike_detection_on_large_move) {
    auto config = create_spike_config();
    RegimeDetector detector(config);

    // Feed normal price movements to establish baseline
    double price = 100.0;
    for (int i = 0; i < 15; ++i) {
        price *= 1.001;  // 0.1% normal moves
        detector.update(price);
    }

    // Now simulate a spike (2% move when average is 0.1%)
    // This is 20x the average, should trigger spike
    double spike_price = price * 1.02;  // 2% sudden move
    detector.update(spike_price);

    // Should be a spike
    ASSERT_TRUE(detector.is_spike());
    ASSERT_EQ(detector.current_regime(), MarketRegime::Spike);
    ASSERT_TRUE(detector.is_dangerous());
}

TEST(test_spike_cooldown_behavior) {
    auto config = create_spike_config();
    RegimeDetector detector(config);

    // Establish baseline
    double price = 100.0;
    for (int i = 0; i < 15; ++i) {
        price *= 1.001;
        detector.update(price);
    }

    // Trigger spike
    price *= 1.02;
    detector.update(price);
    ASSERT_TRUE(detector.is_spike());
    ASSERT_EQ(detector.spike_cooldown(), 5);  // Cooldown started

    // Feed normal prices - should stay in spike mode during cooldown
    for (int i = 0; i < 3; ++i) {
        price *= 1.001;
        detector.update(price);
        ASSERT_EQ(detector.current_regime(), MarketRegime::Spike);
    }

    // Cooldown should be decreasing
    ASSERT_LT(detector.spike_cooldown(), 5);

    // After cooldown expires, should exit spike mode
    for (int i = 0; i < 10; ++i) {
        price *= 1.001;
        detector.update(price);
    }

    ASSERT_FALSE(detector.is_spike());
    ASSERT_NE(detector.current_regime(), MarketRegime::Spike);
}

TEST(test_minimum_move_threshold) {
    auto config = create_spike_config();
    RegimeDetector detector(config);

    // Feed some prices to establish baseline
    double price = 100.0;
    for (int i = 0; i < 15; ++i) {
        price *= 1.0001;  // Very small 0.01% moves
        detector.update(price);
    }

    // Even if relative move is 3x, if absolute move < 0.5%, no spike
    // Average move is ~0.01%, so 3x = 0.03% which is < 0.5% threshold
    price *= 1.0003;  // 0.03% move (3x average but below min threshold)
    detector.update(price);

    ASSERT_FALSE(detector.is_spike());
}

TEST(test_downward_spike) {
    auto config = create_spike_config();
    RegimeDetector detector(config);

    // Establish baseline
    double price = 100.0;
    for (int i = 0; i < 15; ++i) {
        price *= 1.001;  // Upward 0.1% moves
        detector.update(price);
    }

    // Sudden downward spike (-2%)
    price *= 0.98;
    detector.update(price);

    // Should detect spike (direction doesn't matter)
    ASSERT_TRUE(detector.is_spike());
    ASSERT_EQ(detector.current_regime(), MarketRegime::Spike);
}

TEST(test_is_dangerous_includes_high_volatility) {
    RegimeConfig high_vol_config;
    high_vol_config.high_vol_threshold = 0.005;  // Very low threshold
    high_vol_config.lookback = 10;

    RegimeDetector detector(high_vol_config);

    // Feed volatile prices to trigger HighVolatility regime
    double price = 100.0;
    for (int i = 0; i < 20; ++i) {
        // Alternating up/down to create high volatility
        price *= (i % 2 == 0) ? 1.02 : 0.98;
        detector.update(price);
    }

    // Even if not Spike, HighVolatility should be dangerous
    if (detector.current_regime() == MarketRegime::HighVolatility) {
        ASSERT_TRUE(detector.is_dangerous());
    }
}

TEST(test_reset_clears_spike_state) {
    auto config = create_spike_config();
    RegimeDetector detector(config);

    // Establish baseline and trigger spike
    double price = 100.0;
    for (int i = 0; i < 15; ++i) {
        price *= 1.001;
        detector.update(price);
    }
    price *= 1.02;
    detector.update(price);

    ASSERT_TRUE(detector.is_spike());

    // Reset should clear spike state
    detector.reset();

    ASSERT_FALSE(detector.is_spike());
    ASSERT_EQ(detector.spike_cooldown(), 0);
    ASSERT_EQ(detector.current_regime(), MarketRegime::Unknown);
}

TEST(test_regime_to_string_includes_spike) {
    ASSERT_EQ(regime_to_string(MarketRegime::Spike), std::string("SPIKE"));
}

int main() {
    std::cout << "=== Spike Detection Tests ===\n\n";

    RUN_TEST(test_normal_movement_no_spike);
    RUN_TEST(test_spike_detection_on_large_move);
    RUN_TEST(test_spike_cooldown_behavior);
    RUN_TEST(test_minimum_move_threshold);
    RUN_TEST(test_downward_spike);
    RUN_TEST(test_is_dangerous_includes_high_volatility);
    RUN_TEST(test_reset_clears_spike_state);
    RUN_TEST(test_regime_to_string_includes_spike);

    std::cout << "\n=== All spike detection tests PASSED ===\n";
    return 0;
}
