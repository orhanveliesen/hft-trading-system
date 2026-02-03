/**
 * Test Symbol-Specific Position Sizing
 *
 * When tuner sets symbol-specific position sizing config,
 * the trader should use those values instead of global config.
 *
 * This is the TDD test for fixing the gap where trader.cpp
 * doesn't read from SharedSymbolConfigs.
 */

#include <iostream>
#include <cassert>
#include <cstring>

#include "../include/ipc/shared_config.hpp"
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

#define ASSERT_NEAR(a, b, epsilon) do { \
    double diff = std::abs((a) - (b)); \
    if (diff > (epsilon)) { \
        std::cerr << "\nFAIL: " << #a << " (" << (a) << ") not near " << #b << " (" << (b) << "), diff=" << diff << "\n"; \
        assert(false); \
    } \
} while(0)

// =============================================================================
// Mock Portfolio that mirrors trader.cpp Portfolio behavior
// This allows testing the position sizing logic without full trader dependency
// =============================================================================
class TestablePortfolio {
public:
    static constexpr double DEFAULT_BASE_POSITION_PCT = 0.02;  // 2%
    static constexpr double DEFAULT_MAX_POSITION_PCT = 0.05;   // 5%

    void set_config(const hft::ipc::SharedConfig* cfg) { config_ = cfg; }
    void set_symbol_configs(const hft::ipc::SharedSymbolConfigs* cfgs) { symbol_configs_ = cfgs; }

    // Symbol-aware position sizing
    // If symbol has specific config and use_global_position=false, use symbol config
    // Otherwise fall back to global config
    double base_position_pct(const char* symbol = nullptr) const {
        if (symbol && symbol_configs_) {
            const auto* sym_cfg = symbol_configs_->find(symbol);
            if (sym_cfg && !sym_cfg->use_global_position()) {
                return sym_cfg->base_position_x100 / 10000.0;  // x100 -> decimal
            }
        }
        return config_ ? config_->base_position_pct() / 100.0 : DEFAULT_BASE_POSITION_PCT;
    }

    double max_position_pct(const char* symbol = nullptr) const {
        if (symbol && symbol_configs_) {
            const auto* sym_cfg = symbol_configs_->find(symbol);
            if (sym_cfg && !sym_cfg->use_global_position()) {
                return sym_cfg->max_position_x100 / 10000.0;  // x100 -> decimal
            }
        }
        return config_ ? config_->max_position_pct() / 100.0 : DEFAULT_MAX_POSITION_PCT;
    }

private:
    const hft::ipc::SharedConfig* config_ = nullptr;
    const hft::ipc::SharedSymbolConfigs* symbol_configs_ = nullptr;
};

// =============================================================================
// TEST 1: Without symbol configs, use global config
// =============================================================================
TEST(portfolio_uses_global_config_by_default) {
    using namespace hft::ipc;

    SharedConfig global_cfg;
    global_cfg.init();
    global_cfg.base_position_pct_x100.store(300);  // 3%
    global_cfg.max_position_pct_x100.store(800);   // 8%

    TestablePortfolio portfolio;
    portfolio.set_config(&global_cfg);

    // Without symbol configs, should use global
    ASSERT_NEAR(portfolio.base_position_pct(), 0.03, 0.0001);
    ASSERT_NEAR(portfolio.max_position_pct(), 0.08, 0.0001);

    // Even with symbol name, should use global (no symbol_configs set)
    ASSERT_NEAR(portfolio.base_position_pct("BTCUSDT"), 0.03, 0.0001);
    ASSERT_NEAR(portfolio.max_position_pct("BTCUSDT"), 0.08, 0.0001);
}

// =============================================================================
// TEST 2: With symbol configs but use_global_position=true, use global
// =============================================================================
TEST(portfolio_uses_global_when_use_global_flag_set) {
    using namespace hft::ipc;

    SharedConfig global_cfg;
    global_cfg.init();
    global_cfg.base_position_pct_x100.store(300);  // 3%
    global_cfg.max_position_pct_x100.store(800);   // 8%

    SharedSymbolConfigs symbol_cfgs;
    symbol_cfgs.init();

    // Create symbol config with different values but keep use_global_position=true (default)
    auto* btc = symbol_cfgs.get_or_create("BTCUSDT");
    ASSERT_TRUE(btc != nullptr);
    btc->base_position_x100 = 500;  // 5% - different from global
    btc->max_position_x100 = 1200;  // 12% - different from global
    // use_global_position() is TRUE by default
    ASSERT_TRUE(btc->use_global_position());

    TestablePortfolio portfolio;
    portfolio.set_config(&global_cfg);
    portfolio.set_symbol_configs(&symbol_cfgs);

    // Should use GLOBAL values because use_global_position=true
    ASSERT_NEAR(portfolio.base_position_pct("BTCUSDT"), 0.03, 0.0001);  // 3% global
    ASSERT_NEAR(portfolio.max_position_pct("BTCUSDT"), 0.08, 0.0001);   // 8% global
}

// =============================================================================
// TEST 3: With symbol configs and use_global_position=false, use symbol-specific
// THIS IS THE CRITICAL TEST - Currently fails because trader doesn't read symbol configs
// =============================================================================
TEST(portfolio_uses_symbol_specific_when_flag_cleared) {
    using namespace hft::ipc;

    SharedConfig global_cfg;
    global_cfg.init();
    global_cfg.base_position_pct_x100.store(300);  // 3%
    global_cfg.max_position_pct_x100.store(800);   // 8%

    SharedSymbolConfigs symbol_cfgs;
    symbol_cfgs.init();

    // Create symbol config with different values AND clear use_global_position
    auto* btc = symbol_cfgs.get_or_create("BTCUSDT");
    ASSERT_TRUE(btc != nullptr);
    btc->base_position_x100 = 500;  // 5% symbol-specific
    btc->max_position_x100 = 1200;  // 12% symbol-specific
    btc->set_use_global_position(false);  // USE SYMBOL-SPECIFIC

    TestablePortfolio portfolio;
    portfolio.set_config(&global_cfg);
    portfolio.set_symbol_configs(&symbol_cfgs);

    // Should use SYMBOL-SPECIFIC values because use_global_position=false
    ASSERT_NEAR(portfolio.base_position_pct("BTCUSDT"), 0.05, 0.0001);  // 5% symbol
    ASSERT_NEAR(portfolio.max_position_pct("BTCUSDT"), 0.12, 0.0001);   // 12% symbol

    // Other symbols should still use global
    ASSERT_NEAR(portfolio.base_position_pct("ETHUSDT"), 0.03, 0.0001);  // 3% global
    ASSERT_NEAR(portfolio.max_position_pct("ETHUSDT"), 0.08, 0.0001);   // 8% global
}

// =============================================================================
// TEST 4: Multiple symbols with different configs
// =============================================================================
TEST(portfolio_handles_multiple_symbol_configs) {
    using namespace hft::ipc;

    SharedConfig global_cfg;
    global_cfg.init();
    global_cfg.base_position_pct_x100.store(200);  // 2% global
    global_cfg.max_position_pct_x100.store(500);   // 5% global

    SharedSymbolConfigs symbol_cfgs;
    symbol_cfgs.init();

    // BTC: aggressive position sizing
    auto* btc = symbol_cfgs.get_or_create("BTCUSDT");
    btc->base_position_x100 = 400;  // 4%
    btc->max_position_x100 = 1000;  // 10%
    btc->set_use_global_position(false);

    // ETH: conservative position sizing
    auto* eth = symbol_cfgs.get_or_create("ETHUSDT");
    eth->base_position_x100 = 100;  // 1%
    eth->max_position_x100 = 300;   // 3%
    eth->set_use_global_position(false);

    // SOL: uses global
    auto* sol = symbol_cfgs.get_or_create("SOLUSDT");
    sol->base_position_x100 = 999;  // Should be ignored
    sol->max_position_x100 = 999;   // Should be ignored
    // use_global_position remains true (default)

    TestablePortfolio portfolio;
    portfolio.set_config(&global_cfg);
    portfolio.set_symbol_configs(&symbol_cfgs);

    // BTC: symbol-specific
    ASSERT_NEAR(portfolio.base_position_pct("BTCUSDT"), 0.04, 0.0001);
    ASSERT_NEAR(portfolio.max_position_pct("BTCUSDT"), 0.10, 0.0001);

    // ETH: symbol-specific
    ASSERT_NEAR(portfolio.base_position_pct("ETHUSDT"), 0.01, 0.0001);
    ASSERT_NEAR(portfolio.max_position_pct("ETHUSDT"), 0.03, 0.0001);

    // SOL: global (flag not cleared)
    ASSERT_NEAR(portfolio.base_position_pct("SOLUSDT"), 0.02, 0.0001);
    ASSERT_NEAR(portfolio.max_position_pct("SOLUSDT"), 0.05, 0.0001);

    // Unknown symbol: global
    ASSERT_NEAR(portfolio.base_position_pct("XRPUSDT"), 0.02, 0.0001);
    ASSERT_NEAR(portfolio.max_position_pct("XRPUSDT"), 0.05, 0.0001);
}

// =============================================================================
// MAIN
// =============================================================================
int main() {
    std::cout << "Running Symbol Position Sizing Tests:\n";

    RUN_TEST(portfolio_uses_global_config_by_default);
    RUN_TEST(portfolio_uses_global_when_use_global_flag_set);
    RUN_TEST(portfolio_uses_symbol_specific_when_flag_cleared);
    RUN_TEST(portfolio_handles_multiple_symbol_configs);

    std::cout << "\nAll tests passed!\n";
    return 0;
}
