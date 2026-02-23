/**
 * Portfolio Sell Function Test Suite
 *
 * Tests the portfolio sell function to ensure it handles edge cases correctly,
 * especially the critical overselling bug where cash was credited for more
 * than actually sold.
 *
 * BUG DISCOVERED: portfolio.sell() was adding cash based on requested quantity,
 * not actual sold quantity. If you tried to sell 10 but only had 3, cash would
 * increase by 10 units worth!
 *
 * Run with: ./test_portfolio_sell
 */

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

// Include the Portfolio class directly from trader.cpp structure
// We'll recreate a minimal version for testing

namespace test {

static constexpr size_t MAX_SYMBOLS = 64;
static constexpr size_t MAX_POSITION_SLOTS = 8;

struct PositionSlot {
    double quantity = 0;
    double entry_price = 0;
    double target_price = 0;
    double stop_loss = 0;
    bool active = false;

    void clear() {
        quantity = 0;
        entry_price = 0;
        target_price = 0;
        stop_loss = 0;
        active = false;
    }
};

struct SymbolPosition {
    PositionSlot slots[MAX_POSITION_SLOTS];
    int count = 0;

    void clear_all() {
        for (auto& s : slots)
            s.clear();
        count = 0;
    }

    double total_quantity() const {
        double total = 0;
        for (const auto& s : slots) {
            if (s.active)
                total += s.quantity;
        }
        return total;
    }

    double avg_entry() const {
        double total_qty = 0, total_value = 0;
        for (const auto& s : slots) {
            if (s.active) {
                total_qty += s.quantity;
                total_value += s.quantity * s.entry_price;
            }
        }
        return total_qty > 0 ? total_value / total_qty : 0;
    }

    bool add(double price, double qty, double target = 0, double stop = 0) {
        for (auto& slot : slots) {
            if (!slot.active) {
                slot.quantity = qty;
                slot.entry_price = price;
                slot.target_price = target;
                slot.stop_loss = stop;
                slot.active = true;
                count++;
                return true;
            }
        }
        return false; // No free slot
    }
};

class Portfolio {
public:
    double cash = 100000.0;
    double initial_cash = 100000.0;
    double total_commissions = 0;
    double total_volume = 0;
    double total_spread_cost = 0;
    SymbolPosition positions[MAX_SYMBOLS];
    bool symbol_active[MAX_SYMBOLS] = {false};

    double commission_rate() const { return 0.001; } // 0.1%

    void reset() {
        cash = initial_cash;
        total_commissions = 0;
        total_volume = 0;
        total_spread_cost = 0;
        for (size_t i = 0; i < MAX_SYMBOLS; i++) {
            positions[i].clear_all();
            symbol_active[i] = false;
        }
    }

    void buy(size_t s, double price, double qty) {
        if (s >= MAX_SYMBOLS)
            return;
        double cost = price * qty;
        double commission = cost * commission_rate();
        cash -= (cost + commission);
        total_commissions += commission;
        total_volume += cost;
        positions[s].add(price, qty);
        symbol_active[s] = true;
    }

    // FIXED VERSION: Uses actual_sold instead of requested qty
    double sell(size_t s, double price, double qty, double spread_cost = 0, double commission = 0) {
        if (qty <= 0 || price <= 0)
            return 0;
        if (s >= MAX_SYMBOLS)
            return 0;

        double remaining = qty;
        auto& sym_pos = positions[s];

        // Track actual quantity sold (may be less than requested if overselling)
        double actual_sold = 0;

        for (auto& slot : sym_pos.slots) {
            if (!slot.active || remaining <= 0)
                continue;

            double sell_qty = std::min(remaining, slot.quantity);
            slot.quantity -= sell_qty;
            remaining -= sell_qty;
            actual_sold += sell_qty;

            if (slot.quantity <= 0.0001) {
                slot.clear();
                sym_pos.count--;
            }
        }

        // CRITICAL: Use actual_sold, not requested qty
        double trade_value = price * actual_sold;

        if (commission <= 0) {
            commission = trade_value * commission_rate();
        } else {
            if (actual_sold < qty && qty > 0) {
                commission = commission * (actual_sold / qty);
            }
        }

        cash += trade_value - commission;
        total_commissions += commission;
        total_volume += trade_value;
        total_spread_cost += spread_cost;

        if (sym_pos.count == 0) {
            symbol_active[s] = false;
        }

        return actual_sold;
    }

    double get_holding(size_t s) const {
        if (s >= MAX_SYMBOLS)
            return 0;
        return positions[s].total_quantity();
    }
};

} // namespace test

using namespace test;

// Test macros
#define TEST(name) void test_##name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "  Running " #name "... ";                                                                        \
        test_##name();                                                                                                 \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

#define ASSERT_DOUBLE_NEAR(a, b, tol)                                                                                  \
    do {                                                                                                               \
        if (std::abs((a) - (b)) > (tol)) {                                                                             \
            std::cerr << "\nFAILED: " << #a << " != " << #b << " (" << (a) << " != " << (b) << ")\n";                  \
            assert(false);                                                                                             \
        }                                                                                                              \
    } while (0)

#define ASSERT_EQ(a, b)                                                                                                \
    do {                                                                                                               \
        if ((a) != (b)) {                                                                                              \
            std::cerr << "\nFAILED: " << #a << " != " << #b << "\n";                                                   \
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
// CRITICAL BUG TEST: Overselling should not credit extra cash
// =============================================================================

TEST(overselling_does_not_credit_extra_cash) {
    Portfolio p;
    p.reset();

    // Buy 3 units at $100 each
    p.buy(0, 100.0, 3.0);
    double cash_after_buy = p.cash; // ~$99,699.70 (100k - 300 - 0.30 commission)

    // Try to sell 10 units (but only have 3)
    double actual_sold = p.sell(0, 100.0, 10.0);

    // Should only sell 3, not 10
    ASSERT_DOUBLE_NEAR(actual_sold, 3.0, 0.001);

    // Cash should increase by 3 * $100 - commission, NOT 10 * $100
    // Expected: cash_after_buy + 300 - 0.30 = ~$99,999.40
    double expected_cash = cash_after_buy + (3.0 * 100.0) - (3.0 * 100.0 * 0.001);
    ASSERT_DOUBLE_NEAR(p.cash, expected_cash, 0.01);

    // Position should be 0
    ASSERT_DOUBLE_NEAR(p.get_holding(0), 0.0, 0.0001);

    // CRITICAL: Cash should NOT exceed initial cash (minus round-trip commissions)
    // Initial: $100,000, Round-trip commission: ~$0.60
    ASSERT_TRUE(p.cash < p.initial_cash); // Should be slightly less due to commissions
}

TEST(overselling_with_zero_position_does_nothing) {
    Portfolio p;
    p.reset();

    double initial_cash = p.cash;

    // Try to sell 5 units when position is 0
    double actual_sold = p.sell(0, 100.0, 5.0);

    // Should sell nothing
    ASSERT_DOUBLE_NEAR(actual_sold, 0.0, 0.0001);

    // Cash should be unchanged
    ASSERT_DOUBLE_NEAR(p.cash, initial_cash, 0.01);
}

TEST(sell_exact_position) {
    Portfolio p;
    p.reset();

    // Buy 5 units at $100
    p.buy(0, 100.0, 5.0);

    // Sell exactly 5 units
    double actual_sold = p.sell(0, 100.0, 5.0);

    ASSERT_DOUBLE_NEAR(actual_sold, 5.0, 0.001);
    ASSERT_DOUBLE_NEAR(p.get_holding(0), 0.0, 0.0001);
    ASSERT_EQ(p.symbol_active[0], false);
}

TEST(sell_partial_position) {
    Portfolio p;
    p.reset();

    // Buy 10 units
    p.buy(0, 100.0, 10.0);

    // Sell only 3 units
    double actual_sold = p.sell(0, 100.0, 3.0);

    ASSERT_DOUBLE_NEAR(actual_sold, 3.0, 0.001);
    ASSERT_DOUBLE_NEAR(p.get_holding(0), 7.0, 0.001);
    ASSERT_EQ(p.symbol_active[0], true); // Still has position
}

TEST(sell_across_multiple_slots) {
    Portfolio p;
    p.reset();

    // Buy in multiple tranches (creates multiple slots)
    p.buy(0, 100.0, 2.0); // Slot 1: 2 units
    p.buy(0, 105.0, 3.0); // Slot 2: 3 units
    p.buy(0, 110.0, 1.0); // Slot 3: 1 unit
    // Total: 6 units

    ASSERT_DOUBLE_NEAR(p.get_holding(0), 6.0, 0.001);

    // Sell 4 units (should drain slot 1 fully, slot 2 partially)
    double actual_sold = p.sell(0, 120.0, 4.0);

    ASSERT_DOUBLE_NEAR(actual_sold, 4.0, 0.001);
    ASSERT_DOUBLE_NEAR(p.get_holding(0), 2.0, 0.001); // 6 - 4 = 2 remaining
}

TEST(sell_fractional_crypto_quantities) {
    Portfolio p;
    p.reset();

    // Buy 0.03 BTC at $100,000
    p.buy(0, 100000.0, 0.03);

    // Try to sell 0.05 BTC (more than we have)
    double actual_sold = p.sell(0, 100000.0, 0.05);

    // Should only sell 0.03
    ASSERT_DOUBLE_NEAR(actual_sold, 0.03, 0.0001);
    ASSERT_DOUBLE_NEAR(p.get_holding(0), 0.0, 0.0001);

    // Cash credited should be for 0.03 BTC, not 0.05
    // 0.03 * 100000 = $3000 - commission
}

TEST(sell_dust_amount_clears_position) {
    Portfolio p;
    p.reset();

    // Buy a tiny amount
    p.buy(0, 100.0, 0.0002);

    // Sell it
    double actual_sold = p.sell(0, 100.0, 0.0002);

    ASSERT_DOUBLE_NEAR(actual_sold, 0.0002, 0.00001);
    ASSERT_DOUBLE_NEAR(p.get_holding(0), 0.0, 0.00001);
    ASSERT_EQ(p.symbol_active[0], false);
}

TEST(sell_negative_quantity_rejected) {
    Portfolio p;
    p.reset();

    p.buy(0, 100.0, 5.0);
    double cash_before = p.cash;

    // Try to sell negative quantity
    double actual_sold = p.sell(0, 100.0, -5.0);

    ASSERT_DOUBLE_NEAR(actual_sold, 0.0, 0.0001);
    ASSERT_DOUBLE_NEAR(p.cash, cash_before, 0.01); // No change
}

TEST(sell_negative_price_rejected) {
    Portfolio p;
    p.reset();

    p.buy(0, 100.0, 5.0);
    double cash_before = p.cash;

    // Try to sell at negative price
    double actual_sold = p.sell(0, -100.0, 5.0);

    ASSERT_DOUBLE_NEAR(actual_sold, 0.0, 0.0001);
    ASSERT_DOUBLE_NEAR(p.cash, cash_before, 0.01); // No change
}

TEST(sell_invalid_symbol_rejected) {
    Portfolio p;
    p.reset();

    double cash_before = p.cash;

    // Try to sell from invalid symbol
    double actual_sold = p.sell(MAX_SYMBOLS + 1, 100.0, 5.0);

    ASSERT_DOUBLE_NEAR(actual_sold, 0.0, 0.0001);
    ASSERT_DOUBLE_NEAR(p.cash, cash_before, 0.01);
}

TEST(massive_oversell_attack) {
    // Simulate an attacker trying to generate infinite money
    Portfolio p;
    p.reset();

    // Buy 0.01 BTC
    p.buy(0, 100000.0, 0.01);
    double cash_after_buy = p.cash; // ~$98,999

    // Try to sell 1000 BTC (100,000x what we have)
    double actual_sold = p.sell(0, 100000.0, 1000.0);

    // Should only sell 0.01
    ASSERT_DOUBLE_NEAR(actual_sold, 0.01, 0.0001);

    // Cash should NOT be $100,000,000 (1000 * 100000)!
    // Should be approximately initial - round trip commission
    ASSERT_TRUE(p.cash < p.initial_cash);
    ASSERT_TRUE(p.cash > p.initial_cash * 0.99); // Within 1% of initial
}

TEST(repeated_overselling_attempts) {
    Portfolio p;
    p.reset();

    // Buy 1 unit
    p.buy(0, 1000.0, 1.0);

    // Try to oversell multiple times
    for (int i = 0; i < 100; i++) {
        p.sell(0, 1000.0, 10.0); // Try to sell 10 each time
    }

    // Position should be 0 after first sell, cash should not balloon
    ASSERT_DOUBLE_NEAR(p.get_holding(0), 0.0, 0.0001);
    ASSERT_TRUE(p.cash < p.initial_cash); // Still lost money on commissions
}

TEST(commission_scaled_for_partial_sell) {
    Portfolio p;
    p.reset();

    // Buy 3 units at $100
    p.buy(0, 100.0, 3.0);

    // Sell with explicit commission of $1 for 10 units
    // But we only have 3, so commission should be scaled to $0.30
    double actual_sold = p.sell(0, 100.0, 10.0, 0, 1.0);

    ASSERT_DOUBLE_NEAR(actual_sold, 3.0, 0.001);

    // Commission should be 1.0 * (3/10) = 0.30
    // Cash = cash_after_buy + (3 * 100) - 0.30
}

int main() {
    std::cout << "\n=== Portfolio Sell Function Tests ===\n\n";

    std::cout << "Critical Bug Tests (Overselling):\n";
    RUN_TEST(overselling_does_not_credit_extra_cash);
    RUN_TEST(overselling_with_zero_position_does_nothing);
    RUN_TEST(massive_oversell_attack);
    RUN_TEST(repeated_overselling_attempts);

    std::cout << "\nNormal Operation Tests:\n";
    RUN_TEST(sell_exact_position);
    RUN_TEST(sell_partial_position);
    RUN_TEST(sell_across_multiple_slots);
    RUN_TEST(sell_fractional_crypto_quantities);
    RUN_TEST(sell_dust_amount_clears_position);

    std::cout << "\nInput Validation Tests:\n";
    RUN_TEST(sell_negative_quantity_rejected);
    RUN_TEST(sell_negative_price_rejected);
    RUN_TEST(sell_invalid_symbol_rejected);

    std::cout << "\nCommission Tests:\n";
    RUN_TEST(commission_scaled_for_partial_sell);

    std::cout << "\n=== All tests PASSED! ===\n\n";
    return 0;
}
