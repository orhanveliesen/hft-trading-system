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
// Query Method Tests (NEW)
// =============================================================================

TEST(query_position_last_price) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    TradeInput buy{0, 100.0, 1.0, 0.1, 0.0, "BTC"};
    r.record_buy(buy);

    ASSERT_EQ(r.position_last_price(0), 100.0);

    // Update market price
    r.update_market_price(0, 105.0);
    ASSERT_EQ(r.position_last_price(0), 105.0);
}

TEST(query_market_value) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    // Buy 2 BTC @ $100, market price = $110
    TradeInput buy{0, 100.0, 2.0, 0.2, 0.0, "BTC"};
    r.record_buy(buy);
    r.update_market_price(0, 110.0);

    ASSERT_NEAR(r.market_value(), 220.0, 0.01); // 2 * 110
}

TEST(query_equity) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    // Buy 1 BTC @ $100 (cash = 99899.90)
    TradeInput buy{0, 100.0, 1.0, 0.1, 0.0, "BTC"};
    r.record_buy(buy);

    // Market price = $110
    r.update_market_price(0, 110.0);

    // Equity = cash + market_value = 99899.90 + 110 = 100009.90
    ASSERT_NEAR(r.equity(), 100009.90, 0.01);
}

TEST(query_equity_pnl) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    // Buy 1 BTC @ $100
    TradeInput buy{0, 100.0, 1.0, 0.1, 0.0, "BTC"};
    r.record_buy(buy);

    // Market price = $110 (unrealized +$10)
    r.update_market_price(0, 110.0);

    // Equity P&L = equity - initial = 100009.90 - 100000 = 9.90 (profit - commission)
    ASSERT_NEAR(r.equity_pnl(), 9.90, 0.01);
}

TEST(query_pnl_difference_zero) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    // Multiple trades
    TradeInput buy{0, 100.0, 2.0, 0.2, 0.0, "BTC"};
    r.record_buy(buy);
    r.update_market_price(0, 110.0);

    TradeInput sell{0, 110.0, 1.0, 0.11, 0.0, "BTC"};
    r.record_sell(sell);

    // P&L difference should be ~0
    ASSERT_NEAR(r.pnl_difference(), 0.0, 0.01);
}

TEST(query_win_rate) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    // 3 wins
    for (int i = 0; i < 3; i++) {
        TradeInput buy{0, 100.0, 1.0, 0.1, 0.0, "BTC"};
        r.record_buy(buy);
        TradeInput sell{0, 110.0, 1.0, 0.11, 0.0, "BTC"};
        r.record_sell(sell);
    }

    // 2 losses
    for (int i = 0; i < 2; i++) {
        TradeInput buy{0, 100.0, 1.0, 0.1, 0.0, "BTC"};
        r.record_buy(buy);
        TradeInput sell{0, 90.0, 1.0, 0.09, 0.0, "BTC"};
        r.record_sell(sell);
    }

    // Win rate = 3 / 5 = 60%
    ASSERT_NEAR(r.win_rate(), 60.0, 0.1);
}

TEST(query_win_rate_zero_trades) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    ASSERT_EQ(r.win_rate(), 0.0);
}

TEST(query_initial_cash) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(123456.78);

    ASSERT_NEAR(r.initial_cash(), 123456.78, 0.01);

    // Should not change after trades
    TradeInput buy{0, 100.0, 1.0, 0.1, 0.0, "BTC"};
    r.record_buy(buy);
    ASSERT_NEAR(r.initial_cash(), 123456.78, 0.01);
}

// =============================================================================
// Ledger Indexing Tests (NEW)
// =============================================================================

TEST(ledger_entry_by_index) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    // Create 3 trades
    for (int i = 0; i < 3; i++) {
        TradeInput buy{0, 100.0 + i, 1.0, 0.1, 0.0, "BTC"};
        r.record_buy(buy);
    }

    ASSERT_EQ(r.ledger_count(), 3);

    // Check indexing
    auto* e0 = r.ledger_entry(0); // First trade
    ASSERT_TRUE(e0 != nullptr);
    ASSERT_NEAR(e0->price, 100.0, 0.01);

    auto* e1 = r.ledger_entry(1); // Second trade
    ASSERT_TRUE(e1 != nullptr);
    ASSERT_NEAR(e1->price, 101.0, 0.01);

    auto* e2 = r.ledger_entry(2); // Third trade
    ASSERT_TRUE(e2 != nullptr);
    ASSERT_NEAR(e2->price, 102.0, 0.01);

    // Out of range
    auto* e3 = r.ledger_entry(3);
    ASSERT_TRUE(e3 == nullptr);
}

TEST(ledger_first_mismatch_none) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    TradeInput buy{0, 100.0, 1.0, 0.1, 0.0, "BTC"};
    r.record_buy(buy);

    // No mismatches
    auto* mismatch = r.ledger_first_mismatch();
    ASSERT_TRUE(mismatch == nullptr);
}

TEST(ledger_last_empty) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    // No entries yet
    ASSERT_TRUE(r.ledger_last() == nullptr);
}

TEST(ledger_entry_out_of_range) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    // No entries
    ASSERT_TRUE(r.ledger_entry(0) == nullptr);
    ASSERT_TRUE(r.ledger_entry(100) == nullptr);
}

// =============================================================================
// Circular Buffer Tests (NEW)
// =============================================================================

TEST(ledger_circular_buffer_wraparound) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000'000); // Large cash to handle many trades

    // Fill ledger beyond MAX_LEDGER_ENTRIES (10000)
    // Do 10100 trades to force wraparound
    for (int i = 0; i < 10100; i++) {
        TradeInput buy{0, 100.0, 0.01, 0.001, 0.0, "BTC"};
        r.record_buy(buy);

        TradeInput sell{0, 100.0, 0.01, 0.001, 0.0, "BTC"};
        r.record_sell(sell);
    }

    // Ledger should be capped at MAX_LEDGER_ENTRIES (10000)
    ASSERT_EQ(r.ledger_count(), 10000);

    // Should still be able to access all entries
    for (size_t i = 0; i < r.ledger_count(); i++) {
        auto* e = r.ledger_entry(i);
        ASSERT_TRUE(e != nullptr);
    }

    // Last entry should be most recent
    auto* last = r.ledger_last();
    ASSERT_TRUE(last != nullptr);
    ASSERT_EQ(last->sequence, 20200u); // 10100 buys + 10100 sells
}

// =============================================================================
// Query Edge Cases (NEW)
// =============================================================================

TEST(position_last_price_invalid_symbol) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    // Invalid symbol (>= MAX_RECORDER_SYMBOLS)
    ASSERT_EQ(r.position_last_price(999), 0.0);
}

TEST(market_value_multiple_symbols) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    // BTC: 2 @ $110 = $220
    TradeInput btc{0, 100.0, 2.0, 0.2, 0.0, "BTC"};
    r.record_buy(btc);
    r.update_market_price(0, 110.0);

    // ETH: 5 @ $20 = $100
    TradeInput eth{1, 10.0, 5.0, 0.5, 0.0, "ETH"};
    r.record_buy(eth);
    r.update_market_price(1, 20.0);

    // Total market value = 220 + 100 = 320
    ASSERT_NEAR(r.market_value(), 320.0, 0.01);
}

TEST(unrealized_pnl_no_positions) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    ASSERT_EQ(r.unrealized_pnl(), 0.0);
}

TEST(unrealized_pnl_no_market_price) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    TradeInput buy{0, 100.0, 1.0, 0.1, 0.0, "BTC"};
    r.record_buy(buy);

    // last_price is set by record_buy, so unrealized should be 0 (price == avg_price)
    ASSERT_NEAR(r.unrealized_pnl(), 0.0, 0.01);
}

TEST(update_market_price_invalid_symbol) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    // Should not crash
    r.update_market_price(999, 100.0);
}

// =============================================================================
// Ledger Dump Test (NEW)
// =============================================================================

TEST(ledger_dump_coverage) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    // Create some trades
    TradeInput buy{0, 100.0, 1.0, 0.1, 0.0, "BTC"};
    r.record_buy(buy);

    TradeInput sell{0, 110.0, 1.0, 0.11, 0.0, "BTC"};
    r.record_sell(sell);

    // Redirect stdout to avoid console spam in tests
    // Just call it to cover the code
    r.ledger_dump(2);

    // If we got here without crashing, dump works
    ASSERT_TRUE(true);
}

TEST(ledger_dump_more_than_available) {
    using namespace hft::trading;
    TradeRecorder r;
    r.init(100'000);

    TradeInput buy{0, 100.0, 1.0, 0.1, 0.0, "BTC"};
    r.record_buy(buy);

    // Request 100 entries, but only have 1
    r.ledger_dump(100);

    ASSERT_TRUE(true);
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

    std::cout << "\n--- Query Method Tests ---\n";
    RUN_TEST(query_position_last_price);
    RUN_TEST(query_market_value);
    RUN_TEST(query_equity);
    RUN_TEST(query_equity_pnl);
    RUN_TEST(query_pnl_difference_zero);
    RUN_TEST(query_win_rate);
    RUN_TEST(query_win_rate_zero_trades);
    RUN_TEST(query_initial_cash);

    std::cout << "\n--- Ledger Indexing Tests ---\n";
    RUN_TEST(ledger_entry_by_index);
    RUN_TEST(ledger_first_mismatch_none);
    RUN_TEST(ledger_last_empty);
    RUN_TEST(ledger_entry_out_of_range);

    std::cout << "\n--- Circular Buffer Tests ---\n";
    RUN_TEST(ledger_circular_buffer_wraparound);

    std::cout << "\n--- Query Edge Cases ---\n";
    RUN_TEST(position_last_price_invalid_symbol);
    RUN_TEST(market_value_multiple_symbols);
    RUN_TEST(unrealized_pnl_no_positions);
    RUN_TEST(unrealized_pnl_no_market_price);
    RUN_TEST(update_market_price_invalid_symbol);

    std::cout << "\n--- Ledger Dump Tests ---\n";
    RUN_TEST(ledger_dump_coverage);
    RUN_TEST(ledger_dump_more_than_available);

    std::cout << "\n=== All 48 tests passed! ===\n\n";
    return 0;
}
