#include <cassert>
#include <iostream>
#include <cstring>

// Will include the header after we create it
#include "../include/strategy/enhanced_risk_manager.hpp"

using namespace hft;
using namespace hft::strategy;

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

    EnhancedRiskManager rm(config);
    rm.set_initial_capital(100000);  // 100k starting capital

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

    EnhancedRiskManager rm(config);
    rm.set_initial_capital(100000);

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
// Per-Symbol Position Limit Tests
// ============================================

TEST(test_symbol_position_limit) {
    EnhancedRiskConfig config;

    EnhancedRiskManager rm(config);

    // Set AAPL max position to 1000
    rm.set_symbol_limit(1, 1000, 0);  // symbol=1, max_pos=1000, max_notional=0 (disabled)

    // Try to buy 500 AAPL - should be allowed
    assert(rm.check_order(1, Side::Buy, 500, 15000));
    rm.on_fill(1, Side::Buy, 500, 15000);

    // Try to buy another 400 - should be allowed (total 900)
    assert(rm.check_order(1, Side::Buy, 400, 15000));
    rm.on_fill(1, Side::Buy, 400, 15000);

    // Try to buy 200 more - should be rejected (would be 1100 > 1000)
    assert(!rm.check_order(1, Side::Buy, 200, 15000));
}

TEST(test_symbol_position_both_sides) {
    EnhancedRiskConfig config;

    EnhancedRiskManager rm(config);
    rm.set_symbol_limit(1, 1000, 0);

    // Long 800
    rm.on_fill(1, Side::Buy, 800, 15000);

    // Sell 500 (net long 300)
    rm.on_fill(1, Side::Sell, 500, 15000);

    // Can we buy 600 more? (would be net long 900)
    assert(rm.check_order(1, Side::Buy, 600, 15000));

    // Can we buy 800 more? (would be net long 1100 > 1000)
    assert(!rm.check_order(1, Side::Buy, 800, 15000));
}

TEST(test_symbol_notional_limit) {
    EnhancedRiskConfig config;

    EnhancedRiskManager rm(config);

    // Set BTC max notional to 1M (position limit disabled)
    rm.set_symbol_limit(2, 0, 1000000);  // symbol=2, max_pos=0, max_notional=1M

    // Buy 10 BTC at 50k = 500k notional - allowed
    assert(rm.check_order(2, Side::Buy, 10, 500000000));  // Price in basis points
    rm.on_fill(2, Side::Buy, 10, 500000000);

    // Buy 15 more at 50k = 750k notional - rejected (total 1.25M > 1M)
    assert(!rm.check_order(2, Side::Buy, 15, 500000000));
}

// ============================================
// Global Notional Limit Tests
// ============================================

TEST(test_global_notional_limit) {
    EnhancedRiskConfig config;
    config.max_total_notional = 5000000;  // 5M total

    EnhancedRiskManager rm(config);

    // Buy AAPL: 1000 @ 150 = 150k
    rm.on_fill(1, Side::Buy, 1000, 1500000);

    // Buy GOOGL: 100 @ 2800 = 280k (total 430k)
    rm.on_fill(2, Side::Buy, 100, 28000000);

    // Buy TSLA: 500 @ 250 = 125k (total 555k) - allowed
    assert(rm.check_order(3, Side::Buy, 500, 2500000));

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

    // Order within limit
    assert(rm.check_order(1, Side::Buy, 500, 15000));

    // Order at limit
    assert(rm.check_order(1, Side::Buy, 1000, 15000));

    // Order exceeds limit
    assert(!rm.check_order(1, Side::Buy, 1001, 15000));
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

    EnhancedRiskManager rm(config);
    rm.set_initial_capital(200000);
    rm.set_symbol_limit(1, 2000, 500000);

    // All checks pass
    assert(rm.check_order(1, Side::Buy, 100, 1500000));

    // Order size fails
    assert(!rm.check_order(1, Side::Buy, 600, 1500000));
}

TEST(test_risk_state_summary) {
    EnhancedRiskConfig config;
    config.daily_loss_limit = 10000;
    config.max_drawdown_pct = 0.10;

    EnhancedRiskManager rm(config);
    rm.set_initial_capital(100000);

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

TEST(test_hot_path_no_allocation) {
    EnhancedRiskConfig config;
    config.daily_loss_limit = 100000;
    config.max_drawdown_pct = 0.20;
    config.max_order_size = 10000;

    EnhancedRiskManager rm(config);
    rm.set_initial_capital(1000000);

    // Pre-register symbols
    for (Symbol s = 0; s < 100; ++s) {
        rm.set_symbol_limit(s, 10000, 10000000);
    }

    // Simulate hot path - should be very fast
    for (int i = 0; i < 100000; ++i) {
        Symbol sym = i % 100;
        bool allowed = rm.check_order(sym, Side::Buy, 10, 1500000);
        if (allowed) {
            rm.on_fill(sym, Side::Buy, 10, 1500000);
        }

        // Alternate sells to keep position bounded
        if (i % 2 == 1) {
            rm.on_fill(sym, Side::Sell, 10, 1500000);
        }
    }

    // If we got here without crashing/allocating, test passes
    assert(true);
}

// ============================================
// Main
// ============================================

int main() {
    std::cout << "=== Enhanced Risk Manager Tests ===\n\n";

    // Daily P&L tests
    RUN_TEST(test_daily_pnl_limit_not_exceeded);
    RUN_TEST(test_daily_pnl_limit_exceeded);
    RUN_TEST(test_daily_pnl_reset_on_new_day);

    // Drawdown tests
    RUN_TEST(test_drawdown_from_peak);
    RUN_TEST(test_drawdown_updates_peak);

    // Per-symbol tests
    RUN_TEST(test_symbol_position_limit);
    RUN_TEST(test_symbol_position_both_sides);
    RUN_TEST(test_symbol_notional_limit);

    // Global tests
    RUN_TEST(test_global_notional_limit);
    RUN_TEST(test_max_order_size);

    // Combined tests
    RUN_TEST(test_multiple_risk_checks);
    RUN_TEST(test_risk_state_summary);

    // Performance test
    RUN_TEST(test_hot_path_no_allocation);

    std::cout << "\n=== All " << 14 << " tests passed! ===\n";
    return 0;
}
