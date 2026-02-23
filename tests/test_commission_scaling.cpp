/**
 * Commission Scaling Bug Test Suite
 *
 * Tests that commission is correctly scaled when Portfolio::sell()
 * sells less than requested due to overselling protection.
 *
 * BUG DISCOVERED: When sell() scales commission internally (actual_sold < qty),
 * the caller still uses the original unscaled commission, causing accounting drift.
 *
 * Run with: ./test_commission_scaling
 */

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

// =============================================================================
// Test Macros
// =============================================================================

#define TEST(name) void test_##name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "  Running " #name "... ";                                                                        \
        test_##name();                                                                                                 \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

#define ASSERT_EQ(a, b)                                                                                                \
    do {                                                                                                               \
        if ((a) != (b)) {                                                                                              \
            std::cerr << "\nFAILED: " << #a << " != " << #b << "\n";                                                   \
            std::cerr << "  Actual: " << (a) << " != " << (b) << "\n";                                                 \
            assert(false);                                                                                             \
        }                                                                                                              \
    } while (0)

#define ASSERT_NEAR(a, b, tol)                                                                                         \
    do {                                                                                                               \
        if (std::abs((a) - (b)) > (tol)) {                                                                             \
            std::cerr << "\nFAILED: " << #a << " != " << #b << " (" << (a) << " != " << (b) << ")\n";                  \
            assert(false);                                                                                             \
        }                                                                                                              \
    } while (0)

#define ASSERT_TRUE(expr)                                                                                              \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            std::cerr << "\nFAILED: " << #expr << " is false\n";                                                       \
            assert(false);                                                                                             \
        }                                                                                                              \
    } while (0)

// =============================================================================
// Minimal Portfolio for Testing Commission Scaling
// =============================================================================

class TestPortfolio {
public:
    static constexpr size_t MAX_SYMBOLS = 100;

    double cash = 0;
    double total_commissions = 0;
    double commission_rate_ = 0.001; // 0.1%

    // Simple position tracking
    struct Position {
        double quantity = 0;
        double avg_price = 0;
    };
    Position positions[MAX_SYMBOLS];

    void init(double initial_cash) {
        cash = initial_cash;
        total_commissions = 0;
        for (auto& p : positions) {
            p.quantity = 0;
            p.avg_price = 0;
        }
    }

    void buy(uint32_t symbol, double price, double qty, double commission) {
        if (symbol >= MAX_SYMBOLS)
            return;

        auto& pos = positions[symbol];
        double cost = price * qty;

        // Update average price
        double total_cost = pos.avg_price * pos.quantity + cost;
        pos.quantity += qty;
        if (pos.quantity > 0) {
            pos.avg_price = total_cost / pos.quantity;
        }

        // Update cash and commissions
        cash -= cost + commission;
        total_commissions += commission;
    }

    // OLD SIGNATURE (BUG): void sell() - doesn't return actual commission
    // NEW SIGNATURE (FIX): returns actual_commission
    double sell(uint32_t symbol, double price, double qty, double commission) {
        if (symbol >= MAX_SYMBOLS)
            return 0;

        auto& pos = positions[symbol];

        // Overselling protection: cap at available position
        double actual_sold = std::min(qty, pos.quantity);
        if (actual_sold <= 0.0001) {
            return 0; // Nothing to sell
        }

        // Scale commission proportionally if we sold less than requested
        double actual_commission = commission;
        if (actual_sold < qty && qty > 0) {
            actual_commission = commission * (actual_sold / qty);
        }

        // Update position
        pos.quantity -= actual_sold;
        if (pos.quantity < 0.0001) {
            pos.quantity = 0;
            pos.avg_price = 0;
        }

        // Update cash and commissions
        double proceeds = price * actual_sold;
        cash += proceeds - actual_commission;
        total_commissions += actual_commission;

        // Return ACTUAL commission for caller to use
        return actual_commission;
    }

    double position_qty(uint32_t symbol) const {
        if (symbol >= MAX_SYMBOLS)
            return 0;
        return positions[symbol].quantity;
    }

    double position_avg_price(uint32_t symbol) const {
        if (symbol >= MAX_SYMBOLS)
            return 0;
        return positions[symbol].avg_price;
    }
};

// =============================================================================
// Mock SharedPortfolioState (simulates IPC tracking)
// =============================================================================

class MockPortfolioState {
public:
    double cash_ = 0;
    double total_commission_ = 0;
    double realized_pnl_ = 0;
    int fill_count_ = 0;

    void set_cash(double c) { cash_ = c; }
    void add_commission(double c) { total_commission_ += c; }
    void add_realized_pnl(double pnl) { realized_pnl_ += pnl; }
    void record_fill() { fill_count_++; }

    // P&L reconciliation check
    // Component P&L = realized_pnl - commission
    double component_pnl() const { return realized_pnl_ - total_commission_; }
};

// =============================================================================
// Tests
// =============================================================================

TEST(sell_returns_actual_commission_when_full_qty) {
    // When selling exactly what we have, commission should not be scaled
    TestPortfolio portfolio;
    portfolio.init(10000.0);

    // Buy 1.0 BTC at $50000
    portfolio.buy(0, 50000.0, 1.0, 5.0); // $5 commission

    // Sell all 1.0 BTC at $51000 with $5 commission
    double actual_commission = portfolio.sell(0, 51000.0, 1.0, 5.0);

    // Should return full commission since we sold full qty
    ASSERT_NEAR(actual_commission, 5.0, 0.001);
    ASSERT_NEAR(portfolio.total_commissions, 10.0, 0.001); // 5 + 5
}

TEST(sell_returns_scaled_commission_when_partial_qty) {
    // When selling less than requested, commission should be scaled
    TestPortfolio portfolio;
    portfolio.init(10000.0);

    // Buy 0.5 BTC at $50000
    portfolio.buy(0, 50000.0, 0.5, 2.5); // $2.5 commission

    // Try to sell 1.0 BTC (but we only have 0.5)
    // Requested commission: $5.0
    // Expected actual: $5.0 * (0.5 / 1.0) = $2.5
    double actual_commission = portfolio.sell(0, 51000.0, 1.0, 5.0);

    // Commission should be scaled to 50%
    ASSERT_NEAR(actual_commission, 2.5, 0.001);
    ASSERT_NEAR(portfolio.total_commissions, 5.0, 0.001); // 2.5 + 2.5
    ASSERT_NEAR(portfolio.position_qty(0), 0.0, 0.0001);  // All sold
}

TEST(accounting_matches_when_using_actual_commission) {
    // This test verifies the fix: when caller uses actual_commission,
    // accounting stays consistent
    TestPortfolio portfolio;
    MockPortfolioState state;

    double initial_cash = 10000.0;
    portfolio.init(initial_cash);
    state.set_cash(initial_cash);

    // Buy 0.5 BTC at $50000
    double buy_commission = 2.5;
    portfolio.buy(0, 50000.0, 0.5, buy_commission);
    state.set_cash(portfolio.cash);
    state.add_commission(buy_commission);

    // Try to sell 1.0 BTC at $51000 (but we only have 0.5)
    double requested_commission = 5.0;
    double avg_entry = portfolio.position_avg_price(0);
    double qty_before = portfolio.position_qty(0);

    // Get ACTUAL commission from sell
    double actual_commission = portfolio.sell(0, 51000.0, 1.0, requested_commission);

    // Update state with ACTUAL commission (THE FIX!)
    state.set_cash(portfolio.cash);
    state.add_commission(actual_commission); // Not requested_commission!

    // Calculate realized P&L for the actual quantity sold
    double actual_sold = qty_before;                           // 0.5
    double realized_pnl = (51000.0 - avg_entry) * actual_sold; // (51000 - 50000) * 0.5 = 500
    state.add_realized_pnl(realized_pnl);

    // P&L Reconciliation
    // Equity P&L = cash - initial_cash
    double equity_pnl = portfolio.cash - initial_cash;

    // Component P&L = realized_pnl - commission
    double component_pnl = state.component_pnl();

    // The difference should be ZERO (or very close)
    double difference = equity_pnl - component_pnl;

    std::cout << "\n    equity_pnl=" << equity_pnl << ", component_pnl=" << component_pnl << ", diff=" << difference
              << " ";

    ASSERT_NEAR(difference, 0.0, 0.01);
}

TEST(accounting_drifts_when_using_original_commission_BUG) {
    // This test demonstrates the BUG: using original commission causes drift
    TestPortfolio portfolio;
    MockPortfolioState state;

    double initial_cash = 10000.0;
    portfolio.init(initial_cash);
    state.set_cash(initial_cash);

    // Buy 0.5 BTC at $50000
    double buy_commission = 2.5;
    portfolio.buy(0, 50000.0, 0.5, buy_commission);
    state.set_cash(portfolio.cash);
    state.add_commission(buy_commission);

    // Try to sell 1.0 BTC at $51000 (but we only have 0.5)
    double requested_commission = 5.0; // Original commission
    double avg_entry = portfolio.position_avg_price(0);
    double qty_before = portfolio.position_qty(0);

    // Portfolio scales commission internally
    double actual_commission = portfolio.sell(0, 51000.0, 1.0, requested_commission);

    // BUG: State uses ORIGINAL commission, not actual!
    state.set_cash(portfolio.cash);
    state.add_commission(requested_commission); // BUG: Should be actual_commission

    // Calculate realized P&L
    double actual_sold = qty_before;
    double realized_pnl = (51000.0 - avg_entry) * actual_sold;
    state.add_realized_pnl(realized_pnl);

    // P&L Reconciliation
    double equity_pnl = portfolio.cash - initial_cash;
    double component_pnl = state.component_pnl();
    double difference = equity_pnl - component_pnl;

    // The difference should be NON-ZERO (demonstrates the bug)
    // actual_commission = 2.5, requested_commission = 5.0
    // Over-reported commission = 5.0 - 2.5 = 2.5
    // So component_pnl is LOWER than equity_pnl by 2.5
    double expected_drift = requested_commission - actual_commission;

    std::cout << "\n    BUG DEMO: equity_pnl=" << equity_pnl << ", component_pnl=" << component_pnl
              << ", drift=" << difference << " (expected=" << expected_drift << ") ";

    ASSERT_NEAR(difference, expected_drift, 0.01);
    ASSERT_TRUE(std::abs(difference) > 0.01); // Drift exists!
}

TEST(multiple_partial_sells_accumulate_drift) {
    // Multiple partial sells should accumulate the commission drift
    TestPortfolio portfolio;
    MockPortfolioState state;

    double initial_cash = 100000.0;
    portfolio.init(initial_cash);
    state.set_cash(initial_cash);

    // Simulate 100 trades where we try to sell more than we have
    double total_drift = 0.0;

    for (int i = 0; i < 100; i++) {
        // Buy 0.3 units
        double buy_commission = 3.0;
        portfolio.buy(0, 1000.0, 0.3, buy_commission);
        state.add_commission(buy_commission);

        // Try to sell 0.5 units (but only have 0.3)
        double requested_commission = 5.0;
        double actual_commission = portfolio.sell(0, 1010.0, 0.5, requested_commission);

        // Using ORIGINAL commission (the bug)
        state.add_commission(requested_commission);

        // Track drift
        total_drift += (requested_commission - actual_commission);
    }

    state.set_cash(portfolio.cash);

    std::cout << "\n    100 trades: total_drift=$" << total_drift << " ";

    // Each trade drifts by 5.0 - 3.0 = 2.0
    // 100 trades * $2.0 = $200.0 drift
    ASSERT_NEAR(total_drift, 200.0, 1.0);
}

TEST(sell_zero_position_returns_zero_commission) {
    // Selling with no position should return 0 commission
    TestPortfolio portfolio;
    portfolio.init(10000.0);

    // No buy, try to sell
    double actual_commission = portfolio.sell(0, 50000.0, 1.0, 5.0);

    ASSERT_NEAR(actual_commission, 0.0, 0.001);
    ASSERT_NEAR(portfolio.total_commissions, 0.0, 0.001);
    ASSERT_NEAR(portfolio.cash, 10000.0, 0.001); // Cash unchanged
}

TEST(sell_exact_position_no_scaling) {
    // Selling exactly what we have should not scale commission
    TestPortfolio portfolio;
    portfolio.init(10000.0);

    portfolio.buy(0, 100.0, 5.0, 0.5);                             // Buy 5 units
    double actual_commission = portfolio.sell(0, 110.0, 5.0, 0.5); // Sell exactly 5

    ASSERT_NEAR(actual_commission, 0.5, 0.001); // No scaling
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "\n=== Commission Scaling Bug Tests ===\n\n";

    RUN_TEST(sell_returns_actual_commission_when_full_qty);
    RUN_TEST(sell_returns_scaled_commission_when_partial_qty);
    RUN_TEST(accounting_matches_when_using_actual_commission);
    RUN_TEST(accounting_drifts_when_using_original_commission_BUG);
    RUN_TEST(multiple_partial_sells_accumulate_drift);
    RUN_TEST(sell_zero_position_returns_zero_commission);
    RUN_TEST(sell_exact_position_no_scaling);

    std::cout << "\n=== All tests PASSED! ===\n\n";
    return 0;
}
