/**
 * Market Data Service Tests
 *
 * Tests the integrated market data service that:
 * - Receives UDP multicast packets
 * - Parses ITCH messages
 * - Updates order books
 * - Notifies strategies on BBO changes
 */

#include "../include/market_data_service.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
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
#define ASSERT_NE(a, b) assert((a) != (b))
#define ASSERT_TRUE(x) assert(x)
#define ASSERT_FALSE(x) assert(!(x))
#define ASSERT_GE(a, b) assert((a) >= (b))

// Global test state
std::vector<MarketUpdate> received_updates;

void reset_updates() {
    received_updates.clear();
}

std::unique_ptr<MarketDataService> create_test_service() {
    auto service = std::make_unique<MarketDataService>();
    reset_updates();

    service->set_update_callback([](const MarketUpdate& update) { received_updates.push_back(update); });

    // Add test symbols
    // Base price is the minimum price, range is the span
    service->add_symbol(1, "AAPL", 145 * 10000, 200000); // Base $145, range $20 (covers $145-$165)
    service->add_symbol(2, "MSFT", 345 * 10000, 200000); // Base $345, range $20 (covers $345-$365)

    return service;
}

// Single-symbol service for tests using on_add_order (which uses begin())
std::unique_ptr<MarketDataService> create_single_symbol_service() {
    auto service = std::make_unique<MarketDataService>();
    reset_updates();

    service->set_update_callback([](const MarketUpdate& update) { received_updates.push_back(update); });

    // Single symbol for deterministic behavior
    service->add_symbol(1, "AAPL", 145 * 10000, 200000); // Base $145, range $20 (covers $145-$165)

    return service;
}

// Test 1: Symbol registration and lookup
TEST(test_symbol_registration) {
    auto service = create_test_service();

    // Order book should exist for registered symbols
    ASSERT_NE(nullptr, service->get_order_book(1));
    ASSERT_NE(nullptr, service->get_order_book(2));

    // Non-existent symbol should return nullptr
    ASSERT_EQ(nullptr, service->get_order_book(999));
}

// Test 2: Add order creates book entry and triggers update
TEST(test_add_order_updates_book) {
    auto service = create_single_symbol_service();

    // Using generic interface: on_add_order(OrderId, Side, Price, Quantity)
    service->on_add_order(1001, Side::Buy, 150 * 10000, 100); // Buy 100 @ $150.00

    // Verify book updated (uses first registered symbol in single-symbol mode)
    auto* book = service->get_order_book(1);
    ASSERT_NE(nullptr, book);
    ASSERT_EQ(150 * 10000, book->best_bid());
    ASSERT_EQ(100, book->bid_quantity_at(150 * 10000));

    // Verify callback triggered
    ASSERT_EQ(1, received_updates.size());
    ASSERT_EQ(1, received_updates[0].symbol);
    ASSERT_EQ(150 * 10000, received_updates[0].best_bid);
    ASSERT_EQ(100, received_updates[0].bid_size);

    // Messages processed count
    ASSERT_EQ(1, service->messages_processed());
}

// Test 3: Add order on ask side
TEST(test_add_ask_order) {
    auto service = create_single_symbol_service();

    service->on_add_order(2001, Side::Sell, 151 * 10000, 200); // Sell 200 @ $151.00

    auto* book = service->get_order_book(1);
    ASSERT_EQ(151 * 10000, book->best_ask());
    ASSERT_EQ(200, book->ask_quantity_at(151 * 10000));
}

// Test 4: Order execution reduces quantity
TEST(test_order_execution) {
    auto service = create_single_symbol_service();

    // First add an order (use AAPL price range $145-$165)
    service->on_add_order(3001, Side::Buy, 150 * 10000, 500); // Buy 500 @ $150.00

    // Now execute partial
    service->on_order_executed(3001, 200);

    auto* book = service->get_order_book(1);
    ASSERT_EQ(300, book->bid_quantity_at(150 * 10000)); // 500 - 200 = 300
    ASSERT_EQ(2, service->messages_processed());
}

// Test 5: Order cancel reduces quantity
TEST(test_order_cancel) {
    auto service = create_single_symbol_service();

    // Add order
    service->on_add_order(4001, Side::Sell, 152 * 10000, 1000); // Sell 1000 @ $152.00

    // Cancel partial
    service->on_order_cancelled(4001, 400);

    auto* book = service->get_order_book(1);
    ASSERT_EQ(600, book->ask_quantity_at(152 * 10000)); // 1000 - 400 = 600
}

// Test 6: Order delete removes completely
TEST(test_order_delete) {
    auto service = create_single_symbol_service();

    // Add order
    service->on_add_order(5001, Side::Buy, 149 * 10000, 100); // Buy 100 @ $149.00

    // Verify order exists
    auto* book = service->get_order_book(1);
    ASSERT_EQ(100, book->bid_quantity_at(149 * 10000));

    // Delete order
    service->on_order_deleted(5001);

    // Order should be gone
    ASSERT_EQ(0, book->bid_quantity_at(149 * 10000));
}

// Test 7: Unknown order_ref in execute/cancel/delete is ignored
TEST(test_unknown_order_ref_ignored) {
    auto service = create_single_symbol_service();

    // Try to execute non-existent order
    service->on_order_executed(99999, 100);

    // Should not crash and no updates
    ASSERT_EQ(0, received_updates.size());

    // Try cancel
    service->on_order_cancelled(99999, 100);
    ASSERT_EQ(0, received_updates.size());

    // Try delete
    service->on_order_deleted(99999);
    ASSERT_EQ(0, received_updates.size());
}

// Test 8: Statistics tracking
TEST(test_statistics_tracking) {
    auto service = create_single_symbol_service();

    ASSERT_EQ(0, service->packets_received());
    ASSERT_EQ(0, service->messages_processed());

    // Add several orders (use AAPL price range $145-$165)
    for (int i = 0; i < 5; ++i) {
        service->on_add_order(10000 + i, Side::Buy, (150 + i) * 10000, 100);
    }

    ASSERT_EQ(5, service->messages_processed());
}

// Test 9: BBO update callback with bid/ask spread
TEST(test_bbo_spread_update) {
    auto service = create_single_symbol_service();

    // Add bid
    service->on_add_order(11001, Side::Buy, 150 * 10000, 100); // Bid $150.00

    // Add ask
    service->on_add_order(11002, Side::Sell, 151 * 10000, 200); // Ask $151.00

    // Last update should have both bid and ask
    ASSERT_GE(received_updates.size(), 2);
    const auto& last = received_updates.back();

    ASSERT_EQ(150 * 10000, last.best_bid);
    ASSERT_EQ(151 * 10000, last.best_ask);
    ASSERT_EQ(100, last.bid_size);
    ASSERT_EQ(200, last.ask_size);
}

// Test 10: Full execution clears order and updates BBO
TEST(test_full_execution_clears_order) {
    auto service = create_single_symbol_service();

    // Add single order
    service->on_add_order(12001, Side::Buy, 150 * 10000, 100);

    // Full execution
    service->on_order_executed(12001, 100);

    auto* book = service->get_order_book(1);
    ASSERT_EQ(0, book->bid_quantity_at(150 * 10000));
}

int main() {
    std::cout << "=== MarketDataService Tests ===\n";

    RUN_TEST(test_symbol_registration);
    RUN_TEST(test_add_order_updates_book);
    RUN_TEST(test_add_ask_order);
    RUN_TEST(test_order_execution);
    RUN_TEST(test_order_cancel);
    RUN_TEST(test_order_delete);
    RUN_TEST(test_unknown_order_ref_ignored);
    RUN_TEST(test_statistics_tracking);
    RUN_TEST(test_bbo_spread_update);
    RUN_TEST(test_full_execution_clears_order);

    std::cout << "\nAll tests PASSED!\n";
    return 0;
}
