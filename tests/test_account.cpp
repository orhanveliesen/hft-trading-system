#include <cassert>
#include <iostream>
#include "../include/account/account.hpp"

using namespace hft;
using namespace hft::account;

#define TEST(name) void name()
#define RUN_TEST(name) do { \
    std::cout << "Running " << #name << "... "; \
    name(); \
    std::cout << "PASSED\n"; \
} while(0)

#define ASSERT_EQ(a, b) assert((a) == (b))
#define ASSERT_TRUE(x) assert(x)
#define ASSERT_FALSE(x) assert(!(x))

// === AccountInfo Tests ===

TEST(test_account_info_equity) {
    AccountInfo info;
    info.cash_balance = 100000000;  // $1,000,000
    info.unrealized_pnl = 500000;   // $5,000 profit

    ASSERT_EQ(info.equity(), 100500000);  // $1,005,000
}

TEST(test_account_info_net_liq) {
    AccountInfo info;
    info.cash_balance = 50000000;   // $500,000
    info.unrealized_pnl = -1000000; // -$10,000 loss

    ASSERT_EQ(info.net_liq(), 49000000);  // $490,000
}

// === AccountManager Tests ===

TEST(test_manager_initial_state) {
    AccountManager manager;

    ASSERT_EQ(manager.cash_balance(), 0);
    ASSERT_EQ(manager.buying_power(), 0);
    ASSERT_EQ(manager.equity(), 0);
}

TEST(test_manager_update_account) {
    AccountManager manager;

    AccountInfo info;
    info.cash_balance = 100000000;  // $1,000,000
    info.buying_power = 400000000;  // $4,000,000 (4x margin)
    info.margin_available = 300000000;
    info.sequence = 1;

    manager.update(info);

    ASSERT_EQ(manager.cash_balance(), 100000000);
    ASSERT_EQ(manager.buying_power(), 400000000);
    ASSERT_EQ(manager.margin_available(), 300000000);
}

TEST(test_manager_incremental_updates) {
    AccountManager manager;

    manager.update_cash(50000000);
    ASSERT_EQ(manager.cash_balance(), 50000000);

    manager.update_buying_power(200000000);
    ASSERT_EQ(manager.buying_power(), 200000000);

    manager.update_pnl(1000000, 500000);
    ASSERT_EQ(manager.info().unrealized_pnl, 1000000);
    ASSERT_EQ(manager.info().realized_pnl, 500000);
}

// === Pre-Trade Checks ===

TEST(test_order_cost_calculation) {
    MarginRequirement margin;
    margin.initial_margin = 0.25;  // 4x leverage
    margin.min_equity = 2500000;   // $25,000

    AccountManager manager(margin);

    AccountInfo info;
    info.cash_balance = 100000000;  // $1,000,000
    info.buying_power = 400000000;  // $4,000,000
    manager.update(info);

    // Buy 100 shares at $100 = $10,000 notional
    OrderCost cost = manager.calculate_order_cost(Side::Buy, 100, 10000);

    ASSERT_EQ(cost.notional, 1000000);  // $10,000 (100 * $100)
    ASSERT_EQ(cost.margin_required, 250000);  // $2,500 (25%)
    ASSERT_TRUE(cost.can_afford);
}

TEST(test_order_cost_insufficient_funds) {
    MarginRequirement margin;
    margin.initial_margin = 0.25;
    margin.min_equity = 2500000;

    AccountManager manager(margin);

    AccountInfo info;
    info.cash_balance = 100000;  // $1,000 (very low)
    info.buying_power = 100000;  // No leverage
    manager.update(info);

    // Try to buy $10,000 worth
    OrderCost cost = manager.calculate_order_cost(Side::Buy, 100, 10000);

    ASSERT_FALSE(cost.can_afford);
    ASSERT_FALSE(cost.reject_reason.empty());
}

TEST(test_order_cost_below_min_equity) {
    MarginRequirement margin;
    margin.initial_margin = 0.25;
    margin.min_equity = 2500000;  // $25,000 minimum

    AccountManager manager(margin);

    AccountInfo info;
    info.cash_balance = 1000000;  // $10,000 (below PDT limit)
    info.buying_power = 4000000;  // $40,000
    manager.update(info);

    // Even though we have buying power, equity is too low
    OrderCost cost = manager.calculate_order_cost(Side::Buy, 10, 10000);

    ASSERT_FALSE(cost.can_afford);
    ASSERT_TRUE(cost.reject_reason.find("minimum equity") != std::string::npos);
}

// === Buying Power Reservation ===

TEST(test_reserve_buying_power) {
    AccountManager manager;

    AccountInfo info;
    info.buying_power = 100000000;  // $1,000,000
    manager.update(info);

    ASSERT_EQ(manager.buying_power(), 100000000);

    // Reserve $250,000
    bool reserved = manager.reserve_buying_power(25000000);
    ASSERT_TRUE(reserved);
    ASSERT_EQ(manager.buying_power(), 75000000);  // $750,000 left
    ASSERT_EQ(manager.reserved_buying_power(), 25000000);
}

TEST(test_reserve_buying_power_insufficient) {
    AccountManager manager;

    AccountInfo info;
    info.buying_power = 10000000;  // $100,000
    manager.update(info);

    // Try to reserve more than available
    bool reserved = manager.reserve_buying_power(20000000);
    ASSERT_FALSE(reserved);
    ASSERT_EQ(manager.reserved_buying_power(), 0);  // Nothing reserved
}

TEST(test_release_buying_power) {
    AccountManager manager;

    AccountInfo info;
    info.buying_power = 100000000;
    manager.update(info);

    manager.reserve_buying_power(25000000);
    ASSERT_EQ(manager.buying_power(), 75000000);

    // Release half
    manager.release_buying_power(12500000);
    ASSERT_EQ(manager.buying_power(), 87500000);
    ASSERT_EQ(manager.reserved_buying_power(), 12500000);

    // Release the rest
    manager.release_buying_power(12500000);
    ASSERT_EQ(manager.buying_power(), 100000000);
    ASSERT_EQ(manager.reserved_buying_power(), 0);
}

TEST(test_release_more_than_reserved) {
    AccountManager manager;

    AccountInfo info;
    info.buying_power = 100000000;
    manager.update(info);

    manager.reserve_buying_power(10000000);

    // Release more than reserved - should clamp to 0
    manager.release_buying_power(20000000);
    ASSERT_EQ(manager.reserved_buying_power(), 0);
}

// === Quick Afford Check ===

TEST(test_can_afford_quick_check) {
    MarginRequirement margin;
    margin.initial_margin = 0.25;

    AccountManager manager(margin);

    AccountInfo info;
    info.buying_power = 100000000;  // $1,000,000
    manager.update(info);

    // Can afford $100,000 order (needs $25,000 margin)
    ASSERT_TRUE(manager.can_afford(1000, 10000));

    // Cannot afford $10,000,000 order (needs $2,500,000 margin)
    ASSERT_FALSE(manager.can_afford(100000, 10000));
}

// === Update Callback ===

TEST(test_update_callback) {
    AccountManager manager;

    bool callback_called = false;
    int64_t received_cash = 0;

    manager.set_update_callback([&](const AccountInfo& info) {
        callback_called = true;
        received_cash = info.cash_balance;
    });

    AccountInfo info;
    info.cash_balance = 50000000;
    manager.update(info);

    ASSERT_TRUE(callback_called);
    ASSERT_EQ(received_cash, 50000000);
}

int main() {
    std::cout << "=== Account Tests ===\n\n";

    // AccountInfo
    RUN_TEST(test_account_info_equity);
    RUN_TEST(test_account_info_net_liq);

    // AccountManager basics
    RUN_TEST(test_manager_initial_state);
    RUN_TEST(test_manager_update_account);
    RUN_TEST(test_manager_incremental_updates);

    // Pre-trade checks
    RUN_TEST(test_order_cost_calculation);
    RUN_TEST(test_order_cost_insufficient_funds);
    RUN_TEST(test_order_cost_below_min_equity);

    // Buying power reservation
    RUN_TEST(test_reserve_buying_power);
    RUN_TEST(test_reserve_buying_power_insufficient);
    RUN_TEST(test_release_buying_power);
    RUN_TEST(test_release_more_than_reserved);

    // Quick checks
    RUN_TEST(test_can_afford_quick_check);

    // Callbacks
    RUN_TEST(test_update_callback);

    std::cout << "\nAll tests PASSED!\n";
    return 0;
}
