#include "../include/top_of_book.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace hft;

#define TEST(name) void name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "Running " << #name << "... ";                                                                    \
        name();                                                                                                        \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

#define ASSERT_EQ(a, b) assert((a) == (b))
#define ASSERT_TRUE(x) assert(x)
#define ASSERT_FALSE(x) assert(!(x))
#define ASSERT_NEAR(a, b, eps) assert(std::abs((a) - (b)) < (eps))

// === Basic Tests ===

TEST(test_empty_book) {
    TopOfBook book;

    ASSERT_EQ(book.best_bid(), 0);
    ASSERT_EQ(book.best_ask(), 0);
    ASSERT_EQ(book.best_bid_size(), 0);
    ASSERT_EQ(book.best_ask_size(), 0);
    ASSERT_EQ(book.mid_price(), 0);
    ASSERT_EQ(book.spread(), INVALID_PRICE);
    ASSERT_EQ(book.bid_levels(), 0);
    ASSERT_EQ(book.ask_levels(), 0);
}

TEST(test_single_bid) {
    TopOfBook book;

    book.set_level(Side::Buy, 10000, 100);

    ASSERT_EQ(book.best_bid(), 10000);
    ASSERT_EQ(book.best_bid_size(), 100);
    ASSERT_EQ(book.bid_levels(), 1);
    ASSERT_EQ(book.ask_levels(), 0);
}

TEST(test_single_ask) {
    TopOfBook book;

    book.set_level(Side::Sell, 10100, 200);

    ASSERT_EQ(book.best_ask(), 10100);
    ASSERT_EQ(book.best_ask_size(), 200);
    ASSERT_EQ(book.ask_levels(), 1);
    ASSERT_EQ(book.bid_levels(), 0);
}

TEST(test_bbo_and_spread) {
    TopOfBook book;

    book.set_level(Side::Buy, 10000, 100);  // Best bid: $1.0000
    book.set_level(Side::Sell, 10050, 200); // Best ask: $1.0050

    ASSERT_EQ(book.best_bid(), 10000);
    ASSERT_EQ(book.best_ask(), 10050);
    ASSERT_EQ(book.spread(), 50);       // $0.0050 spread
    ASSERT_EQ(book.mid_price(), 10025); // $1.0025 mid
}

// === Level Ordering Tests ===

TEST(test_bids_sorted_descending) {
    TopOfBook book;

    // Add in random order
    book.set_level(Side::Buy, 9900, 100);
    book.set_level(Side::Buy, 10000, 200); // Best
    book.set_level(Side::Buy, 9950, 150);

    ASSERT_EQ(book.best_bid(), 10000);
    ASSERT_EQ(book.bid(0).price, 10000);
    ASSERT_EQ(book.bid(1).price, 9950);
    ASSERT_EQ(book.bid(2).price, 9900);
    ASSERT_EQ(book.bid_levels(), 3);
}

TEST(test_asks_sorted_ascending) {
    TopOfBook book;

    // Add in random order
    book.set_level(Side::Sell, 10100, 100);
    book.set_level(Side::Sell, 10000, 200); // Best
    book.set_level(Side::Sell, 10050, 150);

    ASSERT_EQ(book.best_ask(), 10000);
    ASSERT_EQ(book.ask(0).price, 10000);
    ASSERT_EQ(book.ask(1).price, 10050);
    ASSERT_EQ(book.ask(2).price, 10100);
    ASSERT_EQ(book.ask_levels(), 3);
}

// === Update Tests ===

TEST(test_update_existing_level) {
    TopOfBook book;

    book.set_level(Side::Buy, 10000, 100);
    ASSERT_EQ(book.best_bid_size(), 100);

    // Update same price
    book.set_level(Side::Buy, 10000, 500);
    ASSERT_EQ(book.best_bid_size(), 500);
    ASSERT_EQ(book.bid_levels(), 1); // Still one level
}

TEST(test_remove_level_with_zero_size) {
    TopOfBook book;

    book.set_level(Side::Buy, 10000, 100);
    book.set_level(Side::Buy, 9900, 200);
    ASSERT_EQ(book.bid_levels(), 2);

    // Remove best bid
    book.set_level(Side::Buy, 10000, 0);

    ASSERT_EQ(book.best_bid(), 9900);
    ASSERT_EQ(book.bid_levels(), 1);
}

TEST(test_max_depth_limit) {
    TopOfBook book;

    // Add 7 levels (only 5 should be kept)
    book.set_level(Side::Buy, 10000, 100); // Best
    book.set_level(Side::Buy, 9900, 100);
    book.set_level(Side::Buy, 9800, 100);
    book.set_level(Side::Buy, 9700, 100);
    book.set_level(Side::Buy, 9600, 100); // Level 5
    book.set_level(Side::Buy, 9500, 100); // Should be ignored
    book.set_level(Side::Buy, 9400, 100); // Should be ignored

    ASSERT_EQ(book.bid_levels(), 5);
    ASSERT_EQ(book.bid(4).price, 9600); // Worst tracked level
}

TEST(test_better_price_pushes_out_worst) {
    TopOfBook book;

    // Fill 5 levels
    book.set_level(Side::Buy, 9600, 100);
    book.set_level(Side::Buy, 9700, 100);
    book.set_level(Side::Buy, 9800, 100);
    book.set_level(Side::Buy, 9900, 100);
    book.set_level(Side::Buy, 10000, 100);

    ASSERT_EQ(book.bid(4).price, 9600); // Worst

    // Add better price - should push out 9600
    book.set_level(Side::Buy, 10100, 200);

    ASSERT_EQ(book.best_bid(), 10100);
    ASSERT_EQ(book.bid(4).price, 9700); // 9600 pushed out
}

// === Depth Calculations ===

TEST(test_total_depth) {
    TopOfBook book;

    book.set_level(Side::Buy, 10000, 100);
    book.set_level(Side::Buy, 9900, 200);
    book.set_level(Side::Buy, 9800, 300);

    ASSERT_EQ(book.total_bid_depth(), 600);

    book.set_level(Side::Sell, 10100, 150);
    book.set_level(Side::Sell, 10200, 250);

    ASSERT_EQ(book.total_ask_depth(), 400);
}

TEST(test_imbalance_calculation) {
    TopOfBook book;

    // Equal depth = 0 imbalance
    book.set_level(Side::Buy, 10000, 100);
    book.set_level(Side::Sell, 10100, 100);
    ASSERT_NEAR(book.imbalance(), 0.0, 0.001);

    // More bids = positive imbalance
    book.set_level(Side::Buy, 10000, 300);
    book.set_level(Side::Sell, 10100, 100);
    // imbalance = (300 - 100) / (300 + 100) = 0.5
    ASSERT_NEAR(book.imbalance(), 0.5, 0.001);

    // More asks = negative imbalance
    book.set_level(Side::Buy, 10000, 100);
    book.set_level(Side::Sell, 10100, 300);
    ASSERT_NEAR(book.imbalance(), -0.5, 0.001);
}

// === Clear Test ===

TEST(test_clear_book) {
    TopOfBook book;

    book.set_level(Side::Buy, 10000, 100);
    book.set_level(Side::Sell, 10100, 200);

    book.clear();

    ASSERT_EQ(book.best_bid(), 0);
    ASSERT_EQ(book.best_ask(), 0);
    ASSERT_EQ(book.bid_levels(), 0);
    ASSERT_EQ(book.ask_levels(), 0);
}

// === Size Verification ===

TEST(test_struct_size) {
    // Verify cache-friendly size
    ASSERT_TRUE(sizeof(TopOfBook) <= 128); // Max 2 cache lines
    ASSERT_EQ(sizeof(TopOfBook::Level), 8);
}

// === Snapshot Tests ===

TEST(test_initial_state_empty) {
    TopOfBook book;

    ASSERT_EQ(book.state(), BookState::Empty);
    ASSERT_FALSE(book.is_ready());
    ASSERT_EQ(book.sequence(), 0);
}

TEST(test_l1_snapshot_apply) {
    TopOfBook book;

    L1Snapshot snap;
    snap.bid_price = 10000;
    snap.bid_size = 100;
    snap.ask_price = 10050;
    snap.ask_size = 200;
    snap.sequence = 12345;

    book.apply_snapshot(snap);

    ASSERT_EQ(book.state(), BookState::Ready);
    ASSERT_TRUE(book.is_ready());
    ASSERT_EQ(book.sequence(), 12345);
    ASSERT_EQ(book.best_bid(), 10000);
    ASSERT_EQ(book.best_bid_size(), 100);
    ASSERT_EQ(book.best_ask(), 10050);
    ASSERT_EQ(book.best_ask_size(), 200);
}

TEST(test_l2_snapshot_apply) {
    TopOfBook book;

    L2Snapshot<5> snap;
    snap.bids[0] = {10000, 100};
    snap.bids[1] = {9900, 200};
    snap.bids[2] = {9800, 300};
    snap.bid_count = 3;

    snap.asks[0] = {10050, 150};
    snap.asks[1] = {10100, 250};
    snap.ask_count = 2;

    snap.sequence = 99999;

    book.apply_snapshot(snap);

    ASSERT_EQ(book.state(), BookState::Ready);
    ASSERT_EQ(book.sequence(), 99999);
    ASSERT_EQ(book.bid_levels(), 3);
    ASSERT_EQ(book.ask_levels(), 2);
    ASSERT_EQ(book.bid(0).price, 10000);
    ASSERT_EQ(book.bid(1).price, 9900);
    ASSERT_EQ(book.bid(2).price, 9800);
    ASSERT_EQ(book.ask(0).price, 10050);
    ASSERT_EQ(book.ask(1).price, 10100);
}

TEST(test_to_l1_snapshot) {
    TopOfBook book;

    book.set_level(Side::Buy, 10000, 100);
    book.set_level(Side::Sell, 10050, 200);
    book.set_sequence(555);

    L1Snapshot snap = book.to_l1_snapshot();

    ASSERT_EQ(snap.bid_price, 10000);
    ASSERT_EQ(snap.bid_size, 100);
    ASSERT_EQ(snap.ask_price, 10050);
    ASSERT_EQ(snap.ask_size, 200);
    ASSERT_EQ(snap.sequence, 555);
}

TEST(test_to_l2_snapshot) {
    TopOfBook book;

    book.set_level(Side::Buy, 10000, 100);
    book.set_level(Side::Buy, 9900, 200);
    book.set_level(Side::Sell, 10050, 150);
    book.set_sequence(777);

    auto snap = book.to_l2_snapshot();

    ASSERT_EQ(snap.bid_count, 2);
    ASSERT_EQ(snap.ask_count, 1);
    ASSERT_EQ(snap.bids[0].price, 10000);
    ASSERT_EQ(snap.bids[1].price, 9900);
    ASSERT_EQ(snap.asks[0].price, 10050);
    ASSERT_EQ(snap.sequence, 777);
}

TEST(test_clear_resets_state) {
    TopOfBook book;

    L1Snapshot snap;
    snap.bid_price = 10000;
    snap.bid_size = 100;
    snap.sequence = 12345;
    book.apply_snapshot(snap);

    ASSERT_TRUE(book.is_ready());

    book.clear();

    ASSERT_EQ(book.state(), BookState::Empty);
    ASSERT_FALSE(book.is_ready());
    ASSERT_EQ(book.sequence(), 0);
}

int main() {
    std::cout << "=== TopOfBook Tests ===\n";
    std::cout << "TopOfBook size: " << sizeof(TopOfBook) << " bytes\n\n";

    // Basic tests
    RUN_TEST(test_empty_book);
    RUN_TEST(test_single_bid);
    RUN_TEST(test_single_ask);
    RUN_TEST(test_bbo_and_spread);

    // Level ordering
    RUN_TEST(test_bids_sorted_descending);
    RUN_TEST(test_asks_sorted_ascending);

    // Updates
    RUN_TEST(test_update_existing_level);
    RUN_TEST(test_remove_level_with_zero_size);
    RUN_TEST(test_max_depth_limit);
    RUN_TEST(test_better_price_pushes_out_worst);

    // Depth
    RUN_TEST(test_total_depth);
    RUN_TEST(test_imbalance_calculation);

    // Clear
    RUN_TEST(test_clear_book);

    // Size
    RUN_TEST(test_struct_size);

    // Snapshots
    RUN_TEST(test_initial_state_empty);
    RUN_TEST(test_l1_snapshot_apply);
    RUN_TEST(test_l2_snapshot_apply);
    RUN_TEST(test_to_l1_snapshot);
    RUN_TEST(test_to_l2_snapshot);
    RUN_TEST(test_clear_resets_state);

    std::cout << "\nAll tests PASSED!\n";
    return 0;
}
