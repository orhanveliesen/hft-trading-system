/**
 * Test SymbolTuningConfig - use_global_flags behavior
 *
 * When tuner sets symbol-specific config, use_global_flags should be cleared
 * so the reading code uses symbol-specific values instead of global.
 */

#include <iostream>
#include <cassert>
#include <cstring>

#include "../include/ipc/symbol_config.hpp"

#define TEST(name) void name()

#define RUN_TEST(name) do { \
    std::cout << "  " << #name << "... "; \
    try { \
        name(); \
        std::cout << "PASSED\n"; \
    } catch (...) { \
        std::cout << "FAILED (exception)\n"; \
        return 1; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        std::cerr << "\nFAIL: " << #a << " (" << (a) << ") != " << #b << " (" << (b) << ")\n"; \
        assert(false); \
    } \
} while(0)

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        std::cerr << "\nFAIL: " << #expr << " is false\n"; \
        assert(false); \
    } \
} while(0)

#define ASSERT_FALSE(expr) do { \
    if ((expr)) { \
        std::cerr << "\nFAIL: " << #expr << " is true (expected false)\n"; \
        assert(false); \
    } \
} while(0)

// =============================================================================
// TEST 1: Default use_global_flags should be 0x0F (use global for all)
// =============================================================================
TEST(symbol_tuning_config_default_use_global) {
    using namespace hft::ipc;

    SymbolTuningConfig cfg;
    cfg.init("BTCUSDT");

    // Default: use global for all config groups
    ASSERT_EQ(cfg.use_global_flags, 0x0F);
    ASSERT_TRUE(cfg.use_global_position());
    ASSERT_TRUE(cfg.use_global_target());
    ASSERT_TRUE(cfg.use_global_filtering());
    ASSERT_TRUE(cfg.use_global_ema());
}

// =============================================================================
// TEST 2: set_use_global_* methods work correctly
// =============================================================================
TEST(symbol_tuning_config_set_use_global_methods) {
    using namespace hft::ipc;

    SymbolTuningConfig cfg;
    cfg.init("BTCUSDT");

    // Clear target flag
    cfg.set_use_global_target(false);
    ASSERT_FALSE(cfg.use_global_target());
    ASSERT_TRUE(cfg.use_global_position());  // Others unchanged
    ASSERT_EQ(cfg.use_global_flags, 0x0D);   // 0x0F & ~0x02 = 0x0D

    // Set it back
    cfg.set_use_global_target(true);
    ASSERT_TRUE(cfg.use_global_target());
    ASSERT_EQ(cfg.use_global_flags, 0x0F);
}

// =============================================================================
// TEST 3: SharedSymbolConfigs::update should clear use_global_flags for
//         the config groups that were updated
//
// BUG: Currently this test FAILS because update() doesn't clear flags
// =============================================================================
TEST(symbol_configs_update_clears_use_global_target) {
    using namespace hft::ipc;

    // Create configs directly (not via shared memory for unit test)
    SharedSymbolConfigs configs;
    configs.init();

    // Create initial symbol config
    auto* cfg = configs.get_or_create("BTCUSDT");
    ASSERT_TRUE(cfg != nullptr);
    ASSERT_TRUE(cfg->use_global_target());  // Initially uses global

    // Create a new config with symbol-specific target/stop
    SymbolTuningConfig new_cfg;
    new_cfg.init("BTCUSDT");
    new_cfg.target_pct_x100 = 400;  // 4% (different from default 3%)
    new_cfg.stop_pct_x100 = 600;    // 6% (different from default 5%)
    // Signal that we're providing symbol-specific target values
    new_cfg.set_use_global_target(false);

    // The update should preserve the use_global_target flag from the new config
    bool updated = configs.update("BTCUSDT", new_cfg);
    ASSERT_TRUE(updated);

    // After update with symbol-specific target/stop,
    // use_global_target should be FALSE
    cfg = const_cast<SymbolTuningConfig*>(configs.find("BTCUSDT"));
    ASSERT_TRUE(cfg != nullptr);

    // THIS IS THE BUG: use_global_target() still returns true
    // because update() doesn't preserve the use_global_flags from new_cfg
    ASSERT_FALSE(cfg->use_global_target());  // Should be false after symbol-specific update

    // Values should be the new symbol-specific ones
    ASSERT_EQ(cfg->target_pct_x100, 400);
    ASSERT_EQ(cfg->stop_pct_x100, 600);
}

// =============================================================================
// TEST 4: SharedSymbolConfigs::update should preserve use_global for
//         config groups that weren't changed (if only target/stop changed)
// =============================================================================
TEST(symbol_configs_update_preserves_other_use_global) {
    using namespace hft::ipc;

    SharedSymbolConfigs configs;
    configs.init();

    auto* cfg = configs.get_or_create("BTCUSDT");
    ASSERT_TRUE(cfg != nullptr);

    // Create new config with only target/stop changed
    // (keep position sizing, ema, filtering using global)
    SymbolTuningConfig new_cfg;
    new_cfg.init("BTCUSDT");
    new_cfg.target_pct_x100 = 400;
    new_cfg.stop_pct_x100 = 600;
    // Clear only target flag, keep others using global
    new_cfg.set_use_global_target(false);
    new_cfg.set_use_global_position(true);
    new_cfg.set_use_global_filtering(true);
    new_cfg.set_use_global_ema(true);

    configs.update("BTCUSDT", new_cfg);

    cfg = const_cast<SymbolTuningConfig*>(configs.find("BTCUSDT"));
    ASSERT_TRUE(cfg != nullptr);

    // Target flag should be cleared (we set specific values)
    ASSERT_FALSE(cfg->use_global_target());

    // Other flags should remain as they were (using global)
    ASSERT_TRUE(cfg->use_global_position());
    ASSERT_TRUE(cfg->use_global_filtering());
    ASSERT_TRUE(cfg->use_global_ema());
}

// =============================================================================
// MAIN
// =============================================================================
int main() {
    std::cout << "Running SymbolTuningConfig Tests:\n";

    RUN_TEST(symbol_tuning_config_default_use_global);
    RUN_TEST(symbol_tuning_config_set_use_global_methods);
    RUN_TEST(symbol_configs_update_clears_use_global_target);
    RUN_TEST(symbol_configs_update_preserves_other_use_global);

    std::cout << "\nAll tests passed!\n";
    return 0;
}
