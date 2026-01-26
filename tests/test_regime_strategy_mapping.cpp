/**
 * Tests for Regime-Strategy Mapping functionality
 *
 * Tests:
 * - Default values after init
 * - Get/Set for each regime
 * - Boundary checks (invalid indices)
 * - Sequence increment on changes
 * - Preset configurations
 */

#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>
#include "../include/ipc/shared_config.hpp"
#include "../include/strategy/regime_detector.hpp"

using namespace hft::ipc;
using namespace hft::strategy;

#define TEST(name) void name()
#define RUN_TEST(name) do { \
    std::cout << "  " << #name << "... "; \
    name(); \
    std::cout << "PASSED\n"; \
} while(0)

#define ASSERT_EQ(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a != _b) { \
        std::cerr << "FAILED: " << #a << " != " << #b << "\n"; \
        assert(false); \
    } \
} while(0)

#define ASSERT_STR_EQ(a, b) do { \
    std::string _a = (a); std::string _b = (b); \
    if (_a != _b) { \
        std::cerr << "FAILED: " << #a << " (" << _a << ") != " << #b << " (" << _b << ")\n"; \
        assert(false); \
    } \
} while(0)

#define ASSERT_TRUE(x) assert(x)
#define ASSERT_FALSE(x) assert(!(x))

constexpr const char* TEST_SHM_NAME = "/trader_regime_strategy_test";

// Helper: Clean up shared memory before/after tests
void cleanup_shm() {
    SharedConfig::destroy(TEST_SHM_NAME);
}

// ============================================================================
// Test: Default values after init
// ============================================================================
TEST(test_default_regime_strategy_mapping) {
    cleanup_shm();
    SharedConfig* config = SharedConfig::create(TEST_SHM_NAME);
    ASSERT_TRUE(config != nullptr);

    // Check default mapping:
    // 0=Unknown → NONE (0)
    // 1=TrendingUp → MOMENTUM (1)
    // 2=TrendingDown → DEFENSIVE (4)
    // 3=Ranging → MKT_MAKER (3)
    // 4=HighVol → CAUTIOUS (5)
    // 5=LowVol → MKT_MAKER (3)
    // 6=Spike → NONE (0)

    ASSERT_EQ(config->get_strategy_for_regime(0), static_cast<uint8_t>(StrategyType::NONE));
    ASSERT_EQ(config->get_strategy_for_regime(1), static_cast<uint8_t>(StrategyType::MOMENTUM));
    ASSERT_EQ(config->get_strategy_for_regime(2), static_cast<uint8_t>(StrategyType::DEFENSIVE));
    ASSERT_EQ(config->get_strategy_for_regime(3), static_cast<uint8_t>(StrategyType::MKT_MAKER));
    ASSERT_EQ(config->get_strategy_for_regime(4), static_cast<uint8_t>(StrategyType::CAUTIOUS));
    ASSERT_EQ(config->get_strategy_for_regime(5), static_cast<uint8_t>(StrategyType::MKT_MAKER));
    ASSERT_EQ(config->get_strategy_for_regime(6), static_cast<uint8_t>(StrategyType::NONE));

    cleanup_shm();
}

// ============================================================================
// Test: Set and get strategy for each regime
// ============================================================================
TEST(test_set_get_strategy_for_regime) {
    cleanup_shm();
    SharedConfig* config = SharedConfig::create(TEST_SHM_NAME);
    ASSERT_TRUE(config != nullptr);

    // Set custom mapping
    config->set_strategy_for_regime(0, static_cast<uint8_t>(StrategyType::SMART));
    config->set_strategy_for_regime(1, static_cast<uint8_t>(StrategyType::MEAN_REV));
    config->set_strategy_for_regime(2, static_cast<uint8_t>(StrategyType::MKT_MAKER));
    config->set_strategy_for_regime(3, static_cast<uint8_t>(StrategyType::MOMENTUM));
    config->set_strategy_for_regime(4, static_cast<uint8_t>(StrategyType::NONE));
    config->set_strategy_for_regime(5, static_cast<uint8_t>(StrategyType::CAUTIOUS));
    config->set_strategy_for_regime(6, static_cast<uint8_t>(StrategyType::DEFENSIVE));

    // Verify
    ASSERT_EQ(config->get_strategy_for_regime(0), static_cast<uint8_t>(StrategyType::SMART));
    ASSERT_EQ(config->get_strategy_for_regime(1), static_cast<uint8_t>(StrategyType::MEAN_REV));
    ASSERT_EQ(config->get_strategy_for_regime(2), static_cast<uint8_t>(StrategyType::MKT_MAKER));
    ASSERT_EQ(config->get_strategy_for_regime(3), static_cast<uint8_t>(StrategyType::MOMENTUM));
    ASSERT_EQ(config->get_strategy_for_regime(4), static_cast<uint8_t>(StrategyType::NONE));
    ASSERT_EQ(config->get_strategy_for_regime(5), static_cast<uint8_t>(StrategyType::CAUTIOUS));
    ASSERT_EQ(config->get_strategy_for_regime(6), static_cast<uint8_t>(StrategyType::DEFENSIVE));

    cleanup_shm();
}

// ============================================================================
// Test: Boundary checks (invalid regime indices)
// ============================================================================
TEST(test_invalid_regime_index) {
    cleanup_shm();
    SharedConfig* config = SharedConfig::create(TEST_SHM_NAME);
    ASSERT_TRUE(config != nullptr);

    // Invalid negative index should return 0
    ASSERT_EQ(config->get_strategy_for_regime(-1), 0);
    ASSERT_EQ(config->get_strategy_for_regime(-100), 0);

    // Invalid high index should return 0
    ASSERT_EQ(config->get_strategy_for_regime(7), 0);
    ASSERT_EQ(config->get_strategy_for_regime(100), 0);

    // Invalid set should be ignored (no crash)
    uint32_t seq_before = config->sequence.load();
    config->set_strategy_for_regime(-1, 5);
    config->set_strategy_for_regime(100, 5);
    // Sequence should NOT change for invalid indices
    ASSERT_EQ(config->sequence.load(), seq_before);

    cleanup_shm();
}

// ============================================================================
// Test: Sequence increment on changes
// ============================================================================
TEST(test_sequence_increment_on_strategy_change) {
    cleanup_shm();
    SharedConfig* config = SharedConfig::create(TEST_SHM_NAME);
    ASSERT_TRUE(config != nullptr);

    uint32_t initial_seq = config->sequence.load();

    // Each set should increment sequence
    config->set_strategy_for_regime(0, 1);
    ASSERT_EQ(config->sequence.load(), initial_seq + 1);

    config->set_strategy_for_regime(1, 2);
    ASSERT_EQ(config->sequence.load(), initial_seq + 2);

    config->set_strategy_for_regime(6, 0);
    ASSERT_EQ(config->sequence.load(), initial_seq + 3);

    cleanup_shm();
}

// ============================================================================
// Test: Strategy type to string conversion
// ============================================================================
TEST(test_strategy_type_to_string) {
    ASSERT_STR_EQ(strategy_type_to_string(StrategyType::NONE), "NONE");
    ASSERT_STR_EQ(strategy_type_to_string(StrategyType::MOMENTUM), "MOMENTUM");
    ASSERT_STR_EQ(strategy_type_to_string(StrategyType::MEAN_REV), "MEAN_REV");
    ASSERT_STR_EQ(strategy_type_to_string(StrategyType::MKT_MAKER), "MKT_MAKER");
    ASSERT_STR_EQ(strategy_type_to_string(StrategyType::DEFENSIVE), "DEFENSIVE");
    ASSERT_STR_EQ(strategy_type_to_string(StrategyType::CAUTIOUS), "CAUTIOUS");
    ASSERT_STR_EQ(strategy_type_to_string(StrategyType::SMART), "SMART");
}

// ============================================================================
// Test: Strategy type to short string conversion
// ============================================================================
TEST(test_strategy_type_to_short) {
    ASSERT_STR_EQ(strategy_type_to_short(StrategyType::NONE), "OFF");
    ASSERT_STR_EQ(strategy_type_to_short(StrategyType::MOMENTUM), "MOM");
    ASSERT_STR_EQ(strategy_type_to_short(StrategyType::MEAN_REV), "MRV");
    ASSERT_STR_EQ(strategy_type_to_short(StrategyType::MKT_MAKER), "MM");
    ASSERT_STR_EQ(strategy_type_to_short(StrategyType::DEFENSIVE), "DEF");
    ASSERT_STR_EQ(strategy_type_to_short(StrategyType::CAUTIOUS), "CAU");
    ASSERT_STR_EQ(strategy_type_to_short(StrategyType::SMART), "AI");
}

// ============================================================================
// Test: Conservative preset configuration
// ============================================================================
TEST(test_conservative_preset) {
    cleanup_shm();
    SharedConfig* config = SharedConfig::create(TEST_SHM_NAME);
    ASSERT_TRUE(config != nullptr);

    // Apply conservative preset
    // Conservative: Low risk, defensive in most regimes
    config->set_strategy_for_regime(0, static_cast<uint8_t>(StrategyType::NONE));       // Unknown
    config->set_strategy_for_regime(1, static_cast<uint8_t>(StrategyType::CAUTIOUS));   // TrendingUp
    config->set_strategy_for_regime(2, static_cast<uint8_t>(StrategyType::NONE));       // TrendingDown
    config->set_strategy_for_regime(3, static_cast<uint8_t>(StrategyType::MEAN_REV));   // Ranging
    config->set_strategy_for_regime(4, static_cast<uint8_t>(StrategyType::NONE));       // HighVol
    config->set_strategy_for_regime(5, static_cast<uint8_t>(StrategyType::CAUTIOUS));   // LowVol
    config->set_strategy_for_regime(6, static_cast<uint8_t>(StrategyType::NONE));       // Spike

    // Verify conservative mapping
    ASSERT_EQ(config->get_strategy_for_regime(0), static_cast<uint8_t>(StrategyType::NONE));
    ASSERT_EQ(config->get_strategy_for_regime(1), static_cast<uint8_t>(StrategyType::CAUTIOUS));
    ASSERT_EQ(config->get_strategy_for_regime(2), static_cast<uint8_t>(StrategyType::NONE));
    ASSERT_EQ(config->get_strategy_for_regime(3), static_cast<uint8_t>(StrategyType::MEAN_REV));
    ASSERT_EQ(config->get_strategy_for_regime(4), static_cast<uint8_t>(StrategyType::NONE));
    ASSERT_EQ(config->get_strategy_for_regime(5), static_cast<uint8_t>(StrategyType::CAUTIOUS));
    ASSERT_EQ(config->get_strategy_for_regime(6), static_cast<uint8_t>(StrategyType::NONE));

    cleanup_shm();
}

// ============================================================================
// Test: Aggressive preset configuration
// ============================================================================
TEST(test_aggressive_preset) {
    cleanup_shm();
    SharedConfig* config = SharedConfig::create(TEST_SHM_NAME);
    ASSERT_TRUE(config != nullptr);

    // Apply aggressive preset
    // Aggressive: Higher risk, active in all market conditions
    config->set_strategy_for_regime(0, static_cast<uint8_t>(StrategyType::MOMENTUM));   // Unknown
    config->set_strategy_for_regime(1, static_cast<uint8_t>(StrategyType::MOMENTUM));   // TrendingUp
    config->set_strategy_for_regime(2, static_cast<uint8_t>(StrategyType::MOMENTUM));   // TrendingDown
    config->set_strategy_for_regime(3, static_cast<uint8_t>(StrategyType::MKT_MAKER));  // Ranging
    config->set_strategy_for_regime(4, static_cast<uint8_t>(StrategyType::CAUTIOUS));   // HighVol
    config->set_strategy_for_regime(5, static_cast<uint8_t>(StrategyType::MKT_MAKER));  // LowVol
    config->set_strategy_for_regime(6, static_cast<uint8_t>(StrategyType::DEFENSIVE));  // Spike

    // Verify aggressive mapping
    ASSERT_EQ(config->get_strategy_for_regime(0), static_cast<uint8_t>(StrategyType::MOMENTUM));
    ASSERT_EQ(config->get_strategy_for_regime(1), static_cast<uint8_t>(StrategyType::MOMENTUM));
    ASSERT_EQ(config->get_strategy_for_regime(2), static_cast<uint8_t>(StrategyType::MOMENTUM));
    ASSERT_EQ(config->get_strategy_for_regime(3), static_cast<uint8_t>(StrategyType::MKT_MAKER));
    ASSERT_EQ(config->get_strategy_for_regime(4), static_cast<uint8_t>(StrategyType::CAUTIOUS));
    ASSERT_EQ(config->get_strategy_for_regime(5), static_cast<uint8_t>(StrategyType::MKT_MAKER));
    ASSERT_EQ(config->get_strategy_for_regime(6), static_cast<uint8_t>(StrategyType::DEFENSIVE));

    cleanup_shm();
}

// ============================================================================
// Test: MarketRegime enum values match expected indices
// ============================================================================
TEST(test_market_regime_enum_values) {
    // Verify MarketRegime enum values match the indices used in regime_strategy array
    ASSERT_EQ(static_cast<int>(MarketRegime::Unknown), 0);
    ASSERT_EQ(static_cast<int>(MarketRegime::TrendingUp), 1);
    ASSERT_EQ(static_cast<int>(MarketRegime::TrendingDown), 2);
    ASSERT_EQ(static_cast<int>(MarketRegime::Ranging), 3);
    ASSERT_EQ(static_cast<int>(MarketRegime::HighVolatility), 4);
    ASSERT_EQ(static_cast<int>(MarketRegime::LowVolatility), 5);
    ASSERT_EQ(static_cast<int>(MarketRegime::Spike), 6);
}

// ============================================================================
// Test: StrategyType enum values
// ============================================================================
TEST(test_strategy_type_enum_values) {
    ASSERT_EQ(static_cast<uint8_t>(StrategyType::NONE), 0);
    ASSERT_EQ(static_cast<uint8_t>(StrategyType::MOMENTUM), 1);
    ASSERT_EQ(static_cast<uint8_t>(StrategyType::MEAN_REV), 2);
    ASSERT_EQ(static_cast<uint8_t>(StrategyType::MKT_MAKER), 3);
    ASSERT_EQ(static_cast<uint8_t>(StrategyType::DEFENSIVE), 4);
    ASSERT_EQ(static_cast<uint8_t>(StrategyType::CAUTIOUS), 5);
    ASSERT_EQ(static_cast<uint8_t>(StrategyType::SMART), 6);
    ASSERT_EQ(STRATEGY_TYPE_COUNT, 7u);
}

// ============================================================================
// Test: Cross-process visibility (simulated with threads)
// ============================================================================
TEST(test_cross_thread_visibility) {
    cleanup_shm();
    SharedConfig* writer = SharedConfig::create(TEST_SHM_NAME);
    ASSERT_TRUE(writer != nullptr);

    std::atomic<bool> ready{false};
    std::atomic<bool> done{false};
    uint8_t observed_strategy = 255;

    // Reader thread (simulates another process)
    std::thread reader([&]() {
        SharedConfig* cfg = SharedConfig::open_rw(TEST_SHM_NAME);
        ASSERT_TRUE(cfg != nullptr);
        ready.store(true);

        while (!done.load()) {
            observed_strategy = cfg->get_strategy_for_regime(3);  // Ranging
            std::this_thread::yield();
        }
    });

    // Wait for reader ready
    while (!ready.load()) {
        std::this_thread::yield();
    }

    // Write new value for Ranging regime
    writer->set_strategy_for_regime(3, static_cast<uint8_t>(StrategyType::SMART));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    done.store(true);

    reader.join();

    // Reader should have seen the new value
    ASSERT_EQ(observed_strategy, static_cast<uint8_t>(StrategyType::SMART));

    cleanup_shm();
}

// ============================================================================
// Test: Regime to string conversion
// ============================================================================
TEST(test_regime_to_string) {
    ASSERT_STR_EQ(regime_to_string(MarketRegime::Unknown), "UNKNOWN");
    ASSERT_STR_EQ(regime_to_string(MarketRegime::TrendingUp), "TRENDING_UP");
    ASSERT_STR_EQ(regime_to_string(MarketRegime::TrendingDown), "TRENDING_DOWN");
    ASSERT_STR_EQ(regime_to_string(MarketRegime::Ranging), "RANGING");
    ASSERT_STR_EQ(regime_to_string(MarketRegime::HighVolatility), "HIGH_VOL");
    ASSERT_STR_EQ(regime_to_string(MarketRegime::LowVolatility), "LOW_VOL");
    ASSERT_STR_EQ(regime_to_string(MarketRegime::Spike), "SPIKE");
}

// ============================================================================
// Main
// ============================================================================
int main() {
    std::cout << "\n=== Regime-Strategy Mapping Tests ===\n\n";

    RUN_TEST(test_default_regime_strategy_mapping);
    RUN_TEST(test_set_get_strategy_for_regime);
    RUN_TEST(test_invalid_regime_index);
    RUN_TEST(test_sequence_increment_on_strategy_change);
    RUN_TEST(test_strategy_type_to_string);
    RUN_TEST(test_strategy_type_to_short);
    RUN_TEST(test_conservative_preset);
    RUN_TEST(test_aggressive_preset);
    RUN_TEST(test_market_regime_enum_values);
    RUN_TEST(test_strategy_type_enum_values);
    RUN_TEST(test_cross_thread_visibility);
    RUN_TEST(test_regime_to_string);

    std::cout << "\n=== All Regime-Strategy Mapping Tests Passed! ===\n";
    return 0;
}
