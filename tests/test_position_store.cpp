/**
 * Position Store Test Suite
 *
 * Tests position persistence and cash validation during restore.
 *
 * BUG DISCOVERED: Position file could contain inflated cash from overselling bug.
 * When restoring positions, cash must be validated/recalculated.
 *
 * Run with: ./test_position_store
 */

#include <iostream>
#include <fstream>
#include <cmath>
#include <cassert>
#include <cstdio>
#include "../include/ipc/shared_portfolio_state.hpp"
#include "../include/strategy/position_store.hpp"

using namespace hft::ipc;
using namespace hft::strategy;

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    std::cout << "  Running " #name "... "; \
    test_##name(); \
    std::cout << "PASSED\n"; \
} while(0)

#define ASSERT_DOUBLE_NEAR(a, b, tol) do { \
    if (std::abs((a) - (b)) > (tol)) { \
        std::cerr << "\nFAILED: " << #a << " != " << #b \
                  << " (" << (a) << " != " << (b) << ")\n"; \
        assert(false); \
    } \
} while(0)

static const char* TEST_FILE = "/tmp/test_positions.json";

// Create a corrupted position file (simulating overselling bug)
void write_corrupted_file(double initial_capital, double corrupted_cash,
                          double position_qty, double position_price,
                          double realized_pnl = 0) {
    std::ofstream out(TEST_FILE);
    out << "{\n"
        << "  \"version\": 1,\n"
        << "  \"timestamp_ns\": 12345,\n"
        << "  \"initial_capital\": " << initial_capital << ",\n"
        << "  \"cash\": " << corrupted_cash << ",\n"
        << "  \"total_realized_pnl\": " << realized_pnl << ",\n"
        << "  \"winning_trades\": 0,\n"
        << "  \"losing_trades\": 0,\n"
        << "  \"total_fills\": 10,\n"
        << "  \"total_targets\": 0,\n"
        << "  \"total_stops\": 0,\n"
        << "  \"total_commissions\": 0,\n"
        << "  \"total_spread_cost\": 0,\n"
        << "  \"total_slippage\": 0,\n"
        << "  \"total_volume\": 10000,\n"
        << "  \"positions\": [\n"
        << "    {\n"
        << "      \"symbol\": \"BTCUSDT\",\n"
        << "      \"symbol_id\": 1,\n"
        << "      \"quantity\": " << position_qty << ",\n"
        << "      \"avg_price\": " << position_price << ",\n"
        << "      \"last_price\": " << position_price << ",\n"
        << "      \"realized_pnl\": 0,\n"
        << "      \"buy_count\": 10,\n"
        << "      \"sell_count\": 0\n"
        << "    }\n"
        << "  ]\n"
        << "}\n";
    out.close();
}

void cleanup_test_file() {
    std::remove(TEST_FILE);
}

// =============================================================================
// Cash Validation Tests
// =============================================================================

TEST(corrupted_cash_is_corrected) {
    cleanup_test_file();

    // Setup: $100k initial, bought 0.5 BTC at $50k = $25k position
    // Valid cash would be ~$75k
    // But file has inflated $150k (from overselling bug)
    double initial_capital = 100000.0;
    double position_qty = 0.5;
    double position_price = 50000.0;
    double corrupted_cash = 150000.0;  // Obviously wrong - more than initial!
    double expected_cash = initial_capital - (position_qty * position_price);  // $75k

    write_corrupted_file(initial_capital, corrupted_cash, position_qty, position_price);

    // Create portfolio state (would normally be shared memory)
    // For testing, we use a stack-allocated version
    SharedPortfolioState portfolio;
    portfolio.init(initial_capital);

    PositionStore store(TEST_FILE);
    bool restored = store.restore(portfolio);

    assert(restored);

    // Cash should be corrected to ~$75k, not the corrupted $150k
    double restored_cash = portfolio.cash();
    ASSERT_DOUBLE_NEAR(restored_cash, expected_cash, 1000.0);  // Within $1k tolerance

    // Definitely should NOT be the corrupted value
    assert(restored_cash < corrupted_cash);
    assert(restored_cash <= initial_capital);  // Cash can't exceed initial if we bought something

    cleanup_test_file();
}

TEST(valid_cash_is_preserved) {
    cleanup_test_file();

    // Setup: Valid data - $100k initial, bought 0.5 BTC at $50k
    // Valid cash = $75k (with small margin for commission rounding)
    double initial_capital = 100000.0;
    double position_qty = 0.5;
    double position_price = 50000.0;
    double valid_cash = 74990.0;  // Slightly less due to commissions
    double expected_cash = initial_capital - (position_qty * position_price);

    write_corrupted_file(initial_capital, valid_cash, position_qty, position_price);

    SharedPortfolioState portfolio;
    portfolio.init(initial_capital);

    PositionStore store(TEST_FILE);
    store.restore(portfolio);

    // Cash should be close to the file value (within 1% tolerance)
    double restored_cash = portfolio.cash();
    ASSERT_DOUBLE_NEAR(restored_cash, expected_cash, 1000.0);

    cleanup_test_file();
}

TEST(massive_oversell_attack_corrected) {
    cleanup_test_file();

    // Simulate massive overselling: $100k initial, tiny position
    // but attacker tried to sell 1000x what they had
    double initial_capital = 100000.0;
    double position_qty = 0.0001;  // Tiny BTC position
    double position_price = 100000.0;  // = $10 worth
    double attack_cash = 10000000.0;  // $10 million (1000x oversell!)
    double expected_cash = initial_capital - (position_qty * position_price);  // ~$99,990

    write_corrupted_file(initial_capital, attack_cash, position_qty, position_price);

    SharedPortfolioState portfolio;
    portfolio.init(initial_capital);

    PositionStore store(TEST_FILE);
    store.restore(portfolio);

    double restored_cash = portfolio.cash();

    // Should NOT be $10 million
    assert(restored_cash < attack_cash);
    // Should be close to expected
    ASSERT_DOUBLE_NEAR(restored_cash, expected_cash, 100.0);

    cleanup_test_file();
}

TEST(negative_realized_pnl_reduces_cash) {
    cleanup_test_file();

    // Setup: $100k initial, no positions, but lost money trading
    double initial_capital = 100000.0;
    double position_qty = 0.0;
    double position_price = 0.0;
    double realized_pnl = -5000.0;  // Lost $5k
    double valid_cash = 95000.0;  // $100k - $5k
    double expected_cash = initial_capital + realized_pnl;

    std::ofstream out(TEST_FILE);
    out << "{\n"
        << "  \"version\": 1,\n"
        << "  \"timestamp_ns\": 12345,\n"
        << "  \"initial_capital\": " << initial_capital << ",\n"
        << "  \"cash\": " << valid_cash << ",\n"
        << "  \"total_realized_pnl\": " << realized_pnl << ",\n"
        << "  \"winning_trades\": 0,\n"
        << "  \"losing_trades\": 5,\n"
        << "  \"total_fills\": 10,\n"
        << "  \"total_targets\": 0,\n"
        << "  \"total_stops\": 5,\n"
        << "  \"total_commissions\": 0,\n"
        << "  \"total_spread_cost\": 0,\n"
        << "  \"total_slippage\": 0,\n"
        << "  \"total_volume\": 10000,\n"
        << "  \"positions\": []\n"
        << "}\n";
    out.close();

    SharedPortfolioState portfolio;
    portfolio.init(initial_capital);

    PositionStore store(TEST_FILE);
    store.restore(portfolio);

    double restored_cash = portfolio.cash();
    ASSERT_DOUBLE_NEAR(restored_cash, expected_cash, 100.0);

    cleanup_test_file();
}

TEST(cash_cannot_exceed_initial_plus_realized_pnl) {
    cleanup_test_file();

    // Setup: File claims positive realized P&L but impossible cash
    double initial_capital = 100000.0;
    double position_qty = 0.1;
    double position_price = 50000.0;  // $5k position
    double realized_pnl = 1000.0;  // Made $1k profit
    double corrupted_cash = 200000.0;  // Impossible - double the initial!
    double expected_cash = initial_capital - (position_qty * position_price) + realized_pnl;

    write_corrupted_file(initial_capital, corrupted_cash, position_qty, position_price, realized_pnl);

    SharedPortfolioState portfolio;
    portfolio.init(initial_capital);

    PositionStore store(TEST_FILE);
    store.restore(portfolio);

    double restored_cash = portfolio.cash();

    // Should not be $200k
    assert(restored_cash < corrupted_cash);
    // Should be reasonable
    ASSERT_DOUBLE_NEAR(restored_cash, expected_cash, 1000.0);

    cleanup_test_file();
}

int main() {
    std::cout << "\n=== Position Store Tests ===\n\n";

    std::cout << "Cash Validation Tests:\n";
    RUN_TEST(corrupted_cash_is_corrected);
    RUN_TEST(valid_cash_is_preserved);
    RUN_TEST(massive_oversell_attack_corrected);
    RUN_TEST(negative_realized_pnl_reduces_cash);
    RUN_TEST(cash_cannot_exceed_initial_plus_realized_pnl);

    std::cout << "\n=== All tests PASSED! ===\n\n";
    return 0;
}
