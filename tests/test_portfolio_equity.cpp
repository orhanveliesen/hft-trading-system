#include "../include/ipc/shared_portfolio_state.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace hft::ipc;

#define TEST(name) void name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "  " << #name << "... ";                                                                          \
        name();                                                                                                        \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

#define ASSERT_NEAR(a, b, eps) assert(std::abs((a) - (b)) < (eps))
#define ASSERT_EQ(a, b) assert((a) == (b))

constexpr const char* TEST_SHM_NAME = "/portfolio_equity_test";

// ============================================================================
// BUG-001: Portfolio Equity Calculation
//
// total_equity MUST equal cash + market_value (sum of qty * current_price)
// NOT cash + unrealized_pnl (which is qty * (current_price - avg_price))
// ============================================================================

TEST(equity_equals_cash_plus_market_value) {
    // Cleanup any previous test
    SharedPortfolioState::destroy(TEST_SHM_NAME);

    // Create portfolio with known values
    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 10000.0);
    assert(state != nullptr);

    // Setup: cash = 10000, position = 1 BTC @ avg $50000, current $60000
    // Expected:
    //   market_value = 1 * 60000 = 60000
    //   unrealized_pnl = 1 * (60000 - 50000) = 10000
    //   total_equity = cash + market_value = 10000 + 60000 = 70000
    //   WRONG would be: cash + unrealized_pnl = 10000 + 10000 = 20000

    state->update_position("BTCUSDT", 1.0, 50000.0, 60000.0);

    double cash = state->cash();
    double market_value = state->total_market_value();
    double unrealized_pnl = state->total_unrealized_pnl();
    double equity = state->total_equity();

    // Verify the formula
    ASSERT_NEAR(cash, 10000.0, 0.01);
    ASSERT_NEAR(market_value, 60000.0, 0.01);
    ASSERT_NEAR(unrealized_pnl, 10000.0, 0.01);

    // CRITICAL: equity must be cash + market_value, NOT cash + unrealized_pnl
    ASSERT_NEAR(equity, 70000.0, 0.01);          // cash + market_value
    assert(std::abs(equity - 20000.0) > 1000.0); // Must NOT be cash + unrealized_pnl

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(equity_with_multiple_positions) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);

    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 5000.0);
    assert(state != nullptr);

    // Position 1: 2 ETH @ avg $2000, current $2500
    //   market_value = 2 * 2500 = 5000
    //   unrealized_pnl = 2 * (2500 - 2000) = 1000
    state->update_position("ETHUSDT", 2.0, 2000.0, 2500.0);

    // Position 2: 100 SOL @ avg $100, current $80 (loss)
    //   market_value = 100 * 80 = 8000
    //   unrealized_pnl = 100 * (80 - 100) = -2000
    state->update_position("SOLUSDT", 100.0, 100.0, 80.0);

    // Expected totals:
    //   total_market_value = 5000 + 8000 = 13000
    //   total_unrealized_pnl = 1000 + (-2000) = -1000
    //   total_equity = cash + market_value = 5000 + 13000 = 18000
    //   WRONG would be: cash + unrealized_pnl = 5000 + (-1000) = 4000

    double equity = state->total_equity();
    double wrong_equity = state->cash() + state->total_unrealized_pnl();

    ASSERT_NEAR(equity, 18000.0, 0.01);
    ASSERT_NEAR(wrong_equity, 4000.0, 0.01);

    // These should be very different
    assert(std::abs(equity - wrong_equity) > 10000.0);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(equity_with_zero_positions) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);

    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 25000.0);
    assert(state != nullptr);

    // No positions - equity should equal cash
    double equity = state->total_equity();
    ASSERT_NEAR(equity, 25000.0, 0.01);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

int main() {
    std::cout << "Running portfolio equity tests (BUG-001):\n";

    RUN_TEST(equity_equals_cash_plus_market_value);
    RUN_TEST(equity_with_multiple_positions);
    RUN_TEST(equity_with_zero_positions);

    std::cout << "\nAll portfolio equity tests passed!\n";
    return 0;
}
