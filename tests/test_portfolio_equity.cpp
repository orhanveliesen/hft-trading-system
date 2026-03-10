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

// ============================================================================
// Additional Coverage Tests
// ============================================================================

TEST(win_rate_calculation) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);
    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 10000.0);

    // 3 winning trades
    state->add_realized_pnl(100.0);
    state->add_realized_pnl(50.0);
    state->add_realized_pnl(25.0);

    // 2 losing trades
    state->add_realized_pnl(-30.0);
    state->add_realized_pnl(-10.0);

    // Win rate = 3/5 = 60%
    ASSERT_NEAR(state->win_rate(), 60.0, 0.1);
    ASSERT_EQ(state->winning_trades.load(), 3u);
    ASSERT_EQ(state->losing_trades.load(), 2u);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(win_rate_zero_trades) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);
    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 10000.0);

    // No trades - win rate should be 0
    ASSERT_NEAR(state->win_rate(), 0.0, 0.01);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(cost_tracking) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);
    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 10000.0);

    state->add_slippage(5.0);
    state->add_commission(10.0);
    state->add_spread_cost(3.0);

    ASSERT_NEAR(state->total_slippage(), 5.0, 0.01);
    ASSERT_NEAR(state->total_commissions(), 10.0, 0.01);
    ASSERT_NEAR(state->total_spread_cost(), 3.0, 0.01);
    ASSERT_NEAR(state->total_costs(), 18.0, 0.01);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(gross_pnl_calculation) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);
    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 10000.0);

    state->add_realized_pnl(100.0);
    state->add_slippage(5.0);
    state->add_commission(10.0);

    // gross_pnl = realized_pnl + total_costs = 100 + 15 = 115
    ASSERT_NEAR(state->gross_pnl(), 115.0, 0.01);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(cost_per_trade_metrics) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);
    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 10000.0);

    // 5 fills with total costs of 25
    state->record_fill();
    state->record_fill();
    state->record_fill();
    state->record_fill();
    state->record_fill();
    state->add_slippage(10.0);
    state->add_commission(15.0);

    // cost_per_trade = 25 / 5 = 5.0
    ASSERT_NEAR(state->cost_per_trade(), 5.0, 0.01);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(avg_trade_value) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);
    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 10000.0);

    state->record_fill();
    state->record_fill();
    state->add_volume(1000.0);
    state->add_volume(500.0);

    // avg_trade_value = 1500 / 2 = 750
    ASSERT_NEAR(state->avg_trade_value(), 750.0, 0.01);
    ASSERT_NEAR(state->total_volume(), 1500.0, 0.01);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(cost_pct_per_trade) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);
    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 10000.0);

    state->record_fill();
    state->add_volume(1000.0);
    state->add_commission(5.0);

    // avg_trade_value = 1000 / 1 = 1000
    // cost_per_trade = 5 / 1 = 5
    // cost_pct = 5 / 1000 * 100 = 0.5%
    ASSERT_NEAR(state->cost_pct_per_trade(), 0.5, 0.01);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(record_events) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);
    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 10000.0);

    state->record_fill();
    state->record_fill();
    state->record_target();
    state->record_stop();
    state->record_event();
    state->record_event();

    ASSERT_EQ(state->total_fills.load(), 2u);
    ASSERT_EQ(state->total_targets.load(), 1u);
    ASSERT_EQ(state->total_stops.load(), 1u);
    ASSERT_EQ(state->total_events.load(), 2u);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(fast_path_update_last_price) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);
    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 10000.0);

    // Initialize slot 0
    state->init_slot(0, "BTCUSDT");

    // Update price using fast path
    state->update_last_price_fast(0, 50000.0);

    ASSERT_NEAR(state->positions[0].last_price(), 50000.0, 0.01);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(fast_path_update_position) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);
    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 10000.0);

    state->init_slot(0, "ETHUSDT");

    // Update position using fast path
    state->update_position_fast(0, 5.0, 2000.0, 2500.0, 100.0);

    ASSERT_NEAR(state->positions[0].quantity(), 5.0, 0.01);
    ASSERT_NEAR(state->positions[0].avg_price(), 2000.0, 0.01);
    ASSERT_NEAR(state->positions[0].last_price(), 2500.0, 0.01);
    ASSERT_NEAR(state->positions[0].realized_pnl(), 100.0, 0.01);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(relaxed_memory_order_updates) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);
    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 10000.0);

    state->init_slot(1, "SOLUSDT");

    // Update using relaxed memory order
    int64_t price_x8 = static_cast<int64_t>(150.0 * FIXED_POINT_SCALE);
    state->update_last_price_relaxed(1, price_x8);

    ASSERT_NEAR(state->positions[1].last_price(), 150.0, 0.01);

    // Update position with relaxed order
    int64_t qty_x8 = static_cast<int64_t>(100.0 * FIXED_POINT_SCALE);
    int64_t avg_x8 = static_cast<int64_t>(100.0 * FIXED_POINT_SCALE);
    int64_t last_x8 = static_cast<int64_t>(120.0 * FIXED_POINT_SCALE);
    state->update_position_relaxed(1, qty_x8, avg_x8, last_x8);

    ASSERT_NEAR(state->positions[1].quantity(), 100.0, 0.01);
    ASSERT_NEAR(state->positions[1].avg_price(), 100.0, 0.01);
    ASSERT_NEAR(state->positions[1].last_price(), 120.0, 0.01);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(get_sequence_acquire) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);
    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 10000.0);

    uint32_t seq1 = state->get_sequence_acquire();
    state->set_cash(9000.0);
    uint32_t seq2 = state->get_sequence_acquire();

    assert(seq2 > seq1);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(update_regime) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);
    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 10000.0);

    state->update_regime("BTCUSDT", 1);

    auto* pos = state->get_or_create_position("BTCUSDT");
    ASSERT_EQ(pos->regime.load(), 1u);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(record_buy_sell) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);
    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 10000.0);

    state->record_buy("ETHUSDT");
    state->record_buy("ETHUSDT");
    state->record_sell("ETHUSDT");

    auto* pos = state->get_or_create_position("ETHUSDT");
    ASSERT_EQ(pos->buy_count.load(), 2u);
    ASSERT_EQ(pos->sell_count.load(), 1u);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(open_read_only) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);

    // Create with write access
    auto* writer = SharedPortfolioState::create(TEST_SHM_NAME, 15000.0);
    writer->set_cash(12000.0);

    // Open with read-only access
    auto* reader = SharedPortfolioState::open(TEST_SHM_NAME);
    assert(reader != nullptr);
    assert(reader->is_valid());
    ASSERT_NEAR(reader->cash(), 12000.0, 0.01);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(open_rw) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);

    // Create
    auto* writer = SharedPortfolioState::create(TEST_SHM_NAME, 20000.0);

    // Open with read-write
    auto* rw = SharedPortfolioState::open_rw(TEST_SHM_NAME);
    assert(rw != nullptr);
    assert(rw->is_valid());

    // Modify through rw
    rw->set_cash(18000.0);
    ASSERT_NEAR(rw->cash(), 18000.0, 0.01);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(open_invalid) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);

    // Create and corrupt magic
    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 10000.0);
    state->magic = 0xDEADBEEF;

    // Try to open - should return nullptr
    auto* reader = SharedPortfolioState::open(TEST_SHM_NAME);
    assert(reader == nullptr);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(position_snapshot_methods) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);
    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 10000.0);

    auto* pos = state->get_or_create_position("BTCUSDT");
    auto& snap = pos->snapshot;

    // Set snapshot values
    snap.price_open_x8.store(static_cast<int64_t>(50000.0 * FIXED_POINT_SCALE));
    snap.price_high_x8.store(static_cast<int64_t>(55000.0 * FIXED_POINT_SCALE));
    snap.price_low_x8.store(static_cast<int64_t>(49000.0 * FIXED_POINT_SCALE));
    snap.ema_20_x8.store(static_cast<int64_t>(51000.0 * FIXED_POINT_SCALE));
    snap.atr_14_x8.store(static_cast<int64_t>(1000.0 * FIXED_POINT_SCALE));
    snap.volume_sum_x8.store(static_cast<int64_t>(100.5 * FIXED_POINT_SCALE));
    snap.volatility_x100.store(250); // 2.50%
    snap.trend_direction.store(1);
    snap.tick_count.store(42);

    // Test conversion methods
    ASSERT_NEAR(snap.price_open(), 50000.0, 0.01);
    ASSERT_NEAR(snap.price_high(), 55000.0, 0.01);
    ASSERT_NEAR(snap.price_low(), 49000.0, 0.01);
    ASSERT_NEAR(snap.ema_20(), 51000.0, 0.01);
    ASSERT_NEAR(snap.atr_14(), 1000.0, 0.01);
    ASSERT_NEAR(snap.volume_sum(), 100.5, 0.01);
    ASSERT_NEAR(snap.volatility(), 2.50, 0.01);
    ASSERT_NEAR(snap.volatility_pct(), 2.50, 0.01);
    ASSERT_EQ(snap.trend_direction.load(), 1);
    ASSERT_EQ(snap.tick_count.load(), 42u);

    // Test price_range_pct
    // (55000 - 49000) / 49000 * 100 = 12.24%
    ASSERT_NEAR(snap.price_range_pct(), 12.24, 0.01);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(position_snapshot_clear) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);
    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 10000.0);

    auto* pos = state->get_or_create_position("ETHUSDT");
    auto& snap = pos->snapshot;

    // Set some values
    snap.price_open_x8.store(100);
    snap.tick_count.store(99);

    // Clear
    snap.clear();

    ASSERT_EQ(snap.price_open_x8.load(), 0);
    ASSERT_EQ(snap.tick_count.load(), 0u);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(position_snapshot_zero_price_range) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);
    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 10000.0);

    auto* pos = state->get_or_create_position("SOLUSDT");
    auto& snap = pos->snapshot;

    // Low = 0 should return 0% range
    snap.price_high_x8.store(static_cast<int64_t>(100.0 * FIXED_POINT_SCALE));
    snap.price_low_x8.store(0);

    ASSERT_NEAR(snap.price_range_pct(), 0.0, 0.01);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(fast_path_out_of_bounds) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);
    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 10000.0);

    // Try to update beyond MAX_PORTFOLIO_SYMBOLS - should not crash
    state->update_last_price_fast(MAX_PORTFOLIO_SYMBOLS + 1, 100.0);
    state->update_position_fast(MAX_PORTFOLIO_SYMBOLS + 1, 1.0, 100.0, 100.0);
    state->update_last_price_relaxed(MAX_PORTFOLIO_SYMBOLS + 1, 100);
    state->update_position_relaxed(MAX_PORTFOLIO_SYMBOLS + 1, 100, 100, 100);

    assert(true); // Should reach here without crash

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(init_slot_out_of_bounds) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);
    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 10000.0);

    // Try to init beyond MAX_PORTFOLIO_SYMBOLS - should not crash
    state->init_slot(MAX_PORTFOLIO_SYMBOLS + 1, "INVALID");

    assert(true); // Should reach here without crash

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(total_pnl_calculation) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);
    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 10000.0);

    // Add position with market value
    state->update_position("BTCUSDT", 1.0, 50000.0, 60000.0);

    // total_pnl = total_equity - initial_cash
    // total_equity = cash + market_value = 10000 + 60000 = 70000
    // total_pnl = 70000 - 10000 = 60000
    ASSERT_NEAR(state->total_pnl(), 60000.0, 0.01);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(update_last_price_by_name) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);
    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 10000.0);

    // Update last price by symbol name (slow path)
    state->update_last_price("BTCUSDT", 55000.0);

    auto* pos = state->get_or_create_position("BTCUSDT");
    ASSERT_NEAR(pos->last_price(), 55000.0, 0.01);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(set_initial_cash) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);
    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 10000.0);

    state->set_initial_cash(15000.0);
    ASSERT_NEAR(state->initial_cash(), 15000.0, 0.01);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

TEST(add_realized_pnl_zero) {
    SharedPortfolioState::destroy(TEST_SHM_NAME);
    auto* state = SharedPortfolioState::create(TEST_SHM_NAME, 10000.0);

    // Adding zero P&L should not increment win/loss counters
    state->add_realized_pnl(0.0);

    ASSERT_EQ(state->winning_trades.load(), 0u);
    ASSERT_EQ(state->losing_trades.load(), 0u);
    ASSERT_NEAR(state->total_realized_pnl(), 0.0, 0.01);

    SharedPortfolioState::destroy(TEST_SHM_NAME);
}

int main() {
    std::cout << "Running portfolio equity tests (BUG-001):\n";

    RUN_TEST(equity_equals_cash_plus_market_value);
    RUN_TEST(equity_with_multiple_positions);
    RUN_TEST(equity_with_zero_positions);

    std::cout << "\nRunning comprehensive coverage tests:\n";

    RUN_TEST(win_rate_calculation);
    RUN_TEST(win_rate_zero_trades);
    RUN_TEST(cost_tracking);
    RUN_TEST(gross_pnl_calculation);
    RUN_TEST(cost_per_trade_metrics);
    RUN_TEST(avg_trade_value);
    RUN_TEST(cost_pct_per_trade);
    RUN_TEST(record_events);
    RUN_TEST(fast_path_update_last_price);
    RUN_TEST(fast_path_update_position);
    RUN_TEST(relaxed_memory_order_updates);
    RUN_TEST(get_sequence_acquire);
    RUN_TEST(update_regime);
    RUN_TEST(record_buy_sell);
    RUN_TEST(open_read_only);
    RUN_TEST(open_rw);
    RUN_TEST(open_invalid);
    RUN_TEST(position_snapshot_methods);
    RUN_TEST(position_snapshot_clear);
    RUN_TEST(position_snapshot_zero_price_range);
    RUN_TEST(fast_path_out_of_bounds);
    RUN_TEST(init_slot_out_of_bounds);
    RUN_TEST(total_pnl_calculation);
    RUN_TEST(update_last_price_by_name);
    RUN_TEST(set_initial_cash);
    RUN_TEST(add_realized_pnl_zero);

    std::cout << "\nAll 29 portfolio tests passed!\n";
    return 0;
}
