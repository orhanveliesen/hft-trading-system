#include "../include/trading/risk_check.hpp"
#include "../include/trading/trading_state.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace hft::trading;

#define TEST(name) void name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "  " << #name << "... ";                                                                          \
        name();                                                                                                        \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

#define ASSERT_EQ(a, b) assert((a) == (b))
#define ASSERT_TRUE(x) assert(x)
#define ASSERT_FALSE(x) assert(!(x))
#define ASSERT_NEAR(a, b, eps) assert(std::abs((a) - (b)) < (eps))

// =============================================================================
// check_risk Tests
// =============================================================================

TEST(check_risk_passes_when_running) {
    TradingState state{};
    state.init(100000.0);

    // Should pass when system is running
    ASSERT_TRUE(check_risk(0, Side::Buy, 0.1, 95000.0, state));
}

TEST(check_risk_fails_when_halted) {
    TradingState state{};
    state.init(100000.0);

    state.halt.halted.store(static_cast<uint8_t>(HaltStatus::HALTED));

    // Should fail when halted
    ASSERT_FALSE(check_risk(0, Side::Buy, 0.1, 95000.0, state));
}

TEST(check_risk_fails_when_risk_halted) {
    TradingState state{};
    state.init(100000.0);

    state.risk_state.risk_halted.store(1);

    // Should fail when risk halted
    ASSERT_FALSE(check_risk(0, Side::Buy, 0.1, 95000.0, state));
}

TEST(check_risk_respects_position_limit) {
    TradingState state{};
    state.init(100000.0);

    constexpr size_t SYM = 0;
    state.risk_limits.max_position[SYM] = 10; // Max 10 units
    state.positions.quantity[SYM] = 8.0;      // Current 8 units

    // Buying 2 more should pass (total 10)
    ASSERT_TRUE(check_risk(SYM, Side::Buy, 2.0, 95000.0, state));

    // Buying 3 more should fail (total 11)
    ASSERT_FALSE(check_risk(SYM, Side::Buy, 3.0, 95000.0, state));
}

TEST(check_risk_respects_notional_limit) {
    TradingState state{};
    state.init(100000.0);

    constexpr size_t SYM = 0;
    state.risk_limits.max_notional[SYM] = static_cast<int64_t>(100000 * FIXED_POINT_SCALE);
    state.risk_limits.current_notional[SYM] = static_cast<int64_t>(50000 * FIXED_POINT_SCALE);

    // Order notional: 0.5 * 95000 = 47500 -> total 97500 < 100000 -> pass
    ASSERT_TRUE(check_risk(SYM, Side::Buy, 0.5, 95000.0, state));

    // Order notional: 1.0 * 95000 = 95000 -> total 145000 > 100000 -> fail
    ASSERT_FALSE(check_risk(SYM, Side::Buy, 1.0, 95000.0, state));
}

TEST(check_risk_no_limit_when_zero) {
    TradingState state{};
    state.init(100000.0);

    constexpr size_t SYM = 0;
    // max_position = 0 means no limit
    state.risk_limits.max_position[SYM] = 0;
    state.risk_limits.max_notional[SYM] = 0;

    // Large order should pass with no limits
    ASSERT_TRUE(check_risk(SYM, Side::Buy, 1000.0, 95000.0, state));
}

TEST(check_risk_sell_reduces_position) {
    TradingState state{};
    state.init(100000.0);

    constexpr size_t SYM = 0;
    state.risk_limits.max_position[SYM] = 10;
    state.positions.quantity[SYM] = 8.0;

    // Selling should always pass (reduces position)
    ASSERT_TRUE(check_risk(SYM, Side::Sell, 5.0, 95000.0, state));
}

// =============================================================================
// update_risk_on_fill Tests
// =============================================================================

TEST(update_risk_increases_notional_on_buy) {
    TradingState state{};
    state.init(100000.0);

    constexpr size_t SYM = 0;
    state.risk_limits.current_notional[SYM] = 0;

    // Buy 0.5 @ 95000 = 47500 notional
    update_risk_on_fill(SYM, Side::Buy, 0.5, 95000.0, 0.0, state);

    int64_t expected = static_cast<int64_t>(47500.0 * FIXED_POINT_SCALE);
    ASSERT_EQ(state.risk_limits.current_notional[SYM], expected);
}

TEST(update_risk_decreases_notional_on_sell) {
    TradingState state{};
    state.init(100000.0);

    constexpr size_t SYM = 0;
    state.risk_limits.current_notional[SYM] = static_cast<int64_t>(100000 * FIXED_POINT_SCALE);

    // Sell 0.5 @ 95000 = 47500 notional decrease
    update_risk_on_fill(SYM, Side::Sell, 0.5, 95000.0, 0.0, state);

    int64_t expected = static_cast<int64_t>(52500.0 * FIXED_POINT_SCALE);
    ASSERT_EQ(state.risk_limits.current_notional[SYM], expected);
}

TEST(update_risk_tracks_daily_pnl) {
    TradingState state{};
    state.init(100000.0);

    constexpr size_t SYM = 0;

    // Realized profit of $500
    update_risk_on_fill(SYM, Side::Sell, 0.5, 96000.0, 500.0, state);

    int64_t expected_pnl = static_cast<int64_t>(500.0 * FIXED_POINT_SCALE);
    ASSERT_EQ(state.risk_state.daily_pnl_x8.load(), expected_pnl);
}

TEST(update_risk_triggers_halt_on_loss_limit) {
    TradingState state{};
    state.init(100000.0);

    // Set daily loss limit to 1000
    state.risk_state.daily_loss_limit_x8.store(static_cast<int64_t>(1000.0 * FIXED_POINT_SCALE));

    constexpr size_t SYM = 0;

    // Lose $1500 - should trigger halt
    update_risk_on_fill(SYM, Side::Sell, 0.5, 94000.0, -1500.0, state);

    ASSERT_EQ(state.risk_state.risk_halted.load(), 1u);
    ASSERT_EQ(state.halt.halted.load(), static_cast<uint8_t>(HaltStatus::HALTING));
    ASSERT_EQ(state.halt.reason.load(), static_cast<uint8_t>(HaltReason::RISK_LIMIT));
}

TEST(update_risk_no_halt_within_limit) {
    TradingState state{};
    state.init(100000.0);

    // Set daily loss limit to 2000
    state.risk_state.daily_loss_limit_x8.store(static_cast<int64_t>(2000.0 * FIXED_POINT_SCALE));

    constexpr size_t SYM = 0;

    // Lose $1000 - should NOT trigger halt
    update_risk_on_fill(SYM, Side::Sell, 0.5, 94000.0, -1000.0, state);

    ASSERT_EQ(state.risk_state.risk_halted.load(), 0u);
    ASSERT_EQ(state.halt.halted.load(), static_cast<uint8_t>(HaltStatus::RUNNING));
}

// =============================================================================
// calculate_drawdown Tests
// =============================================================================

TEST(calculate_drawdown_zero_when_at_peak) {
    TradingState state{};
    state.init(100000.0);

    state.risk_state.peak_equity_x8.store(static_cast<int64_t>(100000 * FIXED_POINT_SCALE));
    int64_t current_equity = static_cast<int64_t>(100000 * FIXED_POINT_SCALE);

    double dd = calculate_drawdown(current_equity, state);
    ASSERT_NEAR(dd, 0.0, 1e-9);
}

TEST(calculate_drawdown_correct_percentage) {
    TradingState state{};
    state.init(100000.0);

    state.risk_state.peak_equity_x8.store(static_cast<int64_t>(100000 * FIXED_POINT_SCALE));
    int64_t current_equity = static_cast<int64_t>(90000 * FIXED_POINT_SCALE); // 10% drawdown

    double dd = calculate_drawdown(current_equity, state);
    ASSERT_NEAR(dd, 0.10, 1e-9);
}

TEST(calculate_drawdown_updates_peak) {
    TradingState state{};
    state.init(100000.0);

    state.risk_state.peak_equity_x8.store(static_cast<int64_t>(100000 * FIXED_POINT_SCALE));
    int64_t new_high = static_cast<int64_t>(110000 * FIXED_POINT_SCALE); // New high

    double dd = calculate_drawdown(new_high, state);
    ASSERT_NEAR(dd, 0.0, 1e-9);
    ASSERT_EQ(state.risk_state.peak_equity_x8.load(), new_high);
}

// =============================================================================
// check_drawdown_halt Tests
// =============================================================================

TEST(check_drawdown_halt_triggers_at_threshold) {
    TradingState state{};
    state.init(100000.0);

    state.risk_state.peak_equity_x8.store(static_cast<int64_t>(100000 * FIXED_POINT_SCALE));
    state.risk_state.max_drawdown_pct.store(0.10); // 10% max drawdown

    // 15% drawdown should trigger halt
    int64_t current_equity = static_cast<int64_t>(85000 * FIXED_POINT_SCALE);

    bool halted = check_drawdown_halt(current_equity, state);
    ASSERT_TRUE(halted);
    ASSERT_EQ(state.risk_state.risk_halted.load(), 1u);
}

TEST(check_drawdown_halt_no_trigger_within_threshold) {
    TradingState state{};
    state.init(100000.0);

    state.risk_state.peak_equity_x8.store(static_cast<int64_t>(100000 * FIXED_POINT_SCALE));
    state.risk_state.max_drawdown_pct.store(0.10); // 10% max drawdown

    // 5% drawdown should NOT trigger halt
    int64_t current_equity = static_cast<int64_t>(95000 * FIXED_POINT_SCALE);

    bool halted = check_drawdown_halt(current_equity, state);
    ASSERT_FALSE(halted);
    ASSERT_EQ(state.risk_state.risk_halted.load(), 0u);
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "\n=== Risk Check Tests ===\n\n";

    std::cout << "check_risk Tests:\n";
    RUN_TEST(check_risk_passes_when_running);
    RUN_TEST(check_risk_fails_when_halted);
    RUN_TEST(check_risk_fails_when_risk_halted);
    RUN_TEST(check_risk_respects_position_limit);
    RUN_TEST(check_risk_respects_notional_limit);
    RUN_TEST(check_risk_no_limit_when_zero);
    RUN_TEST(check_risk_sell_reduces_position);

    std::cout << "\nupdate_risk_on_fill Tests:\n";
    RUN_TEST(update_risk_increases_notional_on_buy);
    RUN_TEST(update_risk_decreases_notional_on_sell);
    RUN_TEST(update_risk_tracks_daily_pnl);
    RUN_TEST(update_risk_triggers_halt_on_loss_limit);
    RUN_TEST(update_risk_no_halt_within_limit);

    std::cout << "\ncalculate_drawdown Tests:\n";
    RUN_TEST(calculate_drawdown_zero_when_at_peak);
    RUN_TEST(calculate_drawdown_correct_percentage);
    RUN_TEST(calculate_drawdown_updates_peak);

    std::cout << "\ncheck_drawdown_halt Tests:\n";
    RUN_TEST(check_drawdown_halt_triggers_at_threshold);
    RUN_TEST(check_drawdown_halt_no_trigger_within_threshold);

    std::cout << "\n=== All Risk Check Tests Passed! ===\n";
    return 0;
}
