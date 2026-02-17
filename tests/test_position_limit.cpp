/**
 * test_position_limit.cpp - Position limit enforcement tests
 *
 * Tests for BUG: When MAX_POSITIONS_PER_SYMBOL is reached, the system should:
 * 1. Refuse to add more positions
 * 2. Not deduct cash when position can't be added
 * 3. Provide a way to check capacity BEFORE sending orders
 *
 * This test was created after discovering that the trader kept sending orders
 * after hitting 32 positions, causing cash to get stuck.
 */

#include <iostream>
#include <cassert>
#include <cmath>

#include "../include/trading/portfolio.hpp"
#include "../include/types.hpp"

using namespace hft;
using namespace hft::trading;

#define ASSERT_NEAR(a, b, tol) do { \
    if (std::abs((a) - (b)) > (tol)) { \
        std::cerr << "FAIL: " << #a << " = " << (a) << ", expected " << (b) << "\n"; \
        assert(false); \
    } \
} while(0)

void test_buy_at_position_limit() {
    std::cout << "  test_buy_at_position_limit... ";

    Portfolio p;
    p.init(1000000.0);  // $1M to avoid cash limits

    Symbol BTC = 0;
    double price = 100.0;
    double qty = 0.1;

    // Fill up to MAX_POSITIONS_PER_SYMBOL
    for (size_t i = 0; i < MAX_POSITIONS_PER_SYMBOL; i++) {
        double commission = p.buy(BTC, price, qty);
        assert(commission > 0);  // Should succeed
    }

    double cash_before = p.cash;
    double holding_before = p.get_holding(BTC);

    // Try to add one more - should fail
    double commission = p.buy(BTC, price, qty);

    // Should return 0 (failed)
    assert(commission == 0);

    // Cash should NOT change
    ASSERT_NEAR(p.cash, cash_before, 0.01);

    // Holdings should NOT change
    ASSERT_NEAR(p.get_holding(BTC), holding_before, 0.0001);

    std::cout << "PASSED\n";
}

void test_can_add_position_method_exists() {
    std::cout << "  test_can_add_position_method... ";

    Portfolio p;
    p.init(100000.0);

    Symbol BTC = 0;

    // Should be able to add positions initially
    bool can_add = p.can_add_position(BTC);
    assert(can_add == true);

    // Fill up positions
    for (size_t i = 0; i < MAX_POSITIONS_PER_SYMBOL; i++) {
        p.buy(BTC, 100.0, 0.1);
    }

    // Should NOT be able to add more
    can_add = p.can_add_position(BTC);
    assert(can_add == false);

    std::cout << "PASSED\n";
}

void test_different_symbols_independent_limits() {
    std::cout << "  test_different_symbols_independent_limits... ";

    Portfolio p;
    p.init(1000000.0);

    Symbol BTC = 0;
    Symbol ETH = 1;

    // Fill up BTC positions
    for (size_t i = 0; i < MAX_POSITIONS_PER_SYMBOL; i++) {
        p.buy(BTC, 100.0, 0.1);
    }

    // BTC should be full
    assert(p.can_add_position(BTC) == false);

    // ETH should still have room
    assert(p.can_add_position(ETH) == true);

    // Should be able to buy ETH
    double commission = p.buy(ETH, 100.0, 0.1);
    assert(commission > 0);

    std::cout << "PASSED\n";
}

void test_position_limit_after_sell() {
    std::cout << "  test_position_limit_after_sell... ";

    Portfolio p;
    p.init(1000000.0);

    Symbol BTC = 0;
    double price = 100.0;
    double qty = 0.1;

    // Fill up positions
    for (size_t i = 0; i < MAX_POSITIONS_PER_SYMBOL; i++) {
        p.buy(BTC, price, qty);
    }

    // Should be full
    assert(p.can_add_position(BTC) == false);

    // Sell all
    double total_qty = p.get_holding(BTC);
    p.sell(BTC, price, total_qty);

    // Should have room again
    assert(p.can_add_position(BTC) == true);

    // Should be able to buy
    double commission = p.buy(BTC, price, qty);
    assert(commission > 0);

    std::cout << "PASSED\n";
}

void test_cash_integrity_at_limit() {
    std::cout << "  test_cash_integrity_at_limit... ";

    Portfolio p;
    p.init(100000.0);

    Symbol BTC = 0;
    double price = 100.0;
    double qty = 1.0;

    double initial_cash = p.cash;
    double total_spent = 0;

    // Buy up to limit
    for (size_t i = 0; i < MAX_POSITIONS_PER_SYMBOL; i++) {
        double cash_before = p.cash;
        double commission = p.buy(BTC, price, qty);
        double spent = cash_before - p.cash;
        total_spent += spent;
    }

    // Try to buy more (should fail, no cash deduction)
    double cash_at_limit = p.cash;
    for (int i = 0; i < 100; i++) {
        p.buy(BTC, price, qty);
    }

    // Cash should not have changed
    ASSERT_NEAR(p.cash, cash_at_limit, 0.01);

    // Verify total accounting
    double expected_cash = initial_cash - total_spent;
    ASSERT_NEAR(p.cash, expected_cash, 0.01);

    std::cout << "PASSED\n";
}

void test_position_count_at_limit() {
    std::cout << "  test_position_count_at_limit... ";

    Portfolio p;
    p.init(1000000.0);

    Symbol BTC = 0;

    // Fill up positions
    for (size_t i = 0; i < MAX_POSITIONS_PER_SYMBOL; i++) {
        p.buy(BTC, 100.0, 0.1);
    }

    // Should have exactly MAX positions
    assert(p.positions[BTC].count == MAX_POSITIONS_PER_SYMBOL);

    // Trying to add more should not increase count
    p.buy(BTC, 100.0, 0.1);
    p.buy(BTC, 100.0, 0.1);
    p.buy(BTC, 100.0, 0.1);

    assert(p.positions[BTC].count == MAX_POSITIONS_PER_SYMBOL);

    std::cout << "PASSED\n";
}

int main() {
    std::cout << "\n=== Position Limit Tests ===\n";
    std::cout << "MAX_POSITIONS_PER_SYMBOL = " << MAX_POSITIONS_PER_SYMBOL << "\n\n";

    test_buy_at_position_limit();
    test_can_add_position_method_exists();
    test_different_symbols_independent_limits();
    test_position_limit_after_sell();
    test_cash_integrity_at_limit();
    test_position_count_at_limit();

    std::cout << "\n=== All Position Limit Tests PASSED! ===\n";
    return 0;
}
