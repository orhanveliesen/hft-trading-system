/**
 * Test TradeRecorder - Single Source of Truth for P&L Tracking
 *
 * TDD RED PHASE: These tests define the expected behavior.
 * They will fail until TradeRecorder is implemented.
 *
 * Key invariant to test:
 *   equity_pnl == realized_pnl + unrealized_pnl - total_commission
 *   DIFFERENCE == 0.00 (always!)
 */

#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

// Include the TradeRecorder header (will be created)
#include "../include/ipc/shared_ledger.hpp"
#include "../include/trading/trade_recorder.hpp"

// Test framework macros
#define TEST(name) void name()

#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "  " << #name << "... ";                                                                          \
        try {                                                                                                          \
            name();                                                                                                    \
            std::cout << "PASSED\n";                                                                                   \
        } catch (...) {                                                                                                \
            std::cout << "FAILED (exception)\n";                                                                       \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

#define ASSERT_EQ(a, b)                                                                                                \
    do {                                                                                                               \
        if ((a) != (b)) {                                                                                              \
            std::cerr << "\nFAIL: " << #a << " (" << (a) << ") != " << #b << " (" << (b) << ")\n";                     \
            assert(false);                                                                                             \
        }                                                                                                              \
    } while (0)

#define ASSERT_NEAR(a, b, tol)                                                                                         \
    do {                                                                                                               \
        if (std::abs((a) - (b)) > (tol)) {                                                                             \
            std::cerr << "\nFAIL: " << #a << " (" << (a) << ") != " << #b << " (" << (b) << ") within " << (tol)       \
                      << "\n";                                                                                         \
            assert(false);                                                                                             \
        }                                                                                                              \
    } while (0)

#define ASSERT_TRUE(expr)                                                                                              \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            std::cerr << "\nFAIL: " << #expr << " is false\n";                                                         \
            assert(false);                                                                                             \
        }                                                                                                              \
    } while (0)

#define ASSERT_GT(a, b)                                                                                                \
    do {                                                                                                               \
        if (!((a) > (b))) {                                                                                            \
            std::cerr << "\nFAIL: " << #a << " (" << (a) << ") not > " << #b << " (" << (b) << ")\n";                  \
            assert(false);                                                                                             \
        }                                                                                                              \
    } while (0)

#define ASSERT_LT(a, b)                                                                                                \
    do {                                                                                                               \
        if (!((a) < (b))) {                                                                                            \
            std::cerr << "\nFAIL: " << #a << " (" << (a) << ") not < " << #b << " (" << (b) << ")\n";                  \
            assert(false);                                                                                             \
        }                                                                                                              \
    } while (0)

// =============================================================================
// TEST 1: Buy reduces cash and creates position
// =============================================================================
TEST(trade_recorder_buy_reduces_cash) {
    using namespace hft;
    using namespace hft::trading;

    // Setup: Create recorder with $10,000 initial cash
    TradeRecorder recorder;
    recorder.init(10000.0);

    // Action: Buy 1 unit @ $100, commission = $0.10
    TradeInput input{};
    input.symbol = 0;
    input.price = 100.0;
    input.quantity = 1.0;
    input.commission = 0.10;
    std::strcpy(input.ticker, "BTCUSDT");

    recorder.record_buy(input);

    // Assert: Cash reduced by price + commission
    // cash = 10000 - 100 - 0.10 = 9899.90
    ASSERT_NEAR(recorder.cash(), 9899.90, 0.01);

    // Assert: Position created
    ASSERT_NEAR(recorder.position_quantity(0), 1.0, 0.0001);
    ASSERT_NEAR(recorder.position_avg_price(0), 100.0, 0.01);

    // Assert: Commission tracked
    ASSERT_NEAR(recorder.total_commission(), 0.10, 0.001);

    // Assert: Fill count incremented
    ASSERT_EQ(recorder.total_fills(), 1u);

    // Assert: No realized P&L yet (only bought, didn't sell)
    ASSERT_NEAR(recorder.realized_pnl(), 0.0, 0.001);
}

// =============================================================================
// TEST 2: Sell increases cash and tracks realized P&L (profit)
// =============================================================================
TEST(trade_recorder_sell_tracks_realized_pnl_profit) {
    using namespace hft;
    using namespace hft::trading;

    TradeRecorder recorder;
    recorder.init(10000.0);

    // Buy 1 unit @ $100
    TradeInput buy_input{};
    buy_input.symbol = 0;
    buy_input.price = 100.0;
    buy_input.quantity = 1.0;
    buy_input.commission = 0.10;
    std::strcpy(buy_input.ticker, "BTCUSDT");
    recorder.record_buy(buy_input);

    double cash_after_buy = recorder.cash(); // ~9899.90

    // Sell 1 unit @ $110 (profit!)
    TradeInput sell_input{};
    sell_input.symbol = 0;
    sell_input.price = 110.0;
    sell_input.quantity = 1.0;
    sell_input.commission = 0.11;
    std::strcpy(sell_input.ticker, "BTCUSDT");
    recorder.record_sell(sell_input);

    // Assert: Cash increased by (sell_price - commission)
    // cash = 9899.90 + 110 - 0.11 = 10009.79
    ASSERT_NEAR(recorder.cash(), 10009.79, 0.01);

    // Assert: Realized P&L = (110 - 100) * 1 = +10
    ASSERT_NEAR(recorder.realized_pnl(), 10.0, 0.01);

    // Assert: Position cleared
    ASSERT_NEAR(recorder.position_quantity(0), 0.0, 0.0001);

    // Assert: Total commission = buy + sell
    ASSERT_NEAR(recorder.total_commission(), 0.21, 0.001);
}

// =============================================================================
// TEST 3: Sell at loss tracks negative realized P&L
// =============================================================================
TEST(trade_recorder_sell_tracks_realized_pnl_loss) {
    using namespace hft;
    using namespace hft::trading;

    TradeRecorder recorder;
    recorder.init(10000.0);

    // Buy 1 unit @ $100
    TradeInput buy_input{};
    buy_input.symbol = 0;
    buy_input.price = 100.0;
    buy_input.quantity = 1.0;
    buy_input.commission = 0.10;
    std::strcpy(buy_input.ticker, "BTCUSDT");
    recorder.record_buy(buy_input);

    // Sell 1 unit @ $95 (LOSS!)
    TradeInput sell_input{};
    sell_input.symbol = 0;
    sell_input.price = 95.0;
    sell_input.quantity = 1.0;
    sell_input.commission = 0.095;
    std::strcpy(sell_input.ticker, "BTCUSDT");
    recorder.record_sell(sell_input);

    // Assert: Realized P&L = (95 - 100) * 1 = -5
    ASSERT_NEAR(recorder.realized_pnl(), -5.0, 0.01);

    // Assert: Losing trade counted
    ASSERT_EQ(recorder.losing_trades(), 1u);
    ASSERT_EQ(recorder.winning_trades(), 0u);
}

// =============================================================================
// TEST 4: P&L Reconciliation - THE CRITICAL TEST
// Equity P&L must ALWAYS equal Realized + Unrealized - Commission
// =============================================================================
TEST(trade_recorder_pnl_reconciliation) {
    using namespace hft;
    using namespace hft::trading;

    TradeRecorder recorder;
    recorder.init(10000.0);

    // Simulate multiple trades
    TradeInput input{};
    std::strcpy(input.ticker, "BTCUSDT");

    // Trade 1: Buy 2 @ $100
    input.symbol = 0;
    input.price = 100.0;
    input.quantity = 2.0;
    input.commission = 0.20;
    recorder.record_buy(input);

    // Trade 2: Buy 1 @ $105 (average up)
    input.price = 105.0;
    input.quantity = 1.0;
    input.commission = 0.105;
    recorder.record_buy(input);

    // Update market price to $110 (unrealized profit)
    recorder.update_market_price(0, 110.0);

    // Trade 3: Sell 1 @ $110 (realize partial profit)
    input.price = 110.0;
    input.quantity = 1.0;
    input.commission = 0.11;
    recorder.record_sell(input);

    // Now verify reconciliation
    double equity_pnl = recorder.equity() - 10000.0; // Equity change from initial
    double component_pnl = recorder.realized_pnl() + recorder.unrealized_pnl() - recorder.total_commission();
    double difference = equity_pnl - component_pnl;

    // THE INVARIANT: difference must be zero!
    ASSERT_NEAR(difference, 0.0, 0.01);
}

// =============================================================================
// TEST 5: Partial sell
// =============================================================================
TEST(trade_recorder_partial_sell) {
    using namespace hft;
    using namespace hft::trading;

    TradeRecorder recorder;
    recorder.init(10000.0);

    // Buy 10 units @ $100
    TradeInput buy_input{};
    buy_input.symbol = 0;
    buy_input.price = 100.0;
    buy_input.quantity = 10.0;
    buy_input.commission = 1.0;
    std::strcpy(buy_input.ticker, "BTCUSDT");
    recorder.record_buy(buy_input);

    // Sell 3 units @ $110 (partial)
    TradeInput sell_input{};
    sell_input.symbol = 0;
    sell_input.price = 110.0;
    sell_input.quantity = 3.0;
    sell_input.commission = 0.33;
    std::strcpy(sell_input.ticker, "BTCUSDT");
    recorder.record_sell(sell_input);

    // Assert: Realized P&L = (110 - 100) * 3 = +30
    ASSERT_NEAR(recorder.realized_pnl(), 30.0, 0.01);

    // Assert: Remaining position = 7 units @ avg $100
    ASSERT_NEAR(recorder.position_quantity(0), 7.0, 0.0001);
    ASSERT_NEAR(recorder.position_avg_price(0), 100.0, 0.01);
}

// =============================================================================
// TEST 6: Target exit records winning trade
// =============================================================================
TEST(trade_recorder_target_exit) {
    using namespace hft;
    using namespace hft::trading;

    TradeRecorder recorder;
    recorder.init(10000.0);

    // Buy 1 @ $100
    TradeInput buy_input{};
    buy_input.symbol = 0;
    buy_input.price = 100.0;
    buy_input.quantity = 1.0;
    buy_input.commission = 0.10;
    std::strcpy(buy_input.ticker, "BTCUSDT");
    recorder.record_buy(buy_input);

    // Exit at target ($115 = 15% profit)
    TradeInput exit_input{};
    exit_input.symbol = 0;
    exit_input.price = 115.0;
    exit_input.quantity = 1.0;
    exit_input.commission = 0.115;
    std::strcpy(exit_input.ticker, "BTCUSDT");
    recorder.record_exit(ExitReason::TARGET, exit_input);

    // Assert: Winning trade counted
    ASSERT_EQ(recorder.winning_trades(), 1u);
    ASSERT_EQ(recorder.target_count(), 1u);

    // Assert: Realized profit
    ASSERT_GT(recorder.realized_pnl(), 0.0);
}

// =============================================================================
// TEST 7: Stop loss exit records losing trade
// =============================================================================
TEST(trade_recorder_stop_exit) {
    using namespace hft;
    using namespace hft::trading;

    TradeRecorder recorder;
    recorder.init(10000.0);

    // Buy 1 @ $100
    TradeInput buy_input{};
    buy_input.symbol = 0;
    buy_input.price = 100.0;
    buy_input.quantity = 1.0;
    buy_input.commission = 0.10;
    std::strcpy(buy_input.ticker, "BTCUSDT");
    recorder.record_buy(buy_input);

    // Exit at stop loss ($95 = 5% loss)
    TradeInput exit_input{};
    exit_input.symbol = 0;
    exit_input.price = 95.0;
    exit_input.quantity = 1.0;
    exit_input.commission = 0.095;
    std::strcpy(exit_input.ticker, "BTCUSDT");
    recorder.record_exit(ExitReason::STOP, exit_input);

    // Assert: Losing trade counted
    ASSERT_EQ(recorder.losing_trades(), 1u);
    ASSERT_EQ(recorder.stop_count(), 1u);

    // Assert: Realized loss
    ASSERT_LT(recorder.realized_pnl(), 0.0);
}

// =============================================================================
// TEST 8: Multiple symbols independently tracked
// =============================================================================
TEST(trade_recorder_multiple_symbols) {
    using namespace hft;
    using namespace hft::trading;

    TradeRecorder recorder;
    recorder.init(10000.0);

    // Buy BTC @ $100
    TradeInput btc_input{};
    btc_input.symbol = 0;
    btc_input.price = 100.0;
    btc_input.quantity = 1.0;
    btc_input.commission = 0.10;
    std::strcpy(btc_input.ticker, "BTCUSDT");
    recorder.record_buy(btc_input);

    // Buy ETH @ $50
    TradeInput eth_input{};
    eth_input.symbol = 1;
    eth_input.price = 50.0;
    eth_input.quantity = 2.0;
    eth_input.commission = 0.10;
    std::strcpy(eth_input.ticker, "ETHUSDT");
    recorder.record_buy(eth_input);

    // Assert: Both positions exist
    ASSERT_NEAR(recorder.position_quantity(0), 1.0, 0.0001); // BTC
    ASSERT_NEAR(recorder.position_quantity(1), 2.0, 0.0001); // ETH

    // Sell ETH @ $55 (profit)
    TradeInput eth_sell{};
    eth_sell.symbol = 1;
    eth_sell.price = 55.0;
    eth_sell.quantity = 2.0;
    eth_sell.commission = 0.11;
    std::strcpy(eth_sell.ticker, "ETHUSDT");
    recorder.record_sell(eth_sell);

    // Assert: ETH closed, BTC still open
    ASSERT_NEAR(recorder.position_quantity(0), 1.0, 0.0001); // BTC unchanged
    ASSERT_NEAR(recorder.position_quantity(1), 0.0, 0.0001); // ETH closed

    // Assert: Realized P&L only from ETH
    ASSERT_NEAR(recorder.realized_pnl(), 10.0, 0.01); // (55-50)*2 = 10
}

// =============================================================================
// TEST 9: Stress test - 100 round-trips, no drift
// =============================================================================
TEST(trade_recorder_no_drift_100_trades) {
    using namespace hft;
    using namespace hft::trading;

    TradeRecorder recorder;
    recorder.init(10000.0);

    // Do 100 round-trip trades
    for (int i = 0; i < 100; i++) {
        double buy_price = 100.0 + (i % 10); // Vary price slightly
        double sell_price = buy_price + 1.0; // Always $1 profit

        TradeInput buy_input{};
        buy_input.symbol = 0;
        buy_input.price = buy_price;
        buy_input.quantity = 1.0;
        buy_input.commission = buy_price * 0.001;
        std::strcpy(buy_input.ticker, "BTCUSDT");
        recorder.record_buy(buy_input);

        TradeInput sell_input{};
        sell_input.symbol = 0;
        sell_input.price = sell_price;
        sell_input.quantity = 1.0;
        sell_input.commission = sell_price * 0.001;
        std::strcpy(sell_input.ticker, "BTCUSDT");
        recorder.record_sell(sell_input);
    }

    // Verify reconciliation after many trades
    double equity_pnl = recorder.equity() - 10000.0;
    double component_pnl = recorder.realized_pnl() + recorder.unrealized_pnl() - recorder.total_commission();
    double difference = equity_pnl - component_pnl;

    // THE INVARIANT: difference must be zero even after 100 trades!
    ASSERT_NEAR(difference, 0.0, 0.01);

    // Also verify basic sanity
    ASSERT_EQ(recorder.total_fills(), 200u);                 // 100 buys + 100 sells
    ASSERT_NEAR(recorder.position_quantity(0), 0.0, 0.0001); // All closed
}

// =============================================================================
// TEST 10: Volume tracking
// =============================================================================
TEST(trade_recorder_volume_tracking) {
    using namespace hft;
    using namespace hft::trading;

    TradeRecorder recorder;
    recorder.init(10000.0);

    // Buy 2 @ $100 = $200 volume
    TradeInput input{};
    input.symbol = 0;
    input.price = 100.0;
    input.quantity = 2.0;
    input.commission = 0.20;
    std::strcpy(input.ticker, "BTCUSDT");
    recorder.record_buy(input);

    // Sell 2 @ $110 = $220 volume
    input.price = 110.0;
    input.commission = 0.22;
    recorder.record_sell(input);

    // Total volume = 200 + 220 = 420
    ASSERT_NEAR(recorder.total_volume(), 420.0, 0.01);
}

// =============================================================================
// LEDGER TESTS (NEW)
// =============================================================================

// TEST 11: Ledger records BUY entry
TEST(ledger_records_buy_entry) {
    using namespace hft;
    using namespace hft::trading;

    TradeRecorder r;
    r.init(10000.0);

    TradeInput input{};
    input.symbol = 1;
    input.price = 100.0;
    input.quantity = 1.0;
    input.commission = 0.10;
    std::strcpy(input.ticker, "BTCUSDT");
    r.record_buy(input);

    ASSERT_EQ(r.ledger_count(), 1u);
    auto* e = r.ledger_last();
    ASSERT_TRUE(e != nullptr);
    ASSERT_EQ(e->is_buy, 1u);
    ASSERT_NEAR(e->cash_before, 10000.0, 0.01);
    ASSERT_NEAR(e->cash_after, 9899.90, 0.01);
    ASSERT_NEAR(e->realized_pnl, 0.0, 0.01); // BUY has no P&L
    ASSERT_EQ(e->balance_ok, 1u);
}

// TEST 12: Ledger records SELL with gain
TEST(ledger_records_sell_gain) {
    using namespace hft;
    using namespace hft::trading;

    TradeRecorder r;
    r.init(10000.0);

    // Buy @ 100
    TradeInput buy{};
    buy.symbol = 1;
    buy.price = 100.0;
    buy.quantity = 1.0;
    buy.commission = 0.10;
    std::strcpy(buy.ticker, "BTCUSDT");
    r.record_buy(buy);

    // Sell @ 115 (gain)
    TradeInput sell{};
    sell.symbol = 1;
    sell.price = 115.0;
    sell.quantity = 1.0;
    sell.commission = 0.10;
    std::strcpy(sell.ticker, "BTCUSDT");
    r.record_sell(sell);

    auto* e = r.ledger_last();
    ASSERT_TRUE(e != nullptr);
    ASSERT_EQ(e->is_buy, 0u);
    ASSERT_NEAR(e->realized_pnl, 15.0, 0.01); // Gain: positive
    ASSERT_GT(e->realized_pnl, 0.0);
}

// TEST 13: Ledger records SELL with loss
TEST(ledger_records_sell_loss) {
    using namespace hft;
    using namespace hft::trading;

    TradeRecorder r;
    r.init(10000.0);

    // Buy @ 100
    TradeInput buy{};
    buy.symbol = 1;
    buy.price = 100.0;
    buy.quantity = 1.0;
    buy.commission = 0.10;
    std::strcpy(buy.ticker, "BTCUSDT");
    r.record_buy(buy);

    // Sell @ 90 (loss)
    TradeInput sell{};
    sell.symbol = 1;
    sell.price = 90.0;
    sell.quantity = 1.0;
    sell.commission = 0.10;
    std::strcpy(sell.ticker, "BTCUSDT");
    r.record_sell(sell);

    auto* e = r.ledger_last();
    ASSERT_TRUE(e != nullptr);
    ASSERT_NEAR(e->realized_pnl, -10.0, 0.01); // Loss: negative
    ASSERT_LT(e->realized_pnl, 0.0);
}

// TEST 14: Gains/Losses tracking
TEST(ledger_gains_losses_tracking) {
    using namespace hft;
    using namespace hft::trading;

    TradeRecorder r;
    r.init(10000.0);

    TradeInput input{};
    std::strcpy(input.ticker, "BTCUSDT");

    // Trade 1: +15 gain
    input.symbol = 1;
    input.price = 100.0;
    input.quantity = 1.0;
    input.commission = 0.10;
    r.record_buy(input);
    input.price = 115.0;
    r.record_sell(input);

    // Trade 2: -10 loss
    input.price = 120.0;
    r.record_buy(input);
    input.price = 110.0;
    r.record_sell(input);

    // Running totals
    ASSERT_NEAR(r.total_gains(), 15.0, 0.01);
    ASSERT_NEAR(r.total_losses(), 10.0, 0.01); // Absolute value

    // Verify: gains - losses = realized_pnl
    ASSERT_NEAR(r.total_gains() - r.total_losses(), r.realized_pnl(), 0.01);
}

// TEST 15: Cash matches ledger last entry
TEST(ledger_cash_matches_last_entry) {
    using namespace hft;
    using namespace hft::trading;

    TradeRecorder r;
    r.init(10000.0);

    TradeInput input{};
    input.symbol = 1;
    input.price = 100.0;
    input.quantity = 1.0;
    input.commission = 0.10;
    std::strcpy(input.ticker, "BTCUSDT");
    r.record_buy(input);

    // Cash should match ledger's cash_after
    ASSERT_NEAR(r.cash(), r.ledger_last()->cash_after, 0.001);
}

// TEST 16: Ledger balance check (no mismatches)
TEST(ledger_no_mismatches) {
    using namespace hft;
    using namespace hft::trading;

    TradeRecorder r;
    r.init(10000.0);

    TradeInput input{};
    input.symbol = 1;
    std::strcpy(input.ticker, "BTCUSDT");

    // Multiple trades
    for (int i = 0; i < 10; i++) {
        input.price = 100.0 + i;
        input.quantity = 0.5;
        input.commission = 0.05;
        r.record_buy(input);

        input.price = 105.0 + i;
        r.record_sell(input);
    }

    // All entries should have balance_ok = 1
    ASSERT_EQ(r.ledger_check_balance(), 0u);
}

// TEST 17: Calculation breakdown allows debugging
TEST(ledger_calculation_breakdown) {
    using namespace hft;
    using namespace hft::trading;

    TradeRecorder r;
    r.init(10000.0);

    // Buy @ 100
    TradeInput buy{};
    buy.symbol = 1;
    buy.price = 100.0;
    buy.quantity = 2.0;
    buy.commission = 0.20;
    std::strcpy(buy.ticker, "BTCUSDT");
    r.record_buy(buy);

    auto* e1 = r.ledger_last();
    ASSERT_NEAR(e1->trade_value, 200.0, 0.01);            // price × qty
    ASSERT_NEAR(e1->expected_cash_change, -200.20, 0.01); // -(trade_value + commission)
    ASSERT_EQ(e1->pnl_ok, 1u);                            // BUY has no P&L, always OK

    // Sell @ 105 (profit)
    TradeInput sell{};
    sell.symbol = 1;
    sell.price = 105.0;
    sell.quantity = 2.0;
    sell.commission = 0.21;
    std::strcpy(sell.ticker, "BTCUSDT");
    r.record_sell(sell);

    auto* e2 = r.ledger_last();
    ASSERT_NEAR(e2->trade_value, 210.0, 0.01);           // price × qty
    ASSERT_NEAR(e2->expected_cash_change, 209.79, 0.01); // +(trade_value - commission)
    ASSERT_NEAR(e2->pnl_per_unit, 5.0, 0.01);            // sell_price - avg_entry
    ASSERT_NEAR(e2->expected_pnl, 10.0, 0.01);           // pnl_per_unit × qty
    ASSERT_NEAR(e2->realized_pnl, 10.0, 0.01);           // Should match
    ASSERT_EQ(e2->pnl_ok, 1u);                           // P&L matches expected
}

// TEST 18: Verify consistency (running totals == ledger sum)
TEST(ledger_verify_consistency) {
    using namespace hft;
    using namespace hft::trading;

    TradeRecorder r;
    r.init(10000.0);

    TradeInput input{};
    input.symbol = 1;
    std::strcpy(input.ticker, "BTCUSDT");

    // Do many trades
    for (int i = 0; i < 50; i++) {
        input.price = 100.0;
        input.quantity = 1.0;
        input.commission = 0.10;
        r.record_buy(input);

        input.price = 100.0 + (i % 3 == 0 ? 5.0 : -3.0); // Mix gains and losses
        r.record_sell(input);
    }

    // Verify running totals match ledger sum
    ASSERT_TRUE(r.verify_consistency());
}

// =============================================================================
// TEST 19: SharedLedger IPC integration
// =============================================================================
TEST(shared_ledger_ipc_integration) {
    using namespace hft;
    using namespace hft::trading;
    using namespace hft::ipc;

    const char* shm_name = "/test_recorder_ledger";
    SharedLedger::destroy(shm_name);

    // Create SharedLedger
    auto* shared = SharedLedger::create(shm_name);
    ASSERT_TRUE(shared != nullptr);
    ASSERT_EQ(shared->count(), 0u);

    // Create recorder and connect to SharedLedger
    TradeRecorder r;
    r.init(10000.0);
    r.connect_shared_ledger(shared);
    ASSERT_TRUE(r.has_shared_ledger());

    // Record some trades
    TradeInput input{};
    input.symbol = 1;
    std::strcpy(input.ticker, "ETHUSDT");

    // BUY
    input.price = 2500.0;
    input.quantity = 1.0;
    input.commission = 0.25;
    r.record_buy(input);

    // SELL (profit)
    input.price = 2600.0;
    input.commission = 0.26;
    r.record_sell(input);

    // Verify SharedLedger has entries
    ASSERT_EQ(shared->count(), 2u);

    // Check BUY entry
    auto* e0 = shared->entry(0);
    ASSERT_TRUE(e0 != nullptr);
    ASSERT_NEAR(e0->price(), 2500.0, 0.01);
    ASSERT_NEAR(e0->quantity(), 1.0, 0.001);
    ASSERT_EQ(e0->is_buy.load(), 1u);

    // Check SELL entry
    auto* e1 = shared->entry(1);
    ASSERT_TRUE(e1 != nullptr);
    ASSERT_NEAR(e1->price(), 2600.0, 0.01);
    ASSERT_NEAR(e1->realized_pnl(), 100.0, 0.01); // +100 profit
    ASSERT_EQ(e1->is_buy.load(), 0u);

    // Verify SharedLedger readable from "another process" (simulated)
    auto* reader = SharedLedger::open(shm_name);
    ASSERT_TRUE(reader != nullptr);
    ASSERT_EQ(reader->count(), 2u);
    ASSERT_NEAR(reader->entry(1)->realized_pnl(), 100.0, 0.01);

    // Cleanup
    SharedLedger::unmap(reader);
    SharedLedger::unmap(shared);
    SharedLedger::destroy(shm_name);
}

// =============================================================================
// Callback Tests
// =============================================================================

TEST(sync_callback_invoked_on_trades) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    bool callback_called = false;
    double captured_cash = 0;
    double captured_pnl = 0;
    uint32_t captured_fills = 0;

    r.set_sync_callback([](double cash, double rpnl, double upnl, double comm, double vol, uint32_t fills,
                           uint32_t wins, uint32_t losses, uint32_t targets, uint32_t stops) {
        // Can't capture from inside C function pointer, so this won't work with lambda captures
        // This test verifies the callback signature compiles
        (void)cash;
        (void)rpnl;
    });

    // Since we can't use lambda captures with C function pointers, use static variables
    static bool s_callback_called = false;
    static double s_captured_cash = 0;
    static uint32_t s_captured_fills = 0;

    s_callback_called = false;

    r.set_sync_callback([](double cash, double rpnl, double upnl, double comm, double vol, uint32_t fills,
                           uint32_t wins, uint32_t losses, uint32_t targets, uint32_t stops) {
        s_callback_called = true;
        s_captured_cash = cash;
        s_captured_fills = fills;
    });

    TradeInput buy{0, 100.0, 1.0, 0.1, 0.0, "BTC"};
    r.record_buy(buy);

    ASSERT_TRUE(s_callback_called);
    ASSERT_EQ(s_captured_fills, 1);
    ASSERT_GT(s_captured_cash, 0);
}

TEST(trade_callback_invoked_on_buy) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    static bool s_trade_called = false;
    static bool s_was_buy = false;
    static double s_price = 0;

    s_trade_called = false;

    r.set_trade_callback([](const TradeEventInfo& info) {
        s_trade_called = true;
        s_was_buy = info.is_buy;
        s_price = info.price;
    });

    TradeInput buy{0, 100.0, 1.0, 0.1, 0.0, "BTC"};
    r.record_buy(buy);

    ASSERT_TRUE(s_trade_called);
    ASSERT_TRUE(s_was_buy);
    ASSERT_EQ(s_price, 100.0);
}

TEST(trade_callback_invoked_on_sell) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    TradeInput buy{0, 100.0, 1.0, 0.1, 0.0, "BTC"};
    r.record_buy(buy);

    static bool s_trade_called = false;
    static bool s_was_buy = true;
    static double s_realized = 0;

    s_trade_called = false;

    r.set_trade_callback([](const TradeEventInfo& info) {
        s_trade_called = true;
        s_was_buy = info.is_buy;
        s_realized = info.realized_pnl;
    });

    TradeInput sell{0, 110.0, 1.0, 0.11, 0.0, "BTC"};
    r.record_sell(sell);

    ASSERT_TRUE(s_trade_called);
    ASSERT_TRUE(!s_was_buy);
    ASSERT_GT(s_realized, 9.0); // ~10 profit - commission
}

// =============================================================================
// Exit Reason Tests
// =============================================================================

TEST(exit_reason_pullback) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    TradeInput buy{0, 100.0, 1.0, 0.1, 0.0, "BTC"};
    r.record_buy(buy);

    TradeInput exit{0, 105.0, 1.0, 0.105, 0.0, "BTC"};
    r.record_exit(ExitReason::PULLBACK, exit);

    ASSERT_EQ(r.position_quantity(0), 0);
    ASSERT_GT(r.realized_pnl(), 4.0); // ~5 profit - commissions
}

TEST(exit_reason_emergency) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    TradeInput buy{0, 100.0, 1.0, 0.1, 0.0, "BTC"};
    r.record_buy(buy);

    TradeInput exit{0, 95.0, 1.0, 0.095, 0.0, "BTC"};
    r.record_exit(ExitReason::EMERGENCY, exit);

    ASSERT_EQ(r.position_quantity(0), 0);
    ASSERT_TRUE(r.realized_pnl() < 0); // Loss
}

TEST(exit_reason_signal) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    TradeInput buy{0, 100.0, 1.0, 0.1, 0.0, "BTC"};
    r.record_buy(buy);

    TradeInput exit{0, 102.0, 1.0, 0.102, 0.0, "BTC"};
    r.record_exit(ExitReason::SIGNAL, exit);

    ASSERT_EQ(r.position_quantity(0), 0);
    ASSERT_GT(r.realized_pnl(), 1.5); // ~2 profit - commissions
}

// =============================================================================
// Edge Case Tests
// =============================================================================

TEST(reject_zero_quantity_buy) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    double cash_before = r.cash();

    TradeInput buy{0, 100.0, 0.0, 0.1, 0.0, "BTC"}; // Zero quantity
    r.record_buy(buy);

    // Should be ignored, cash unchanged
    ASSERT_EQ(r.cash(), cash_before);
    ASSERT_EQ(r.position_quantity(0), 0);
}

TEST(reject_zero_price_buy) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    double cash_before = r.cash();

    TradeInput buy{0, 0.0, 1.0, 0.1, 0.0, "BTC"}; // Zero price
    r.record_buy(buy);

    // Should be ignored, cash unchanged
    ASSERT_EQ(r.cash(), cash_before);
    ASSERT_EQ(r.position_quantity(0), 0);
}

TEST(reject_invalid_symbol_index) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    double cash_before = r.cash();

    TradeInput buy{999, 100.0, 1.0, 0.1, 0.0, "BTC"}; // Symbol >= MAX_RECORDER_SYMBOLS (64)
    r.record_buy(buy);

    // Should be ignored, cash unchanged
    ASSERT_EQ(r.cash(), cash_before);
}

TEST(reject_zero_quantity_sell) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    TradeInput buy{0, 100.0, 1.0, 0.1, 0.0, "BTC"};
    r.record_buy(buy);

    double pnl_before = r.realized_pnl();

    TradeInput sell{0, 110.0, 0.0, 0.11, 0.0, "BTC"}; // Zero quantity
    r.record_sell(sell);

    // Should be ignored, position and P&L unchanged
    ASSERT_EQ(r.position_quantity(0), 1.0);
    ASSERT_EQ(r.realized_pnl(), pnl_before);
}

TEST(reject_sell_with_no_position) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    double cash_before = r.cash();

    TradeInput sell{0, 110.0, 1.0, 0.11, 0.0, "BTC"}; // No position to sell
    r.record_sell(sell);

    // Should be ignored, cash unchanged
    ASSERT_EQ(r.cash(), cash_before);
    ASSERT_EQ(r.realized_pnl(), 0);
}

// =============================================================================
// MAIN
// =============================================================================
int main() {
    std::cout << "\n=== TradeRecorder Tests ===\n\n";

    int failed = 0;

    RUN_TEST(trade_recorder_buy_reduces_cash);
    RUN_TEST(trade_recorder_sell_tracks_realized_pnl_profit);
    RUN_TEST(trade_recorder_sell_tracks_realized_pnl_loss);
    RUN_TEST(trade_recorder_pnl_reconciliation);
    RUN_TEST(trade_recorder_partial_sell);
    RUN_TEST(trade_recorder_target_exit);
    RUN_TEST(trade_recorder_stop_exit);
    RUN_TEST(trade_recorder_multiple_symbols);
    RUN_TEST(trade_recorder_no_drift_100_trades);
    RUN_TEST(trade_recorder_volume_tracking);

    std::cout << "\n--- Ledger Tests ---\n";
    RUN_TEST(ledger_records_buy_entry);
    RUN_TEST(ledger_records_sell_gain);
    RUN_TEST(ledger_records_sell_loss);
    RUN_TEST(ledger_gains_losses_tracking);
    RUN_TEST(ledger_cash_matches_last_entry);
    RUN_TEST(ledger_no_mismatches);
    RUN_TEST(ledger_calculation_breakdown);
    RUN_TEST(ledger_verify_consistency);

    std::cout << "\n--- SharedLedger IPC Tests ---\n";
    RUN_TEST(shared_ledger_ipc_integration);

    std::cout << "\n--- Callback Tests ---\n";
    RUN_TEST(sync_callback_invoked_on_trades);
    RUN_TEST(trade_callback_invoked_on_buy);
    RUN_TEST(trade_callback_invoked_on_sell);

    std::cout << "\n--- Exit Reason Tests ---\n";
    RUN_TEST(exit_reason_pullback);
    RUN_TEST(exit_reason_emergency);
    RUN_TEST(exit_reason_signal);

    std::cout << "\n--- Edge Case Tests ---\n";
    RUN_TEST(reject_zero_quantity_buy);
    RUN_TEST(reject_zero_price_buy);
    RUN_TEST(reject_invalid_symbol_index);
    RUN_TEST(reject_zero_quantity_sell);
    RUN_TEST(reject_sell_with_no_position);

    std::cout << "\n=== All tests passed! ===\n\n";
    return 0;
}
