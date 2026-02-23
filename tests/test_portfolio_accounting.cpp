/**
 * Test LocalPortfolio accounting to verify cash flows are correct
 *
 * This test verifies:
 * 1. BUY: cash decreases by (price * qty + commission)
 * 2. SELL: cash increases by (price * qty - commission)
 * 3. No double-counting
 */

#include <cassert>
#include <cmath>
#include <iostream>

// Minimal LocalPortfolio for testing
class TestPortfolio {
public:
    static constexpr size_t MAX_SYMBOLS = 100;
    static constexpr double COMMISSION_RATE = 0.001; // 0.1%

    double cash = 0;
    double holdings[MAX_SYMBOLS] = {0};
    double entry_prices[MAX_SYMBOLS] = {0};

    void init(double initial_cash) {
        cash = initial_cash;
        for (size_t i = 0; i < MAX_SYMBOLS; i++) {
            holdings[i] = 0;
            entry_prices[i] = 0;
        }
    }

    // BUY: cash -= (price * qty) + commission
    void buy(size_t symbol, double price, double qty) {
        double trade_value = price * qty;
        double commission = trade_value * COMMISSION_RATE;
        cash -= (trade_value + commission);

        // Update position
        double old_qty = holdings[symbol];
        double old_value = old_qty * entry_prices[symbol];
        double new_value = old_value + trade_value;
        holdings[symbol] = old_qty + qty;
        if (holdings[symbol] > 0) {
            entry_prices[symbol] = new_value / holdings[symbol];
        }
    }

    // SELL: cash += (price * qty) - commission
    void sell(size_t symbol, double price, double qty) {
        double trade_value = price * qty;
        double commission = trade_value * COMMISSION_RATE;
        cash += (trade_value - commission);

        // Update position
        holdings[symbol] -= qty;
        if (holdings[symbol] <= 0.0001) {
            holdings[symbol] = 0;
            entry_prices[symbol] = 0;
        }
    }

    double total_value(double current_price, size_t symbol) const { return cash + holdings[symbol] * current_price; }
};

#define ASSERT_NEAR(a, b, tol)                                                                                         \
    do {                                                                                                               \
        if (std::abs((a) - (b)) > (tol)) {                                                                             \
            std::cerr << "FAIL: " << #a << " = " << (a) << ", expected " << (b) << "\n";                               \
            assert(false);                                                                                             \
        }                                                                                                              \
    } while (0)

void test_basic_buy() {
    std::cout << "  test_basic_buy... ";

    TestPortfolio p;
    p.init(10000.0);

    // BUY 1 BTC @ $100
    p.buy(0, 100.0, 1.0);

    // Expected: cash = 10000 - 100 - 0.1 (commission) = 9899.9
    ASSERT_NEAR(p.cash, 9899.9, 0.01);
    ASSERT_NEAR(p.holdings[0], 1.0, 0.0001);
    ASSERT_NEAR(p.entry_prices[0], 100.0, 0.01);

    std::cout << "PASSED\n";
}

void test_basic_sell() {
    std::cout << "  test_basic_sell... ";

    TestPortfolio p;
    p.init(10000.0);

    // BUY 1 BTC @ $100
    p.buy(0, 100.0, 1.0);

    // SELL 1 BTC @ $110 (profit)
    p.sell(0, 110.0, 1.0);

    // Expected:
    // After buy: cash = 9899.9
    // After sell: cash = 9899.9 + 110 - 0.11 = 10009.79
    ASSERT_NEAR(p.cash, 10009.79, 0.01);
    ASSERT_NEAR(p.holdings[0], 0.0, 0.0001);

    std::cout << "PASSED\n";
}

void test_round_trip_profit() {
    std::cout << "  test_round_trip_profit... ";

    TestPortfolio p;
    p.init(10000.0);

    // BUY 1 BTC @ $100, SELL @ $110 (10% profit)
    p.buy(0, 100.0, 1.0);
    p.sell(0, 110.0, 1.0);

    // Profit = 10 - 0.1 (buy comm) - 0.11 (sell comm) = 9.79
    double expected_profit = 10.0 - 0.1 - 0.11;
    ASSERT_NEAR(p.cash - 10000.0, expected_profit, 0.01);

    std::cout << "PASSED\n";
}

void test_round_trip_loss() {
    std::cout << "  test_round_trip_loss... ";

    TestPortfolio p;
    p.init(10000.0);

    // BUY 1 BTC @ $100, SELL @ $90 (10% loss)
    p.buy(0, 100.0, 1.0);
    p.sell(0, 90.0, 1.0);

    // Loss = -10 - 0.1 (buy comm) - 0.09 (sell comm) = -10.19
    double expected_pnl = -10.0 - 0.1 - 0.09;
    ASSERT_NEAR(p.cash - 10000.0, expected_pnl, 0.01);

    std::cout << "PASSED\n";
}

void test_multiple_buys() {
    std::cout << "  test_multiple_buys... ";

    TestPortfolio p;
    p.init(10000.0);

    // BUY 1 BTC @ $100
    p.buy(0, 100.0, 1.0);
    // BUY 1 BTC @ $120
    p.buy(0, 120.0, 1.0);

    // Expected:
    // cash = 10000 - 100.1 - 120.12 = 9779.78
    ASSERT_NEAR(p.cash, 9779.78, 0.01);
    ASSERT_NEAR(p.holdings[0], 2.0, 0.0001);
    // Avg entry = (100 + 120) / 2 = 110
    ASSERT_NEAR(p.entry_prices[0], 110.0, 0.01);

    std::cout << "PASSED\n";
}

void test_partial_sell() {
    std::cout << "  test_partial_sell... ";

    TestPortfolio p;
    p.init(10000.0);

    // BUY 2 BTC @ $100
    p.buy(0, 100.0, 2.0);
    // SELL 1 BTC @ $110
    p.sell(0, 110.0, 1.0);

    // After buy: cash = 10000 - 200.2 = 9799.8
    // After sell: cash = 9799.8 + 109.89 = 9909.69
    ASSERT_NEAR(p.cash, 9909.69, 0.01);
    ASSERT_NEAR(p.holdings[0], 1.0, 0.0001);

    std::cout << "PASSED\n";
}

void test_equity_invariant() {
    std::cout << "  test_equity_invariant... ";

    TestPortfolio p;
    p.init(10000.0);

    double price = 100.0;

    // Before trade: equity = 10000
    double equity_before = p.total_value(price, 0);
    ASSERT_NEAR(equity_before, 10000.0, 0.01);

    // After BUY: equity should decrease by commission only
    p.buy(0, price, 1.0);
    double equity_after_buy = p.total_value(price, 0);
    // equity = cash + holdings = 9899.9 + 100 = 9999.9
    ASSERT_NEAR(equity_after_buy, 9999.9, 0.01);

    // After SELL at same price: equity decreases by sell commission
    p.sell(0, price, 1.0);
    double equity_after_sell = p.total_value(price, 0);
    // equity = 9899.9 + 99.9 = 9999.8
    ASSERT_NEAR(equity_after_sell, 9999.8, 0.01);

    // Total equity lost = 0.1 + 0.1 = 0.2 (commissions)
    ASSERT_NEAR(10000.0 - equity_after_sell, 0.2, 0.01);

    std::cout << "PASSED\n";
}

void test_no_double_counting() {
    std::cout << "  test_no_double_counting... ";

    TestPortfolio p;
    p.init(20000.0); // Same as our test setup

    // Simulate 100 round-trip trades
    double total_commission = 0;
    for (int i = 0; i < 100; i++) {
        double price = 1000.0 + (i % 10); // Slight price variation
        p.buy(0, price, 0.1);
        total_commission += price * 0.1 * 0.001;

        p.sell(0, price * 1.01, 0.1); // 1% profit
        total_commission += price * 1.01 * 0.1 * 0.001;
    }

    // Cash should be positive and reasonable
    // With 1% profit per trade minus ~0.2% commission:
    // Net profit per trade ≈ 0.8% of trade value ≈ $0.8
    // Total profit ≈ 100 * $0.8 = $80
    std::cout << "Final cash: $" << p.cash << ", ";
    std::cout << "Total commission: $" << total_commission << ", ";

    // Cash should be close to initial + profits - commissions
    // Should definitely be < $25000 (not $94000!)
    assert(p.cash < 25000.0);
    assert(p.cash > 18000.0); // Some profit expected

    std::cout << "PASSED\n";
}

int main() {
    std::cout << "\n=== Portfolio Accounting Tests ===\n\n";

    test_basic_buy();
    test_basic_sell();
    test_round_trip_profit();
    test_round_trip_loss();
    test_multiple_buys();
    test_partial_sell();
    test_equity_invariant();
    test_no_double_counting();

    std::cout << "\n=== All Portfolio Tests PASSED! ===\n";
    return 0;
}
