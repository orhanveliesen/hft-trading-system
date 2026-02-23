#include "../include/feed_handler.hpp"
#include "../include/itch_messages.hpp"
#include "../include/market_data_handler.hpp"
#include "../include/orderbook.hpp"
#include "../include/types.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

using namespace hft;

#define TEST(name) void name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "Running " << #name << "... ";                                                                    \
        name();                                                                                                        \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

#define ASSERT_EQ(a, b) assert((a) == (b))

// Helper functions for building ITCH messages
void write_be16(uint8_t* buf, uint16_t val) {
    buf[0] = (val >> 8) & 0xFF;
    buf[1] = val & 0xFF;
}

void write_be32(uint8_t* buf, uint32_t val) {
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8) & 0xFF;
    buf[3] = val & 0xFF;
}

void write_be64(uint8_t* buf, uint64_t val) {
    for (int i = 7; i >= 0; --i) {
        buf[7 - i] = (val >> (i * 8)) & 0xFF;
    }
}

std::vector<uint8_t> build_add_order(uint64_t order_ref, char side, uint32_t shares, uint32_t price) {
    std::vector<uint8_t> msg(36);
    msg[0] = 'A';
    write_be16(&msg[1], 1);
    write_be16(&msg[3], 0);
    write_be64(&msg[11], order_ref);
    msg[19] = side;
    write_be32(&msg[20], shares);
    std::memcpy(&msg[24], "AAPL    ", 8);
    write_be32(&msg[32], price);
    return msg;
}

std::vector<uint8_t> build_order_executed(uint64_t order_ref, uint32_t shares) {
    std::vector<uint8_t> msg(31);
    msg[0] = 'E';
    write_be16(&msg[1], 1);
    write_be16(&msg[3], 0);
    write_be64(&msg[11], order_ref);
    write_be32(&msg[19], shares);
    write_be64(&msg[23], 12345);
    return msg;
}

std::vector<uint8_t> build_order_delete(uint64_t order_ref) {
    std::vector<uint8_t> msg(19);
    msg[0] = 'D';
    write_be16(&msg[1], 1);
    write_be16(&msg[3], 0);
    write_be64(&msg[11], order_ref);
    return msg;
}

std::vector<uint8_t> build_order_cancel(uint64_t order_ref, uint32_t shares) {
    std::vector<uint8_t> msg(23);
    msg[0] = 'X';
    write_be16(&msg[1], 1);
    write_be16(&msg[3], 0);
    write_be64(&msg[11], order_ref);
    write_be32(&msg[19], shares);
    return msg;
}

// Test: Feed handler updates order book on Add Order
TEST(test_feed_add_updates_book) {
    OrderBook book;
    MarketDataHandler handler(book);
    FeedHandler<MarketDataHandler> feed(handler);

    auto msg = build_add_order(100, 'B', 500, 150000); // Buy 500 @ $15.00
    feed.process_message(msg.data(), msg.size());

    ASSERT_EQ(book.best_bid(), 150000);
    ASSERT_EQ(book.bid_quantity_at(150000), 500);
}

// Test: Multiple orders build the book correctly
TEST(test_feed_builds_book) {
    OrderBook book;
    MarketDataHandler handler(book);
    FeedHandler<MarketDataHandler> feed(handler);

    // Add buy orders at different prices
    feed.process_message(build_add_order(1, 'B', 100, 149000).data(), 36); // $14.90
    feed.process_message(build_add_order(2, 'B', 200, 150000).data(), 36); // $15.00
    feed.process_message(build_add_order(3, 'B', 150, 150000).data(), 36); // $15.00

    // Add sell orders
    feed.process_message(build_add_order(4, 'S', 100, 151000).data(), 36); // $15.10
    feed.process_message(build_add_order(5, 'S', 200, 152000).data(), 36); // $15.20

    ASSERT_EQ(book.best_bid(), 150000);           // Highest buy
    ASSERT_EQ(book.best_ask(), 151000);           // Lowest sell
    ASSERT_EQ(book.bid_quantity_at(150000), 350); // 200 + 150
    ASSERT_EQ(book.bid_quantity_at(149000), 100);
}

// Test: Order execution reduces book quantity
TEST(test_feed_execution_reduces_book) {
    OrderBook book;
    MarketDataHandler handler(book);
    FeedHandler<MarketDataHandler> feed(handler);

    feed.process_message(build_add_order(1, 'B', 100, 150000).data(), 36);
    feed.process_message(build_order_executed(1, 30).data(), 31); // Execute 30

    ASSERT_EQ(book.bid_quantity_at(150000), 70); // 100 - 30
}

// Test: Full execution removes order
TEST(test_feed_full_execution_removes_order) {
    OrderBook book;
    MarketDataHandler handler(book);
    FeedHandler<MarketDataHandler> feed(handler);

    feed.process_message(build_add_order(1, 'B', 100, 150000).data(), 36);
    feed.process_message(build_order_executed(1, 100).data(), 31); // Full execution

    ASSERT_EQ(book.best_bid(), INVALID_PRICE);
    ASSERT_EQ(book.bid_quantity_at(150000), 0);
}

// Test: Order delete removes from book
TEST(test_feed_delete_removes_order) {
    OrderBook book;
    MarketDataHandler handler(book);
    FeedHandler<MarketDataHandler> feed(handler);

    feed.process_message(build_add_order(1, 'B', 100, 150000).data(), 36);
    feed.process_message(build_add_order(2, 'B', 200, 150000).data(), 36);
    feed.process_message(build_order_delete(1).data(), 19); // Delete order 1

    ASSERT_EQ(book.bid_quantity_at(150000), 200); // Only order 2 remains
}

// Test: Order cancel reduces quantity
TEST(test_feed_cancel_reduces_quantity) {
    OrderBook book;
    MarketDataHandler handler(book);
    FeedHandler<MarketDataHandler> feed(handler);

    feed.process_message(build_add_order(1, 'B', 100, 150000).data(), 36);
    feed.process_message(build_order_cancel(1, 40).data(), 23); // Cancel 40

    ASSERT_EQ(book.bid_quantity_at(150000), 60); // 100 - 40
}

// Test: Realistic trading sequence
TEST(test_realistic_trading_sequence) {
    OrderBook book;
    MarketDataHandler handler(book);
    FeedHandler<MarketDataHandler> feed(handler);

    // Market opens - orders arrive
    feed.process_message(build_add_order(1, 'B', 1000, 100000).data(), 36); // Bid @ $10.00
    feed.process_message(build_add_order(2, 'B', 500, 99900).data(), 36);   // Bid @ $9.99
    feed.process_message(build_add_order(3, 'S', 800, 100100).data(), 36);  // Ask @ $10.01
    feed.process_message(build_add_order(4, 'S', 600, 100200).data(), 36);  // Ask @ $10.02

    ASSERT_EQ(book.best_bid(), 100000);
    ASSERT_EQ(book.best_ask(), 100100);

    // Trade happens - partial fill on order 1
    feed.process_message(build_order_executed(1, 300).data(), 31);
    ASSERT_EQ(book.bid_quantity_at(100000), 700);

    // Trader cancels remaining order 1
    feed.process_message(build_order_delete(1).data(), 19);
    ASSERT_EQ(book.best_bid(), 99900); // Next level becomes best

    // New aggressive bid
    feed.process_message(build_add_order(5, 'B', 200, 100050).data(), 36);
    ASSERT_EQ(book.best_bid(), 100050);
}

int main() {
    std::cout << "=== Integration Tests (Feed -> OrderBook) ===\n";

    RUN_TEST(test_feed_add_updates_book);
    RUN_TEST(test_feed_builds_book);
    RUN_TEST(test_feed_execution_reduces_book);
    RUN_TEST(test_feed_full_execution_removes_order);
    RUN_TEST(test_feed_delete_removes_order);
    RUN_TEST(test_feed_cancel_reduces_quantity);
    RUN_TEST(test_realistic_trading_sequence);

    std::cout << "\nAll tests PASSED!\n";
    return 0;
}
