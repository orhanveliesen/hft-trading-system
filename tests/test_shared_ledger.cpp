/**
 * Test SharedLedger - Shared memory ledger for IPC
 */

#include "../include/ipc/shared_ledger.hpp"

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

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

// Helper to populate entry
void populate_entry(hft::ipc::SharedLedgerEntry* e, bool is_buy, double price, double qty, double pnl) {
    e->price_x8.store(static_cast<int64_t>(price * hft::ipc::LEDGER_FIXED_SCALE));
    e->quantity_x8.store(static_cast<int64_t>(qty * hft::ipc::LEDGER_FIXED_SCALE));
    e->realized_pnl_x8.store(static_cast<int64_t>(pnl * hft::ipc::LEDGER_FIXED_SCALE));
    e->is_buy.store(is_buy ? 1 : 0);
    std::strcpy(e->ticker, "BTCUSDT");
}

// =============================================================================
// TEST 1: Create and destroy shared ledger
// =============================================================================
TEST(shared_ledger_create_destroy) {
    using namespace hft::ipc;

    const char* name = "/test_ledger_1";

    // Clean up any existing
    SharedLedger::destroy(name);

    // Create
    auto* ledger = SharedLedger::create(name);
    ASSERT_TRUE(ledger != nullptr);
    ASSERT_TRUE(ledger->is_valid());
    ASSERT_EQ(ledger->count(), 0u);

    // Cleanup
    SharedLedger::unmap(ledger);
    SharedLedger::destroy(name);
}

// =============================================================================
// TEST 2: Append entries
// =============================================================================
TEST(shared_ledger_append_entries) {
    using namespace hft::ipc;

    const char* name = "/test_ledger_2";
    SharedLedger::destroy(name);

    auto* ledger = SharedLedger::create(name);
    ASSERT_TRUE(ledger != nullptr);

    // Append 3 entries
    auto* e1 = ledger->append();
    populate_entry(e1, true, 100.0, 1.0, 0.0);

    auto* e2 = ledger->append();
    populate_entry(e2, false, 110.0, 1.0, 10.0);

    auto* e3 = ledger->append();
    populate_entry(e3, true, 105.0, 2.0, 0.0);

    ASSERT_EQ(ledger->count(), 3u);
    ASSERT_EQ(ledger->entry(0)->sequence.load(), 1u);
    ASSERT_EQ(ledger->entry(1)->sequence.load(), 2u);
    ASSERT_EQ(ledger->entry(2)->sequence.load(), 3u);

    // Verify values
    ASSERT_NEAR(ledger->entry(0)->price(), 100.0, 0.01);
    ASSERT_NEAR(ledger->entry(1)->realized_pnl(), 10.0, 0.01);
    ASSERT_NEAR(ledger->entry(2)->quantity(), 2.0, 0.01);

    SharedLedger::unmap(ledger);
    SharedLedger::destroy(name);
}

// =============================================================================
// TEST 3: Circular buffer wraps around
// =============================================================================
TEST(shared_ledger_circular_buffer) {
    using namespace hft::ipc;

    const char* name = "/test_ledger_3";
    SharedLedger::destroy(name);

    auto* ledger = SharedLedger::create(name);

    // Fill beyond capacity
    for (size_t i = 0; i < MAX_SHARED_LEDGER_ENTRIES + 100; i++) {
        auto* e = ledger->append();
        e->price_x8.store(static_cast<int64_t>((i + 1) * hft::ipc::LEDGER_FIXED_SCALE));
    }

    // Count should be at max
    ASSERT_EQ(ledger->count(), MAX_SHARED_LEDGER_ENTRIES);

    // First entry should be the one after we started overwriting
    // Sequence 1-100 were overwritten, so first is 101
    ASSERT_EQ(ledger->first()->sequence.load(), 101u);

    // Last entry should be the most recent
    ASSERT_EQ(ledger->last()->sequence.load(), MAX_SHARED_LEDGER_ENTRIES + 100u);

    SharedLedger::unmap(ledger);
    SharedLedger::destroy(name);
}

// =============================================================================
// TEST 4: Open from another "process" (simulated)
// =============================================================================
TEST(shared_ledger_open_read) {
    using namespace hft::ipc;

    const char* name = "/test_ledger_4";
    SharedLedger::destroy(name);

    // Writer creates
    auto* writer = SharedLedger::create(name);
    auto* e = writer->append();
    populate_entry(e, true, 12345.67, 0.5, 0.0);

    // Reader opens
    auto* reader = SharedLedger::open(name);
    ASSERT_TRUE(reader != nullptr);
    ASSERT_TRUE(reader->is_valid());
    ASSERT_EQ(reader->count(), 1u);

    // Reader sees the entry
    auto* re = reader->entry(0);
    ASSERT_NEAR(re->price(), 12345.67, 0.01);
    ASSERT_NEAR(re->quantity(), 0.5, 0.001);
    ASSERT_EQ(re->is_buy.load(), 1u);

    SharedLedger::unmap(reader);
    SharedLedger::unmap(writer);
    SharedLedger::destroy(name);
}

// =============================================================================
// TEST 5: Mismatch detection
// =============================================================================
TEST(shared_ledger_mismatch_detection) {
    using namespace hft::ipc;

    const char* name = "/test_ledger_5";
    SharedLedger::destroy(name);

    auto* ledger = SharedLedger::create(name);

    // Entry with no mismatch
    auto* e1 = ledger->append();
    e1->balance_ok.store(1);
    e1->pnl_ok.store(1);

    // Entry with balance mismatch
    auto* e2 = ledger->append();
    e2->balance_ok.store(0); // MISMATCH!
    e2->pnl_ok.store(1);

    // Entry with pnl mismatch
    auto* e3 = ledger->append();
    e3->balance_ok.store(1);
    e3->pnl_ok.store(0); // MISMATCH!

    ASSERT_EQ(ledger->check_mismatches(), 2u);
    ASSERT_TRUE(!ledger->entry(0)->has_mismatch());
    ASSERT_TRUE(ledger->entry(1)->has_mismatch());
    ASSERT_TRUE(ledger->entry(2)->has_mismatch());

    SharedLedger::unmap(ledger);
    SharedLedger::destroy(name);
}

// =============================================================================
// TEST 6: Fixed-point conversion accuracy
// =============================================================================
TEST(shared_ledger_fixed_point_accuracy) {
    using namespace hft::ipc;

    const char* name = "/test_ledger_6";
    SharedLedger::destroy(name);

    auto* ledger = SharedLedger::create(name);
    auto* e = ledger->append();

    // Test various values
    double test_price = 91234.56789012;
    double test_qty = 0.00012345;
    double test_pnl = -123.456789;

    e->price_x8.store(static_cast<int64_t>(test_price * LEDGER_FIXED_SCALE));
    e->quantity_x8.store(static_cast<int64_t>(test_qty * LEDGER_FIXED_SCALE));
    e->realized_pnl_x8.store(static_cast<int64_t>(test_pnl * LEDGER_FIXED_SCALE));

    // 8 decimal places precision
    ASSERT_NEAR(e->price(), test_price, 0.000001);
    ASSERT_NEAR(e->quantity(), test_qty, 0.00000001);
    ASSERT_NEAR(e->realized_pnl(), test_pnl, 0.000001);

    SharedLedger::unmap(ledger);
    SharedLedger::destroy(name);
}

// =============================================================================
// TEST 7: All conversion helpers
// =============================================================================
TEST(shared_ledger_all_conversion_helpers) {
    using namespace hft::ipc;

    const char* name = "/test_ledger_7";
    SharedLedger::destroy(name);

    auto* ledger = SharedLedger::create(name);
    auto* e = ledger->append();

    // Set all fixed-point fields
    e->commission_x8.store(static_cast<int64_t>(1.23 * LEDGER_FIXED_SCALE));
    e->cash_before_x8.store(static_cast<int64_t>(10000.0 * LEDGER_FIXED_SCALE));
    e->cash_after_x8.store(static_cast<int64_t>(9900.0 * LEDGER_FIXED_SCALE));
    e->cash_expected_x8.store(static_cast<int64_t>(9895.0 * LEDGER_FIXED_SCALE));
    e->trade_value_x8.store(static_cast<int64_t>(100.0 * LEDGER_FIXED_SCALE));
    e->expected_cash_change_x8.store(static_cast<int64_t>(-105.0 * LEDGER_FIXED_SCALE));
    e->avg_entry_x8.store(static_cast<int64_t>(50.0 * LEDGER_FIXED_SCALE));
    e->pnl_per_unit_x8.store(static_cast<int64_t>(2.5 * LEDGER_FIXED_SCALE));
    e->expected_pnl_x8.store(static_cast<int64_t>(5.0 * LEDGER_FIXED_SCALE));
    e->position_qty_x8.store(static_cast<int64_t>(10.0 * LEDGER_FIXED_SCALE));
    e->position_avg_x8.store(static_cast<int64_t>(52.5 * LEDGER_FIXED_SCALE));
    e->running_realized_pnl_x8.store(static_cast<int64_t>(123.45 * LEDGER_FIXED_SCALE));
    e->running_commission_x8.store(static_cast<int64_t>(6.78 * LEDGER_FIXED_SCALE));

    // Test all conversion helpers
    ASSERT_NEAR(e->commission(), 1.23, 0.01);
    ASSERT_NEAR(e->cash_before(), 10000.0, 0.01);
    ASSERT_NEAR(e->cash_after(), 9900.0, 0.01);
    ASSERT_NEAR(e->cash_expected(), 9895.0, 0.01);
    ASSERT_NEAR(e->trade_value(), 100.0, 0.01);
    ASSERT_NEAR(e->expected_cash_change(), -105.0, 0.01);
    ASSERT_NEAR(e->avg_entry(), 50.0, 0.01);
    ASSERT_NEAR(e->pnl_per_unit(), 2.5, 0.01);
    ASSERT_NEAR(e->expected_pnl(), 5.0, 0.01);
    ASSERT_NEAR(e->position_qty(), 10.0, 0.01);
    ASSERT_NEAR(e->position_avg(), 52.5, 0.01);
    ASSERT_NEAR(e->running_realized_pnl(), 123.45, 0.01);
    ASSERT_NEAR(e->running_commission(), 6.78, 0.01);

    // Test discrepancy helpers
    ASSERT_NEAR(e->cash_discrepancy(), 5.0, 0.01); // 9900 - 9895
    ASSERT_NEAR(e->pnl_discrepancy(), -5.0, 0.01); // 0 - 5 (realized_pnl not set)

    SharedLedger::unmap(ledger);
    SharedLedger::destroy(name);
}

// =============================================================================
// TEST 8: Edge cases - empty ledger queries
// =============================================================================
TEST(shared_ledger_empty_queries) {
    using namespace hft::ipc;

    const char* name = "/test_ledger_8";
    SharedLedger::destroy(name);

    auto* ledger = SharedLedger::create(name);

    // Query empty ledger
    ASSERT_EQ(ledger->count(), 0u);
    ASSERT_TRUE(ledger->first() == nullptr);
    ASSERT_TRUE(ledger->last() == nullptr);
    ASSERT_TRUE(ledger->entry(0) == nullptr);
    ASSERT_TRUE(ledger->entry(100) == nullptr);
    ASSERT_EQ(ledger->check_mismatches(), 0u);

    SharedLedger::unmap(ledger);
    SharedLedger::destroy(name);
}

// =============================================================================
// TEST 9: Out-of-bounds entry access
// =============================================================================
TEST(shared_ledger_out_of_bounds) {
    using namespace hft::ipc;

    const char* name = "/test_ledger_9";
    SharedLedger::destroy(name);

    auto* ledger = SharedLedger::create(name);

    // Add 3 entries
    for (int i = 0; i < 3; i++) {
        ledger->append();
    }

    ASSERT_EQ(ledger->count(), 3u);
    ASSERT_TRUE(ledger->entry(0) != nullptr);
    ASSERT_TRUE(ledger->entry(2) != nullptr);

    // Out of bounds
    ASSERT_TRUE(ledger->entry(3) == nullptr);
    ASSERT_TRUE(ledger->entry(100) == nullptr);

    SharedLedger::unmap(ledger);
    SharedLedger::destroy(name);
}

// =============================================================================
// TEST 10: record_mismatch increments counter
// =============================================================================
TEST(shared_ledger_record_mismatch) {
    using namespace hft::ipc;

    const char* name = "/test_ledger_10";
    SharedLedger::destroy(name);

    auto* ledger = SharedLedger::create(name);

    ASSERT_EQ(ledger->mismatch_count.load(), 0u);

    ledger->record_mismatch();
    ASSERT_EQ(ledger->mismatch_count.load(), 1u);

    ledger->record_mismatch();
    ledger->record_mismatch();
    ASSERT_EQ(ledger->mismatch_count.load(), 3u);

    SharedLedger::unmap(ledger);
    SharedLedger::destroy(name);
}

// =============================================================================
// TEST 11: open_rw allows read-write access
// =============================================================================
TEST(shared_ledger_open_rw) {
    using namespace hft::ipc;

    const char* name = "/test_ledger_11";
    SharedLedger::destroy(name);

    // Create initial ledger
    auto* writer = SharedLedger::create(name);
    auto* e = writer->append();
    e->price_x8.store(static_cast<int64_t>(100.0 * LEDGER_FIXED_SCALE));
    SharedLedger::unmap(writer);

    // Open with read-write
    auto* rw = SharedLedger::open_rw(name);
    ASSERT_TRUE(rw != nullptr);
    ASSERT_TRUE(rw->is_valid());
    ASSERT_EQ(rw->count(), 1u);

    // Verify can read
    auto* re = rw->entry(0);
    ASSERT_NEAR(re->price(), 100.0, 0.01);

    // Verify can write
    auto* e2 = rw->append();
    e2->price_x8.store(static_cast<int64_t>(200.0 * LEDGER_FIXED_SCALE));
    ASSERT_EQ(rw->count(), 2u);

    SharedLedger::unmap(rw);
    SharedLedger::destroy(name);
}

// =============================================================================
// TEST 12: open_rw returns nullptr for invalid ledger
// =============================================================================
TEST(shared_ledger_open_rw_invalid) {
    using namespace hft::ipc;

    const char* name = "/test_ledger_12";
    SharedLedger::destroy(name);

    // Create ledger with wrong magic
    auto* ledger = SharedLedger::create(name);
    ledger->magic = 0xDEADBEEF; // Corrupt magic
    SharedLedger::unmap(ledger);

    // open_rw should return nullptr
    auto* rw = SharedLedger::open_rw(name);
    ASSERT_TRUE(rw == nullptr);

    SharedLedger::destroy(name);
}

// =============================================================================
// TEST 13: open returns nullptr for invalid ledger
// =============================================================================
TEST(shared_ledger_open_invalid) {
    using namespace hft::ipc;

    const char* name = "/test_ledger_13";
    SharedLedger::destroy(name);

    // Create ledger with wrong magic
    auto* ledger = SharedLedger::create(name);
    ledger->magic = 0xDEADBEEF; // Corrupt magic
    SharedLedger::unmap(ledger);

    // open should return nullptr
    auto* reader = SharedLedger::open(name);
    ASSERT_TRUE(reader == nullptr);

    SharedLedger::destroy(name);
}

// =============================================================================
// TEST 14: open returns nullptr for non-existent ledger
// =============================================================================
TEST(shared_ledger_open_nonexistent) {
    using namespace hft::ipc;

    const char* name = "/test_ledger_nonexist";
    SharedLedger::destroy(name);

    auto* ledger = SharedLedger::open(name);
    ASSERT_TRUE(ledger == nullptr);
}

// =============================================================================
// TEST 15: open_rw returns nullptr for non-existent ledger
// =============================================================================
TEST(shared_ledger_open_rw_nonexistent) {
    using namespace hft::ipc;

    const char* name = "/test_ledger_rw_nonexist";
    SharedLedger::destroy(name);

    auto* ledger = SharedLedger::open_rw(name);
    ASSERT_TRUE(ledger == nullptr);
}

// =============================================================================
// TEST 16: total_entries tracks all writes including wraparound
// =============================================================================
TEST(shared_ledger_total_entries_tracking) {
    using namespace hft::ipc;

    const char* name = "/test_ledger_16";
    SharedLedger::destroy(name);

    auto* ledger = SharedLedger::create(name);

    ASSERT_EQ(ledger->total_entries.load(), 0u);

    // Add 10 entries
    for (int i = 0; i < 10; i++) {
        ledger->append();
    }

    ASSERT_EQ(ledger->total_entries.load(), 10u);
    ASSERT_EQ(ledger->count(), 10u);

    SharedLedger::unmap(ledger);
    SharedLedger::destroy(name);
}

// =============================================================================
// TEST 17: session_id is set on init
// =============================================================================
TEST(shared_ledger_session_id) {
    using namespace hft::ipc;

    const char* name = "/test_ledger_17";
    SharedLedger::destroy(name);

    auto* ledger = SharedLedger::create(name);

    // Session ID should be non-zero (timestamp)
    ASSERT_TRUE(ledger->session_id > 0);

    SharedLedger::unmap(ledger);
    SharedLedger::destroy(name);
}

// =============================================================================
// TEST 18: unmap with nullptr is safe
// =============================================================================
TEST(shared_ledger_unmap_nullptr) {
    using namespace hft::ipc;

    // Should not crash
    SharedLedger::unmap(nullptr);

    ASSERT_TRUE(true);
}

// =============================================================================
// MAIN
// =============================================================================
int main() {
    std::cout << "\n=== SharedLedger Tests ===\n\n";

    RUN_TEST(shared_ledger_create_destroy);
    RUN_TEST(shared_ledger_append_entries);
    RUN_TEST(shared_ledger_circular_buffer);
    RUN_TEST(shared_ledger_open_read);
    RUN_TEST(shared_ledger_mismatch_detection);
    RUN_TEST(shared_ledger_fixed_point_accuracy);
    RUN_TEST(shared_ledger_all_conversion_helpers);
    RUN_TEST(shared_ledger_empty_queries);
    RUN_TEST(shared_ledger_out_of_bounds);
    RUN_TEST(shared_ledger_record_mismatch);
    RUN_TEST(shared_ledger_open_rw);
    RUN_TEST(shared_ledger_open_rw_invalid);
    RUN_TEST(shared_ledger_open_invalid);
    RUN_TEST(shared_ledger_open_nonexistent);
    RUN_TEST(shared_ledger_open_rw_nonexistent);
    RUN_TEST(shared_ledger_total_entries_tracking);
    RUN_TEST(shared_ledger_session_id);
    RUN_TEST(shared_ledger_unmap_nullptr);

    std::cout << "\n=== All 18 tests passed! ===\n\n";
    return 0;
}
