/**
 * Test Symbol-Specific Position Sizing
 *
 * Each symbol has its own position sizing parameters in SymbolTuningConfig.
 * ConfigStrategy uses these directly for position calculations.
 */

#include "../include/ipc/shared_config.hpp"
#include "../include/ipc/symbol_config.hpp"

#include <cassert>
#include <cstring>
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

#define ASSERT_EQ(a, b)                                                                                                \
    do {                                                                                                               \
        if ((a) != (b)) {                                                                                              \
            std::cerr << "\nFAIL: " << #a << " (" << (a) << ") != " << #b << " (" << (b) << ")\n";                     \
            assert(false);                                                                                             \
        }                                                                                                              \
    } while (0)

#define ASSERT_TRUE(expr)                                                                                              \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            std::cerr << "\nFAIL: " << #expr << " is false\n";                                                         \
            assert(false);                                                                                             \
        }                                                                                                              \
    } while (0)

#define ASSERT_NEAR(a, b, epsilon)                                                                                     \
    do {                                                                                                               \
        double diff = std::abs((a) - (b));                                                                             \
        if (diff > (epsilon)) {                                                                                        \
            std::cerr << "\nFAIL: " << #a << " (" << (a) << ") not near " << #b << " (" << (b) << "), diff=" << diff   \
                      << "\n";                                                                                         \
            assert(false);                                                                                             \
        }                                                                                                              \
    } while (0)

// =============================================================================
// TEST 1: Default position sizing values
// =============================================================================
TEST(symbol_config_default_position_sizing) {
    using namespace hft::ipc;

    SymbolTuningConfig cfg;
    cfg.init("BTCUSDT");

    // Default values (from defaults.hpp)
    ASSERT_NEAR(cfg.base_position_pct(), 2.0, 0.01); // 2%
    ASSERT_NEAR(cfg.max_position_pct(), 5.0, 0.01);  // 5%
    ASSERT_NEAR(cfg.min_position_pct(), 1.0, 0.01);  // 1% (MIN_POSITION_X100 = 100)
}

// =============================================================================
// TEST 2: Custom position sizing per symbol
// =============================================================================
TEST(symbol_config_custom_position_sizing) {
    using namespace hft::ipc;

    SharedSymbolConfigs configs;
    configs.init();

    // BTC: aggressive
    auto* btc = configs.get_or_create("BTCUSDT");
    ASSERT_TRUE(btc != nullptr);
    btc->base_position_x100 = 400; // 4%
    btc->max_position_x100 = 1000; // 10%
    btc->min_position_x100 = 100;  // 1%

    // ETH: conservative
    auto* eth = configs.get_or_create("ETHUSDT");
    ASSERT_TRUE(eth != nullptr);
    eth->base_position_x100 = 100; // 1%
    eth->max_position_x100 = 300;  // 3%
    eth->min_position_x100 = 50;   // 0.5%

    // Verify BTC values
    ASSERT_NEAR(btc->base_position_pct(), 4.0, 0.01);
    ASSERT_NEAR(btc->max_position_pct(), 10.0, 0.01);
    ASSERT_NEAR(btc->min_position_pct(), 1.0, 0.01);

    // Verify ETH values
    ASSERT_NEAR(eth->base_position_pct(), 1.0, 0.01);
    ASSERT_NEAR(eth->max_position_pct(), 3.0, 0.01);
    ASSERT_NEAR(eth->min_position_pct(), 0.5, 0.01);
}

// =============================================================================
// TEST 3: Update position sizing via SharedSymbolConfigs
// =============================================================================
TEST(symbol_configs_update_position_sizing) {
    using namespace hft::ipc;

    SharedSymbolConfigs configs;
    configs.init();

    // Create initial config
    auto* btc = configs.get_or_create("BTCUSDT");
    ASSERT_TRUE(btc != nullptr);
    ASSERT_NEAR(btc->base_position_pct(), 2.0, 0.01); // Default

    // Create update with new values
    SymbolTuningConfig new_cfg;
    new_cfg.init("BTCUSDT");
    new_cfg.base_position_x100 = 500; // 5%
    new_cfg.max_position_x100 = 1500; // 15%
    new_cfg.min_position_x100 = 200;  // 2%

    // Apply update
    bool updated = configs.update("BTCUSDT", new_cfg);
    ASSERT_TRUE(updated);

    // Verify new values
    btc = configs.get_or_create("BTCUSDT");
    ASSERT_NEAR(btc->base_position_pct(), 5.0, 0.01);
    ASSERT_NEAR(btc->max_position_pct(), 15.0, 0.01);
    ASSERT_NEAR(btc->min_position_pct(), 2.0, 0.01);
}

// =============================================================================
// TEST 4: Each symbol is independent
// =============================================================================
TEST(symbols_have_independent_configs) {
    using namespace hft::ipc;

    SharedSymbolConfigs configs;
    configs.init();

    // Create configs for multiple symbols
    auto* btc = configs.get_or_create("BTCUSDT");
    auto* eth = configs.get_or_create("ETHUSDT");
    auto* sol = configs.get_or_create("SOLUSDT");

    // Set different values for each
    btc->base_position_x100 = 400;
    eth->base_position_x100 = 200;
    sol->base_position_x100 = 100;

    // Verify each has its own value
    ASSERT_NEAR(configs.find("BTCUSDT")->base_position_pct(), 4.0, 0.01);
    ASSERT_NEAR(configs.find("ETHUSDT")->base_position_pct(), 2.0, 0.01);
    ASSERT_NEAR(configs.find("SOLUSDT")->base_position_pct(), 1.0, 0.01);

    // Modifying one doesn't affect others
    btc->base_position_x100 = 600;
    ASSERT_NEAR(configs.find("BTCUSDT")->base_position_pct(), 6.0, 0.01);
    ASSERT_NEAR(configs.find("ETHUSDT")->base_position_pct(), 2.0, 0.01); // Unchanged
    ASSERT_NEAR(configs.find("SOLUSDT")->base_position_pct(), 1.0, 0.01); // Unchanged
}

// =============================================================================
// MAIN
// =============================================================================
int main() {
    std::cout << "Running Symbol Position Sizing Tests:\n";

    RUN_TEST(symbol_config_default_position_sizing);
    RUN_TEST(symbol_config_custom_position_sizing);
    RUN_TEST(symbol_configs_update_position_sizing);
    RUN_TEST(symbols_have_independent_configs);

    std::cout << "\nAll tests passed!\n";
    return 0;
}
