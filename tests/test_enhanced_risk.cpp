#include <cassert>
#include <iostream>
#include <cstring>

#include "../include/risk/enhanced_risk_manager.hpp"

using namespace hft;
using namespace hft::risk;

// ============================================
// Test Helpers
// ============================================

#define TEST(name) void name()
#define RUN_TEST(name) do { \
    std::cout << "Running " << #name << "... "; \
    name(); \
    std::cout << "PASSED\n"; \
} while(0)

// ============================================
// Daily P&L Limit Tests
// ============================================

TEST(test_daily_pnl_limit_not_exceeded) {
    EnhancedRiskConfig config;
    config.daily_loss_limit = 10000;  // Max 10k loss per day

    EnhancedRiskManager rm(config);

    // Start of day P&L = 0, current P&L = -5000
    rm.update_pnl(-5000);

    // Should still allow trading (5k loss < 10k limit)
    assert(rm.can_trade());
    assert(!rm.is_daily_limit_breached());
}

TEST(test_daily_pnl_limit_exceeded) {
    EnhancedRiskConfig config;
    config.daily_loss_limit = 10000;

    EnhancedRiskManager rm(config);

    // Lose more than daily limit
    rm.update_pnl(-15000);

    // Should halt trading
    assert(!rm.can_trade());
    assert(rm.is_daily_limit_breached());
}

TEST(test_daily_pnl_reset_on_new_day) {
    EnhancedRiskConfig config;
    config.daily_loss_limit = 10000;

    EnhancedRiskManager rm(config);

    // Day 1: Lose 8k (within limit)
    rm.update_pnl(-8000);
    assert(rm.can_trade());

    // Day 2: Reset daily tracking
    rm.new_trading_day();

    // Now at -8000 total, but daily loss is 0
    // Lose another 5k today
    rm.update_pnl(-13000);  // Total -13k, but today only -5k

    assert(rm.can_trade());  // Today's loss (5k) < limit (10k)
}

// ============================================
// Max Drawdown Tests
// ============================================

TEST(test_drawdown_from_peak) {
    EnhancedRiskConfig config;
    config.max_drawdown_pct = 0.10;  // 10% max drawdown
    config.initial_capital = 100000;  // 100k starting capital

    EnhancedRiskManager rm(config);

    // Make profit first - peak is now 110k
    rm.update_pnl(10000);
    assert(rm.can_trade());

    // Small drawdown (5k from peak = 4.5%)
    rm.update_pnl(5000);
    assert(rm.can_trade());
    assert(!rm.is_drawdown_breached());

    // Large drawdown (15k from peak = 13.6% > 10%)
    rm.update_pnl(-5000);  // Now at 95k, peak was 110k
    assert(!rm.can_trade());
    assert(rm.is_drawdown_breached());
}

TEST(test_drawdown_updates_peak) {
    EnhancedRiskConfig config;
    config.max_drawdown_pct = 0.20;  // 20% max drawdown
    config.initial_capital = 100000;

    EnhancedRiskManager rm(config);

    // Profit: 100k -> 120k (new peak)
    rm.update_pnl(20000);
    assert(rm.peak_equity() == 120000);

    // Drawdown: 120k -> 110k (8.3% drawdown)
    rm.update_pnl(10000);
    assert(rm.peak_equity() == 120000);  // Peak unchanged

    // New high: 110k -> 130k (new peak)
    rm.update_pnl(30000);
    assert(rm.peak_equity() == 130000);  // New peak
}

// ============================================
// Per-Symbol Position Limit Tests (Hot Path with Index)
// ============================================

TEST(test_symbol_position_limit_hot_path) {
    EnhancedRiskConfig config;
    EnhancedRiskManager rm(config);

    // Register symbol and get index for hot path
    SymbolIndex aapl_idx = rm.register_symbol("AAPL", 1000, 0);

    // Try to buy 500 AAPL - should be allowed
    assert(rm.check_order(aapl_idx, Side::Buy, 500, 15000));
    rm.on_fill(aapl_idx, Side::Buy, 500, 15000);

    // Try to buy another 400 - should be allowed (total 900)
    assert(rm.check_order(aapl_idx, Side::Buy, 400, 15000));
    rm.on_fill(aapl_idx, Side::Buy, 400, 15000);

    // Try to buy 200 more - should be rejected (would be 1100 > 1000)
    assert(!rm.check_order(aapl_idx, Side::Buy, 200, 15000));

    // Verify position
    assert(rm.symbol_position(aapl_idx) == 900);
}

TEST(test_symbol_position_limit_cold_path) {
    EnhancedRiskConfig config;
    EnhancedRiskManager rm(config);

    // Use string-based API (convenience, not for hot path)
    rm.set_symbol_limit("AAPL", 1000, 0);

    // String-based check_order
    assert(rm.check_order("AAPL", Side::Buy, 500, 15000));
    rm.on_fill("AAPL", Side::Buy, 500, 15000);

    assert(rm.check_order("AAPL", Side::Buy, 400, 15000));
    rm.on_fill("AAPL", Side::Buy, 400, 15000);

    // Should be rejected
    assert(!rm.check_order("AAPL", Side::Buy, 200, 15000));

    // Verify position via string
    assert(rm.symbol_position("AAPL") == 900);
}

TEST(test_symbol_position_both_sides) {
    EnhancedRiskConfig config;
    EnhancedRiskManager rm(config);

    SymbolIndex idx = rm.register_symbol("AAPL", 1000, 0);

    // Long 800
    rm.on_fill(idx, Side::Buy, 800, 15000);

    // Sell 500 (net long 300)
    rm.on_fill(idx, Side::Sell, 500, 15000);

    // Can we buy 600 more? (would be net long 900)
    assert(rm.check_order(idx, Side::Buy, 600, 15000));

    // Can we buy 800 more? (would be net long 1100 > 1000)
    assert(!rm.check_order(idx, Side::Buy, 800, 15000));
}

TEST(test_symbol_notional_limit) {
    EnhancedRiskConfig config;
    EnhancedRiskManager rm(config);

    // Register BTC with max notional 1M
    SymbolIndex btc_idx = rm.register_symbol("BTCUSDT", 0, 1000000);

    // Buy 10 BTC at 50k = 500k notional - allowed
    assert(rm.check_order(btc_idx, Side::Buy, 10, 500000000));
    rm.on_fill(btc_idx, Side::Buy, 10, 500000000);

    // Buy 15 more at 50k = 750k notional - rejected (total > 1M)
    assert(!rm.check_order(btc_idx, Side::Buy, 15, 500000000));
}

// ============================================
// Global Notional Limit Tests
// ============================================

TEST(test_global_notional_limit) {
    EnhancedRiskConfig config;
    config.max_total_notional = 5000000;  // 5M total

    EnhancedRiskManager rm(config);

    SymbolIndex aapl = rm.register_symbol("AAPL");
    SymbolIndex googl = rm.register_symbol("GOOGL");
    SymbolIndex tsla = rm.register_symbol("TSLA");

    // Buy AAPL: 1000 @ 150 = 150k
    rm.on_fill(aapl, Side::Buy, 1000, 1500000);

    // Buy GOOGL: 100 @ 2800 = 280k
    rm.on_fill(googl, Side::Buy, 100, 28000000);

    // Buy TSLA: 500 @ 250 = 125k - allowed
    assert(rm.check_order(tsla, Side::Buy, 500, 2500000));

    // Total notional check
    assert(rm.total_notional() < config.max_total_notional);
}

// ============================================
// Order Size Limit Tests
// ============================================

TEST(test_max_order_size) {
    EnhancedRiskConfig config;
    config.max_order_size = 1000;

    EnhancedRiskManager rm(config);
    SymbolIndex idx = rm.register_symbol("AAPL");

    // Order within limit
    assert(rm.check_order(idx, Side::Buy, 500, 15000));

    // Order at limit
    assert(rm.check_order(idx, Side::Buy, 1000, 15000));

    // Order exceeds limit
    assert(!rm.check_order(idx, Side::Buy, 1001, 15000));
}

// ============================================
// Combined Risk Checks
// ============================================

TEST(test_multiple_risk_checks) {
    EnhancedRiskConfig config;
    config.daily_loss_limit = 50000;
    config.max_drawdown_pct = 0.15;
    config.max_order_size = 500;
    config.max_total_notional = 1000000;
    config.initial_capital = 200000;

    EnhancedRiskManager rm(config);
    SymbolIndex idx = rm.register_symbol("AAPL", 2000, 500000);

    // All checks pass
    assert(rm.check_order(idx, Side::Buy, 100, 1500000));

    // Order size fails
    assert(!rm.check_order(idx, Side::Buy, 600, 1500000));
}

TEST(test_risk_state_summary) {
    EnhancedRiskConfig config;
    config.daily_loss_limit = 10000;
    config.max_drawdown_pct = 0.10;
    config.initial_capital = 100000;

    EnhancedRiskManager rm(config);

    rm.update_pnl(5000);  // Profit
    rm.update_pnl(-2000); // Small loss

    auto state = rm.build_state();

    assert(state.current_pnl == -2000);
    assert(state.peak_equity == 105000);
    assert(state.daily_pnl == -2000);
    assert(state.can_trade == true);
}

// ============================================
// Hot Path Performance Test
// ============================================

TEST(test_hot_path_performance) {
    EnhancedRiskConfig config;
    config.daily_loss_limit = 100000;
    config.max_drawdown_pct = 0.20;
    config.max_order_size = 10000;
    config.initial_capital = 1000000;

    EnhancedRiskManager rm(config);

    // Pre-register symbols and cache indices (simulates startup)
    rm.reserve_symbols(100);
    std::vector<SymbolIndex> indices;
    for (int s = 0; s < 100; ++s) {
        std::string symbol = "SYM" + std::to_string(s);
        SymbolIndex idx = rm.register_symbol(symbol, 10000, 10000000);
        indices.push_back(idx);
    }

    // Simulate hot path with cached indices - no string lookups!
    for (int i = 0; i < 100000; ++i) {
        SymbolIndex idx = indices[i % 100];
        bool allowed = rm.check_order(idx, Side::Buy, 10, 1500000);
        if (allowed) {
            rm.on_fill(idx, Side::Buy, 10, 1500000);
        }

        // Alternate sells to keep position bounded
        if (i % 2 == 1) {
            rm.on_fill(idx, Side::Sell, 10, 1500000);
        }
    }

    // If we got here without issues, test passes
    assert(rm.symbol_count() == 100);
}

// ============================================
// Symbol Index Caching Test (ITCH-style)
// ============================================

TEST(test_itch_style_symbol_mapping) {
    EnhancedRiskConfig config;
    config.initial_capital = 1000000;

    EnhancedRiskManager rm(config);

    // Simulate ITCH Symbol Directory - build locate -> index mapping
    // In real code, this would be: std::array<SymbolIndex, 65536> locate_to_index;
    std::vector<SymbolIndex> locate_to_index(1000, INVALID_SYMBOL_INDEX);

    // Symbol Directory messages arrive at startup
    locate_to_index[123] = rm.register_symbol("AAPL", 5000, 0);
    locate_to_index[456] = rm.register_symbol("MSFT", 3000, 0);
    locate_to_index[789] = rm.register_symbol("GOOGL", 1000, 0);

    // Hot path - use cached index, no string lookup
    SymbolIndex aapl = locate_to_index[123];
    assert(rm.check_order(aapl, Side::Buy, 100, 1500000));
    rm.on_fill(aapl, Side::Buy, 100, 1500000);

    // Verify
    assert(rm.symbol_position(aapl) == 100);
    assert(rm.get_symbol_name(aapl) == "AAPL");
}

// ============================================
// Config-based initialization test
// ============================================

TEST(test_config_based_initialization) {
    EnhancedRiskConfig config;
    config.initial_capital = 500000;
    config.daily_loss_limit = 25000;
    config.max_drawdown_pct = 0.08;
    config.max_order_size = 5000;

    EnhancedRiskManager rm(config);

    // Verify config was applied
    assert(rm.peak_equity() == 500000);
    assert(rm.config().daily_loss_limit == 25000);
    assert(rm.config().max_order_size == 5000);

    // Register a test symbol
    SymbolIndex idx = rm.register_symbol("TEST");

    // Test order size from config
    assert(rm.check_order(idx, Side::Buy, 5000, 10000));
    assert(!rm.check_order(idx, Side::Buy, 5001, 10000));
}

// ============================================
// Main
// ============================================

int main() {
    std::cout << "=== Enhanced Risk Manager Tests (Hybrid) ===\n\n";

    // Daily P&L tests
    RUN_TEST(test_daily_pnl_limit_not_exceeded);
    RUN_TEST(test_daily_pnl_limit_exceeded);
    RUN_TEST(test_daily_pnl_reset_on_new_day);

    // Drawdown tests
    RUN_TEST(test_drawdown_from_peak);
    RUN_TEST(test_drawdown_updates_peak);

    // Per-symbol tests (both hot and cold path)
    RUN_TEST(test_symbol_position_limit_hot_path);
    RUN_TEST(test_symbol_position_limit_cold_path);
    RUN_TEST(test_symbol_position_both_sides);
    RUN_TEST(test_symbol_notional_limit);

    // Global tests
    RUN_TEST(test_global_notional_limit);
    RUN_TEST(test_max_order_size);

    // Combined tests
    RUN_TEST(test_multiple_risk_checks);
    RUN_TEST(test_risk_state_summary);

    // Performance test
    RUN_TEST(test_hot_path_performance);

    // ITCH-style mapping test
    RUN_TEST(test_itch_style_symbol_mapping);

    // Config tests
    RUN_TEST(test_config_based_initialization);

    std::cout << "\n=== All 16 tests passed! ===\n";
    return 0;
}
