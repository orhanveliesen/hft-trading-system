/**
 * Test SymbolTuningConfig - Per-symbol configuration
 *
 * Each symbol has independent configuration including:
 * - Mode thresholds (losses_to_cautious, wins_to_aggressive, etc.)
 * - Position sizing (base, min, max position)
 * - Signal thresholds
 * - Performance tracking (streak, total trades, win rate)
 */

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

#define ASSERT_FALSE(expr)                                                                                             \
    do {                                                                                                               \
        if ((expr)) {                                                                                                  \
            std::cerr << "\nFAIL: " << #expr << " is true (expected false)\n";                                         \
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

// =============================================================================
// TEST 1: Default values after init
// =============================================================================
TEST(symbol_tuning_config_default_values) {
    using namespace hft::ipc;

    SymbolTuningConfig cfg;
    cfg.init("BTCUSDT");

    // Symbol name
    ASSERT_EQ(std::string(cfg.symbol), "BTCUSDT");
    ASSERT_TRUE(cfg.is_enabled());

    // Mode thresholds (from defaults.hpp)
    ASSERT_EQ(cfg.losses_to_cautious, 2);
    ASSERT_EQ(cfg.losses_to_defensive, 4);
    ASSERT_EQ(cfg.losses_to_pause, 5);
    ASSERT_EQ(cfg.losses_to_exit_only, 6);
    ASSERT_EQ(cfg.wins_to_aggressive, 3);

    // Initial state (zeroed by memset in init())
    ASSERT_EQ(cfg.consecutive_losses, 0);
    ASSERT_EQ(cfg.consecutive_wins, 0);
    ASSERT_EQ(cfg.current_mode, 0); // AGGRESSIVE (default from memset)

    // Performance
    ASSERT_EQ(cfg.total_trades, 0);
    ASSERT_EQ(cfg.winning_trades, 0);
}

// =============================================================================
// TEST 2: Mode threshold accessors
// =============================================================================
TEST(symbol_tuning_config_threshold_accessors) {
    using namespace hft::ipc;

    SymbolTuningConfig cfg;
    cfg.init("ETHUSDT");

    // Signal thresholds
    ASSERT_NEAR(cfg.signal_threshold_aggressive(), 0.3, 0.01);
    ASSERT_NEAR(cfg.signal_threshold_normal(), 0.5, 0.01);
    ASSERT_NEAR(cfg.signal_threshold_cautious(), 0.7, 0.01);

    // Sharpe thresholds
    ASSERT_NEAR(cfg.sharpe_aggressive(), 1.0, 0.01);
    ASSERT_NEAR(cfg.sharpe_cautious(), 0.3, 0.01);
    ASSERT_NEAR(cfg.sharpe_defensive(), 0.0, 0.01);

    // Win rate thresholds (0-100 scale)
    ASSERT_NEAR(cfg.win_rate_aggressive_threshold(), 60.0, 0.01); // 60%
    ASSERT_NEAR(cfg.win_rate_cautious_threshold(), 40.0, 0.01);   // 40%
}

// =============================================================================
// TEST 3: Record trade updates streak and stats
// =============================================================================
TEST(symbol_tuning_config_record_trade) {
    using namespace hft::ipc;

    SymbolTuningConfig cfg;
    cfg.init("SOLUSDT");

    // Record a winning trade
    cfg.record_trade(true, 1.5); // 1.5% profit
    ASSERT_EQ(cfg.consecutive_wins, 1);
    ASSERT_EQ(cfg.consecutive_losses, 0);
    ASSERT_EQ(cfg.total_trades, 1);
    ASSERT_EQ(cfg.winning_trades, 1);
    ASSERT_NEAR(cfg.win_rate(), 100.0, 0.01); // 100% (0-100 scale)

    // Record another winning trade
    cfg.record_trade(true, 2.0);
    ASSERT_EQ(cfg.consecutive_wins, 2);
    ASSERT_EQ(cfg.total_trades, 2);
    ASSERT_EQ(cfg.winning_trades, 2);

    // Record a losing trade - resets win streak
    cfg.record_trade(false, -1.0); // 1% loss
    ASSERT_EQ(cfg.consecutive_wins, 0);
    ASSERT_EQ(cfg.consecutive_losses, 1);
    ASSERT_EQ(cfg.total_trades, 3);
    ASSERT_EQ(cfg.winning_trades, 2);
    ASSERT_NEAR(cfg.win_rate(), 66.67, 0.1); // 2/3 = 66.67% (0-100 scale)
}

// =============================================================================
// TEST 4: SharedSymbolConfigs management
// =============================================================================
TEST(shared_symbol_configs_create_and_find) {
    using namespace hft::ipc;

    SharedSymbolConfigs configs;
    configs.init();

    // Get or create new config
    auto* btc = configs.get_or_create("BTCUSDT");
    ASSERT_TRUE(btc != nullptr);
    ASSERT_EQ(std::string(btc->symbol), "BTCUSDT");

    // Find existing config
    auto* btc2 = configs.find("BTCUSDT");
    ASSERT_TRUE(btc2 != nullptr);
    ASSERT_TRUE(btc == btc2); // Same pointer

    // Find non-existent returns nullptr
    auto* unknown = configs.find("UNKNOWN");
    ASSERT_TRUE(unknown == nullptr);

    // Get or create multiple symbols
    auto* eth = configs.get_or_create("ETHUSDT");
    auto* sol = configs.get_or_create("SOLUSDT");
    ASSERT_TRUE(eth != nullptr);
    ASSERT_TRUE(sol != nullptr);
    ASSERT_TRUE(eth != btc);
    ASSERT_TRUE(sol != eth);
}

// =============================================================================
// TEST 5: SharedSymbolConfigs update
// =============================================================================
TEST(shared_symbol_configs_update) {
    using namespace hft::ipc;

    SharedSymbolConfigs configs;
    configs.init();

    // Create initial config
    auto* cfg = configs.get_or_create("BTCUSDT");
    ASSERT_TRUE(cfg != nullptr);
    ASSERT_EQ(cfg->losses_to_cautious, 2); // Default

    // Create new config with different values
    SymbolTuningConfig new_cfg;
    new_cfg.init("BTCUSDT");
    new_cfg.losses_to_cautious = 3;  // Changed from default 2
    new_cfg.losses_to_defensive = 5; // Changed from default 4
    new_cfg.target_pct_x100 = 400;   // 4%

    // Update
    bool updated = configs.update("BTCUSDT", new_cfg);
    ASSERT_TRUE(updated);

    // Verify changes
    cfg = configs.get_or_create("BTCUSDT");
    ASSERT_EQ(cfg->losses_to_cautious, 3);
    ASSERT_EQ(cfg->losses_to_defensive, 5);
    ASSERT_EQ(cfg->target_pct_x100, 400);
}

// =============================================================================
// TEST 6: Position sizing accessors
// =============================================================================
TEST(symbol_tuning_config_position_sizing) {
    using namespace hft::ipc;

    SymbolTuningConfig cfg;
    cfg.init("BTCUSDT");

    // Check default position sizing (from defaults.hpp)
    ASSERT_NEAR(cfg.base_position_pct(), 2.0, 0.1); // Default 2%
    ASSERT_NEAR(cfg.max_position_pct(), 5.0, 0.1);  // Default 5%
    ASSERT_NEAR(cfg.min_position_pct(), 1.0, 0.1);  // Default 1% (MIN_POSITION_X100=100)

    // Modify and check
    cfg.base_position_x100 = 300; // 3%
    cfg.max_position_x100 = 1000; // 10%
    cfg.min_position_x100 = 100;  // 1%

    ASSERT_NEAR(cfg.base_position_pct(), 3.0, 0.1);
    ASSERT_NEAR(cfg.max_position_pct(), 10.0, 0.1);
    ASSERT_NEAR(cfg.min_position_pct(), 1.0, 0.1);
}

// =============================================================================
// TEST 7: Cooldown value bounds (prevent int16_t overflow)
// =============================================================================
TEST(symbol_tuning_config_cooldown_bounds) {
    using namespace hft::ipc;

    SymbolTuningConfig cfg;
    cfg.init("BTCUSDT");

    // Default cooldown should be reasonable
    ASSERT_TRUE(cfg.cooldown_ms > 0);
    ASSERT_TRUE(cfg.cooldown_ms <= 30000); // Max 30 seconds is reasonable

    // Test setting valid cooldown
    cfg.set_cooldown_ms(5000); // 5 seconds
    ASSERT_EQ(cfg.cooldown_ms, 5000);

    // Test clamping large values (must not overflow to negative)
    cfg.set_cooldown_ms(45000);            // Larger than int16_t max (32767)
    ASSERT_TRUE(cfg.cooldown_ms > 0);      // Must never be negative
    ASSERT_TRUE(cfg.cooldown_ms <= 32767); // Clamped to int16_t max

    // Test clamping negative values
    cfg.set_cooldown_ms(-1000);
    ASSERT_TRUE(cfg.cooldown_ms > 0); // Must not allow negative

    // Verify it's clamped to a sensible minimum
    cfg.set_cooldown_ms(0);
    ASSERT_TRUE(cfg.cooldown_ms >= 100); // At least 100ms
}

// =============================================================================
// MAIN
// =============================================================================
int main() {
    std::cout << "Running SymbolTuningConfig Tests:\n";

    RUN_TEST(symbol_tuning_config_default_values);
    RUN_TEST(symbol_tuning_config_threshold_accessors);
    RUN_TEST(symbol_tuning_config_record_trade);
    RUN_TEST(shared_symbol_configs_create_and_find);
    RUN_TEST(shared_symbol_configs_update);
    RUN_TEST(symbol_tuning_config_position_sizing);
    RUN_TEST(symbol_tuning_config_cooldown_bounds);

    std::cout << "\nAll tests passed!\n";
    return 0;
}
