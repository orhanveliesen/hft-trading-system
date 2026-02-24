#include "../include/orderbook.hpp"
#include "../include/types.hpp"

#include <cassert>
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

// Test: Empty order book should have no best bid/ask
TEST(test_empty_orderbook) {
    OrderBook book(0, 100'000); // base=0, range=100k for test prices

    ASSERT_EQ(book.best_bid(), INVALID_PRICE);
    ASSERT_EQ(book.best_ask(), INVALID_PRICE);
    ASSERT_EQ(book.bid_quantity_at(10000), 0);
    ASSERT_EQ(book.ask_quantity_at(10000), 0);
}

// Test: Add buy order and verify best bid
TEST(test_add_buy_order) {
    OrderBook book(0, 100'000);

    book.add_order(1, Side::Buy, 10000, 100); // id=1, price=$1.00, qty=100

    ASSERT_EQ(book.best_bid(), 10000);
    ASSERT_EQ(book.best_ask(), INVALID_PRICE);
    ASSERT_EQ(book.bid_quantity_at(10000), 100);
}

// Test: Add sell order and verify best ask
TEST(test_add_sell_order) {
    OrderBook book(0, 100'000);

    book.add_order(1, Side::Sell, 10100, 50); // id=1, price=$1.01, qty=50

    ASSERT_EQ(book.best_bid(), INVALID_PRICE);
    ASSERT_EQ(book.best_ask(), 10100);
    ASSERT_EQ(book.ask_quantity_at(10100), 50);
}

// Test: Multiple orders at same price level
TEST(test_multiple_orders_same_price) {
    OrderBook book(0, 100'000);

    book.add_order(1, Side::Buy, 10000, 100);
    book.add_order(2, Side::Buy, 10000, 200);

    ASSERT_EQ(book.best_bid(), 10000);
    ASSERT_EQ(book.bid_quantity_at(10000), 300); // 100 + 200
}

// Test: Best bid is highest price
TEST(test_best_bid_is_highest) {
    OrderBook book(0, 100'000);

    book.add_order(1, Side::Buy, 10000, 100);
    book.add_order(2, Side::Buy, 10100, 100); // higher price
    book.add_order(3, Side::Buy, 9900, 100);  // lower price

    ASSERT_EQ(book.best_bid(), 10100); // highest wins
}

// Test: Best ask is lowest price
TEST(test_best_ask_is_lowest) {
    OrderBook book(0, 100'000);

    book.add_order(1, Side::Sell, 10200, 100);
    book.add_order(2, Side::Sell, 10100, 100); // lower price
    book.add_order(3, Side::Sell, 10300, 100); // higher price

    ASSERT_EQ(book.best_ask(), 10100); // lowest wins
}

// Test: Cancel order removes it from book
TEST(test_cancel_order) {
    OrderBook book(0, 100'000);

    book.add_order(1, Side::Buy, 10000, 100);
    book.add_order(2, Side::Buy, 10000, 200);

    ASSERT_TRUE(book.cancel_order(1));
    ASSERT_EQ(book.bid_quantity_at(10000), 200); // only order 2 remains
}

// Test: Cancel last order at price level removes the level
TEST(test_cancel_removes_price_level) {
    OrderBook book(0, 100'000);

    book.add_order(1, Side::Buy, 10000, 100);
    book.cancel_order(1);

    ASSERT_EQ(book.best_bid(), INVALID_PRICE);
    ASSERT_EQ(book.bid_quantity_at(10000), 0);
}

// Test: Partial execution reduces quantity
TEST(test_partial_execution) {
    OrderBook book(0, 100'000);

    book.add_order(1, Side::Buy, 10000, 100);
    book.execute_order(1, 30); // execute 30 of 100

    ASSERT_EQ(book.bid_quantity_at(10000), 70); // 100 - 30
}

// Test: Full execution removes order
TEST(test_full_execution) {
    OrderBook book(0, 100'000);

    book.add_order(1, Side::Buy, 10000, 100);
    book.execute_order(1, 100); // execute all

    ASSERT_EQ(book.best_bid(), INVALID_PRICE);
    ASSERT_EQ(book.bid_quantity_at(10000), 0);
}

// Test: Cancel non-existent order returns false
TEST(test_cancel_nonexistent) {
    OrderBook book(0, 100'000);

    ASSERT_FALSE(book.cancel_order(999));
}

int main() {
    std::cout << "=== OrderBook Tests ===\n";

    RUN_TEST(test_empty_orderbook);
    RUN_TEST(test_add_buy_order);
    RUN_TEST(test_add_sell_order);
    RUN_TEST(test_multiple_orders_same_price);
    RUN_TEST(test_best_bid_is_highest);
    RUN_TEST(test_best_ask_is_lowest);
    RUN_TEST(test_cancel_order);
    RUN_TEST(test_cancel_removes_price_level);
    RUN_TEST(test_partial_execution);
    RUN_TEST(test_full_execution);
    RUN_TEST(test_cancel_nonexistent);

    std::cout << "\nAll tests PASSED!\n";
    return 0;
}
