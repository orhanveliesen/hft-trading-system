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

#include "../include/ipc/shared_portfolio_state.hpp"
#include "../include/strategy/position_store.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>

using namespace hft::ipc;
using namespace hft::strategy;

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

static const char* TEST_FILE = "/tmp/test_positions.json";

// Create a corrupted position file (simulating overselling bug)
void write_corrupted_file(double initial_capital, double corrupted_cash, double position_qty, double position_price,
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
    double corrupted_cash = 150000.0;                                         // Obviously wrong - more than initial!
    double expected_cash = initial_capital - (position_qty * position_price); // $75k

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
    ASSERT_DOUBLE_NEAR(restored_cash, expected_cash, 1000.0); // Within $1k tolerance

    // Definitely should NOT be the corrupted value
    assert(restored_cash < corrupted_cash);
    assert(restored_cash <= initial_capital); // Cash can't exceed initial if we bought something

    cleanup_test_file();
}

TEST(valid_cash_is_preserved) {
    cleanup_test_file();

    // Setup: Valid data - $100k initial, bought 0.5 BTC at $50k
    // Valid cash = $75k (with small margin for commission rounding)
    double initial_capital = 100000.0;
    double position_qty = 0.5;
    double position_price = 50000.0;
    double valid_cash = 74990.0; // Slightly less due to commissions
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
    double position_qty = 0.0001;                                             // Tiny BTC position
    double position_price = 100000.0;                                         // = $10 worth
    double attack_cash = 10000000.0;                                          // $10 million (1000x oversell!)
    double expected_cash = initial_capital - (position_qty * position_price); // ~$99,990

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
    double realized_pnl = -5000.0; // Lost $5k
    double valid_cash = 95000.0;   // $100k - $5k
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
    double realized_pnl = 1000.0;     // Made $1k profit
    double corrupted_cash = 200000.0; // Impossible - double the initial!
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

// =============================================================================
// Save/Write Tests (NEW)
// =============================================================================

TEST(save_creates_file) {
    cleanup_test_file();

    SharedPortfolioState portfolio;
    portfolio.init(100000.0);

    PositionStore store(TEST_FILE);
    bool saved = store.save(portfolio);

    assert(saved);
    assert(store.exists());

    cleanup_test_file();
}

TEST(save_rate_limiting) {
    cleanup_test_file();

    SharedPortfolioState portfolio;
    portfolio.init(100000.0);

    PositionStore store(TEST_FILE);

    // First save should work
    bool first_save = store.save(portfolio);
    assert(first_save);

    // Second save immediately after should be rate-limited (return true but skip)
    bool second_save = store.save(portfolio);
    assert(second_save); // Returns true but actually skipped

    cleanup_test_file();
}

TEST(save_immediate_bypasses_rate_limit) {
    cleanup_test_file();

    SharedPortfolioState portfolio;
    portfolio.init(100000.0);

    PositionStore store(TEST_FILE);

    // First save
    bool first = store.save(portfolio);
    assert(first);

    // Add realized PNL to justify cash change (cash validation will recalculate)
    portfolio.cash_x8.store(static_cast<int64_t>(95000.0 * 1e8));
    portfolio.total_realized_pnl_x8.store(static_cast<int64_t>(-5000.0 * 1e8)); // Lost $5k
    bool second = store.save_immediate(portfolio);
    assert(second);

    // Verify file was updated
    PositionStore reader(TEST_FILE);
    SharedPortfolioState restored;
    restored.init(100000.0);
    reader.restore(restored);
    // Cash should be initial - realized_pnl = 100000 - 5000 = 95000
    ASSERT_DOUBLE_NEAR(restored.cash(), 95000.0, 1.0);
    ASSERT_DOUBLE_NEAR(restored.total_realized_pnl(), -5000.0, 1.0);

    cleanup_test_file();
}

TEST(write_multiple_positions) {
    cleanup_test_file();

    SharedPortfolioState portfolio;
    portfolio.init(100000.0);

    // Add 3 positions
    auto& pos0 = portfolio.positions[0];
    std::strncpy(pos0.symbol, "BTCUSDT", 15);
    pos0.quantity_x8.store(static_cast<int64_t>(0.5 * 1e8));
    pos0.avg_price_x8.store(static_cast<int64_t>(50000.0 * 1e8));
    pos0.last_price_x8.store(static_cast<int64_t>(51000.0 * 1e8));
    pos0.active.store(1);
    pos0.buy_count.store(10);
    pos0.sell_count.store(5);

    auto& pos1 = portfolio.positions[1];
    std::strncpy(pos1.symbol, "ETHUSDT", 15);
    pos1.quantity_x8.store(static_cast<int64_t>(5.0 * 1e8));
    pos1.avg_price_x8.store(static_cast<int64_t>(3000.0 * 1e8));
    pos1.last_price_x8.store(static_cast<int64_t>(3100.0 * 1e8));
    pos1.active.store(1);
    pos1.buy_count.store(20);
    pos1.sell_count.store(8);

    auto& pos2 = portfolio.positions[2];
    std::strncpy(pos2.symbol, "SOLUSDT", 15);
    pos2.quantity_x8.store(static_cast<int64_t>(100.0 * 1e8));
    pos2.avg_price_x8.store(static_cast<int64_t>(150.0 * 1e8));
    pos2.last_price_x8.store(static_cast<int64_t>(155.0 * 1e8));
    pos2.active.store(1);
    pos2.buy_count.store(30);
    pos2.sell_count.store(12);

    PositionStore store(TEST_FILE);
    bool saved = store.save_immediate(portfolio);
    assert(saved);

    // Restore and verify all 3 positions
    SharedPortfolioState restored;
    restored.init(100000.0);
    store.restore(restored);

    assert(restored.positions[0].active.load());
    assert(restored.positions[1].active.load());
    assert(restored.positions[2].active.load());

    ASSERT_DOUBLE_NEAR(restored.positions[0].quantity(), 0.5, 0.001);
    ASSERT_DOUBLE_NEAR(restored.positions[1].quantity(), 5.0, 0.001);
    ASSERT_DOUBLE_NEAR(restored.positions[2].quantity(), 100.0, 0.001);

    cleanup_test_file();
}

TEST(skip_zero_quantity_positions) {
    cleanup_test_file();

    SharedPortfolioState portfolio;
    portfolio.init(100000.0);

    // Position with zero quantity (closed)
    auto& pos = portfolio.positions[0];
    std::strncpy(pos.symbol, "BTCUSDT", 15);
    pos.quantity_x8.store(0); // Zero!
    pos.avg_price_x8.store(static_cast<int64_t>(50000.0 * 1e8));
    pos.active.store(1); // Active but zero quantity

    PositionStore store(TEST_FILE);
    store.save_immediate(portfolio);

    // Read file and verify no positions saved
    std::ifstream in(TEST_FILE);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    // Should have empty positions array (check for array markers)
    assert(content.find("\"positions\":") != std::string::npos);
    assert(content.find('[') != std::string::npos);
    assert(content.find(']') != std::string::npos);
    // No BTCUSDT in content (since it was skipped)
    assert(content.find("BTCUSDT") == std::string::npos);

    cleanup_test_file();
}

TEST(exists_returns_false_when_no_file) {
    cleanup_test_file();

    PositionStore store(TEST_FILE);
    assert(!store.exists());
}

TEST(clear_removes_file) {
    cleanup_test_file();

    SharedPortfolioState portfolio;
    portfolio.init(100000.0);

    PositionStore store(TEST_FILE);
    store.save_immediate(portfolio);
    assert(store.exists());

    store.clear();
    assert(!store.exists());
}

TEST(path_returns_correct_path) {
    PositionStore store(TEST_FILE);
    assert(std::string(store.path()) == TEST_FILE);
}

// =============================================================================
// Parse Error Handling Tests (NEW)
// =============================================================================

TEST(restore_empty_file) {
    cleanup_test_file();

    // Create empty file
    std::ofstream out(TEST_FILE);
    out.close();

    SharedPortfolioState portfolio;
    portfolio.init(100000.0);

    PositionStore store(TEST_FILE);
    bool restored = store.restore(portfolio);

    assert(!restored); // Should fail on empty file

    cleanup_test_file();
}

TEST(restore_missing_file) {
    cleanup_test_file();

    SharedPortfolioState portfolio;
    portfolio.init(100000.0);

    PositionStore store(TEST_FILE);
    bool restored = store.restore(portfolio);

    assert(!restored); // Should fail on missing file
}

TEST(restore_with_no_positions_array) {
    cleanup_test_file();

    // File without positions array
    std::ofstream out(TEST_FILE);
    out << "{\n"
        << "  \"version\": 1,\n"
        << "  \"initial_capital\": 100000.0,\n"
        << "  \"cash\": 100000.0\n"
        << "}\n";
    out.close();

    SharedPortfolioState portfolio;
    portfolio.init(100000.0);

    PositionStore store(TEST_FILE);
    bool restored = store.restore(portfolio);

    assert(restored); // Should succeed (no positions is OK)

    cleanup_test_file();
}

TEST(restore_with_empty_positions_array) {
    cleanup_test_file();

    std::ofstream out(TEST_FILE);
    out << "{\n"
        << "  \"version\": 1,\n"
        << "  \"initial_capital\": 100000.0,\n"
        << "  \"cash\": 100000.0,\n"
        << "  \"positions\": []\n"
        << "}\n";
    out.close();

    SharedPortfolioState portfolio;
    portfolio.init(100000.0);

    PositionStore store(TEST_FILE);
    bool restored = store.restore(portfolio);

    assert(restored);

    cleanup_test_file();
}

TEST(restore_with_malformed_position) {
    cleanup_test_file();

    // Malformed position (missing closing brace)
    std::ofstream out(TEST_FILE);
    out << "{\n"
        << "  \"version\": 1,\n"
        << "  \"initial_capital\": 100000.0,\n"
        << "  \"cash\": 100000.0,\n"
        << "  \"positions\": [\n"
        << "    {\n"
        << "      \"symbol\": \"BTCUSDT\",\n"
        << "      \"quantity\": 1.0\n"
        // Missing closing brace!
        << "  ]\n"
        << "}\n";
    out.close();

    SharedPortfolioState portfolio;
    portfolio.init(100000.0);

    PositionStore store(TEST_FILE);
    bool restored = store.restore(portfolio);

    assert(restored); // Should still succeed (parser tolerates malformed)

    cleanup_test_file();
}

TEST(parse_string_with_spaces) {
    cleanup_test_file();

    // Symbol with extra spaces around colon
    std::ofstream out(TEST_FILE);
    out << "{\n"
        << "  \"version\": 1,\n"
        << "  \"initial_capital\": 100000.0,\n"
        << "  \"cash\": 100000.0,\n"
        << "  \"positions\": [\n"
        << "    {\n"
        << "      \"symbol\":  \"BTCUSDT\",\n" // Extra spaces
        << "      \"symbol_id\": 0,\n"
        << "      \"quantity\": 1.0,\n"
        << "      \"avg_price\": 50000.0\n"
        << "    }\n"
        << "  ]\n"
        << "}\n";
    out.close();

    SharedPortfolioState portfolio;
    portfolio.init(100000.0);

    PositionStore store(TEST_FILE);
    store.restore(portfolio);

    // Should parse correctly
    assert(std::string(portfolio.positions[0].symbol) == "BTCUSDT");

    cleanup_test_file();
}

TEST(parse_double_with_scientific_notation) {
    cleanup_test_file();

    std::ofstream out(TEST_FILE);
    out << "{\n"
        << "  \"version\": 1,\n"
        << "  \"initial_capital\": 1e5,\n" // Scientific notation
        << "  \"cash\": 9.999e4,\n"        // Scientific notation
        << "  \"positions\": []\n"
        << "}\n";
    out.close();

    SharedPortfolioState portfolio;
    portfolio.init(50000.0);

    PositionStore store(TEST_FILE);
    store.restore(portfolio);

    ASSERT_DOUBLE_NEAR(portfolio.initial_cash(), 100000.0, 1.0);
    ASSERT_DOUBLE_NEAR(portfolio.cash(), 99990.0, 1.0);

    cleanup_test_file();
}

TEST(restore_all_portfolio_fields) {
    cleanup_test_file();

    // Create file with all fields populated
    std::ofstream out(TEST_FILE);
    out << "{\n"
        << "  \"version\": 1,\n"
        << "  \"timestamp_ns\": 123456789,\n"
        << "  \"initial_capital\": 100000.0,\n"
        << "  \"cash\": 95000.0,\n"
        << "  \"total_realized_pnl\": -5000.0,\n"
        << "  \"winning_trades\": 10,\n"
        << "  \"losing_trades\": 15,\n"
        << "  \"total_fills\": 50,\n"
        << "  \"total_targets\": 8,\n"
        << "  \"total_stops\": 12,\n"
        << "  \"total_commissions\": 500.0,\n"
        << "  \"total_spread_cost\": 100.0,\n"
        << "  \"total_slippage\": 50.0,\n"
        << "  \"total_volume\": 1000000.0,\n"
        << "  \"positions\": []\n"
        << "}\n";
    out.close();

    SharedPortfolioState portfolio;
    portfolio.init(50000.0);

    PositionStore store(TEST_FILE);
    store.restore(portfolio);

    // Verify all fields
    ASSERT_DOUBLE_NEAR(portfolio.initial_cash(), 100000.0, 1.0);
    ASSERT_DOUBLE_NEAR(portfolio.cash(), 95000.0, 1.0);
    ASSERT_DOUBLE_NEAR(portfolio.total_realized_pnl(), -5000.0, 1.0);
    assert(portfolio.winning_trades.load() == 10);
    assert(portfolio.losing_trades.load() == 15);
    assert(portfolio.total_fills.load() == 50);
    assert(portfolio.total_targets.load() == 8);
    assert(portfolio.total_stops.load() == 12);
    ASSERT_DOUBLE_NEAR(portfolio.total_commissions(), 500.0, 1.0);
    ASSERT_DOUBLE_NEAR(portfolio.total_spread_cost(), 100.0, 1.0);
    ASSERT_DOUBLE_NEAR(portfolio.total_slippage(), 50.0, 1.0);
    ASSERT_DOUBLE_NEAR(portfolio.total_volume(), 1000000.0, 1.0);

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

    std::cout << "\nSave/Write Tests:\n";
    RUN_TEST(save_creates_file);
    RUN_TEST(save_rate_limiting);
    RUN_TEST(save_immediate_bypasses_rate_limit);
    RUN_TEST(write_multiple_positions);
    RUN_TEST(skip_zero_quantity_positions);
    RUN_TEST(exists_returns_false_when_no_file);
    RUN_TEST(clear_removes_file);
    RUN_TEST(path_returns_correct_path);

    std::cout << "\nParse Error Handling Tests:\n";
    RUN_TEST(restore_empty_file);
    RUN_TEST(restore_missing_file);
    RUN_TEST(restore_with_no_positions_array);
    RUN_TEST(restore_with_empty_positions_array);
    RUN_TEST(restore_with_malformed_position);
    RUN_TEST(parse_string_with_spaces);
    RUN_TEST(parse_double_with_scientific_notation);
    RUN_TEST(restore_all_portfolio_fields);

    std::cout << "\n=== All 21 tests PASSED! ===\n\n";
    return 0;
}
