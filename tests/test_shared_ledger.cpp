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

    std::cout << "\n=== All tests passed! ===\n\n";
    return 0;
}
