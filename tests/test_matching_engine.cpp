#include <cassert>
#include <iostream>
#include <vector>
#include "../include/types.hpp"
#include "../include/matching_engine.hpp"

using namespace hft;

#define TEST(name) void name()
#define RUN_TEST(name) do { \
    std::cout << "Running " << #name << "... "; \
    name(); \
    std::cout << "PASSED\n"; \
} while(0)

#define ASSERT_EQ(a, b) assert((a) == (b))
#define ASSERT_TRUE(x) assert(x)
#define ASSERT_FALSE(x) assert(!(x))

// Helper to collect trades
struct TradeCollector {
    std::vector<Trade> trades;

    void on_trade(const Trade& trade) {
        trades.push_back(trade);
    }

    void clear() { trades.clear(); }
};

// Test: No match when buy price < best ask (spread exists)
TEST(test_no_match_with_spread) {
    MatchingEngine engine(0, 100'000);
    TradeCollector collector;
    engine.set_trade_callback([&](const Trade& t) { collector.on_trade(t); });

    // Add sell order at 10100
    engine.add_order(1, Side::Sell, 10100, 100);

    // Add buy order at 10000 (below best ask)
    engine.add_order(2, Side::Buy, 10000, 100);

    // No trade should occur
    ASSERT_EQ(collector.trades.size(), 0);

    // Both orders should be resting
    ASSERT_EQ(engine.best_bid(), 10000);
    ASSERT_EQ(engine.best_ask(), 10100);
}

// Test: Full match when buy crosses spread
TEST(test_full_match_buy_crosses) {
    MatchingEngine engine(0, 100'000);
    TradeCollector collector;
    engine.set_trade_callback([&](const Trade& t) { collector.on_trade(t); });

    // Add sell order at 10100, qty=100
    engine.add_order(1, Side::Sell, 10100, 100);

    // Add buy order at 10100 (same price), qty=100
    engine.add_order(2, Side::Buy, 10100, 100);

    // One trade should occur
    ASSERT_EQ(collector.trades.size(), 1);
    ASSERT_EQ(collector.trades[0].price, 10100);
    ASSERT_EQ(collector.trades[0].quantity, 100);
    ASSERT_EQ(collector.trades[0].aggressor_side, Side::Buy);

    // Both sides should be empty
    ASSERT_EQ(engine.best_bid(), INVALID_PRICE);
    ASSERT_EQ(engine.best_ask(), INVALID_PRICE);
}

// Test: Partial match - aggressor larger than resting
TEST(test_partial_match_aggressor_larger) {
    MatchingEngine engine(0, 100'000);
    TradeCollector collector;
    engine.set_trade_callback([&](const Trade& t) { collector.on_trade(t); });

    // Add sell order at 10100, qty=50
    engine.add_order(1, Side::Sell, 10100, 50);

    // Add buy order at 10100, qty=100 (larger)
    engine.add_order(2, Side::Buy, 10100, 100);

    // Trade for 50
    ASSERT_EQ(collector.trades.size(), 1);
    ASSERT_EQ(collector.trades[0].quantity, 50);

    // Remaining 50 becomes resting bid
    ASSERT_EQ(engine.best_bid(), 10100);
    ASSERT_EQ(engine.bid_quantity_at(10100), 50);
    ASSERT_EQ(engine.best_ask(), INVALID_PRICE);
}

// Test: Partial match - aggressor smaller than resting
TEST(test_partial_match_aggressor_smaller) {
    MatchingEngine engine(0, 100'000);
    TradeCollector collector;
    engine.set_trade_callback([&](const Trade& t) { collector.on_trade(t); });

    // Add sell order at 10100, qty=100
    engine.add_order(1, Side::Sell, 10100, 100);

    // Add buy order at 10100, qty=30 (smaller)
    engine.add_order(2, Side::Buy, 10100, 30);

    // Trade for 30
    ASSERT_EQ(collector.trades.size(), 1);
    ASSERT_EQ(collector.trades[0].quantity, 30);

    // Remaining 70 stays in ask book
    ASSERT_EQ(engine.best_bid(), INVALID_PRICE);
    ASSERT_EQ(engine.best_ask(), 10100);
    ASSERT_EQ(engine.ask_quantity_at(10100), 70);
}

// Test: Price-time priority (FIFO at same price)
TEST(test_price_time_priority) {
    MatchingEngine engine(0, 100'000);
    TradeCollector collector;
    engine.set_trade_callback([&](const Trade& t) { collector.on_trade(t); });

    // Add two sell orders at same price
    engine.add_order(1, Side::Sell, 10100, 50);   // First
    engine.add_order(2, Side::Sell, 10100, 50);   // Second

    // Buy 50 - should match order 1 first
    engine.add_order(3, Side::Buy, 10100, 50);

    ASSERT_EQ(collector.trades.size(), 1);
    ASSERT_EQ(collector.trades[0].passive_order_id, 1);  // First order matched
    ASSERT_EQ(collector.trades[0].quantity, 50);

    // Order 2 should still be resting
    ASSERT_EQ(engine.ask_quantity_at(10100), 50);
}

// Test: Walk the book - multiple price levels
TEST(test_walk_the_book) {
    MatchingEngine engine(0, 100'000);
    TradeCollector collector;
    engine.set_trade_callback([&](const Trade& t) { collector.on_trade(t); });

    // Add asks at multiple prices
    engine.add_order(1, Side::Sell, 10100, 50);   // Best ask
    engine.add_order(2, Side::Sell, 10200, 50);   // Next level
    engine.add_order(3, Side::Sell, 10300, 50);   // Third level

    // Buy 120 at 10300 (should walk through levels)
    engine.add_order(4, Side::Buy, 10300, 120);

    // Should get 3 trades
    ASSERT_EQ(collector.trades.size(), 3);

    // First trade at best price (10100)
    ASSERT_EQ(collector.trades[0].price, 10100);
    ASSERT_EQ(collector.trades[0].quantity, 50);

    // Second trade at next price (10200)
    ASSERT_EQ(collector.trades[1].price, 10200);
    ASSERT_EQ(collector.trades[1].quantity, 50);

    // Third trade partial at 10300
    ASSERT_EQ(collector.trades[2].price, 10300);
    ASSERT_EQ(collector.trades[2].quantity, 20);  // Only needed 20 more

    // Remaining 30 at 10300 should stay
    ASSERT_EQ(engine.best_ask(), 10300);
    ASSERT_EQ(engine.ask_quantity_at(10300), 30);
}

// Test: Sell aggressor matches bids
TEST(test_sell_aggressor) {
    MatchingEngine engine(0, 100'000);
    TradeCollector collector;
    engine.set_trade_callback([&](const Trade& t) { collector.on_trade(t); });

    // Add bid at 10000
    engine.add_order(1, Side::Buy, 10000, 100);

    // Sell at 10000 (crosses)
    engine.add_order(2, Side::Sell, 10000, 60);

    ASSERT_EQ(collector.trades.size(), 1);
    ASSERT_EQ(collector.trades[0].aggressor_side, Side::Sell);
    ASSERT_EQ(collector.trades[0].quantity, 60);

    // Remaining 40 in bid
    ASSERT_EQ(engine.best_bid(), 10000);
    ASSERT_EQ(engine.bid_quantity_at(10000), 40);
}

// Test: Better price execution (price improvement)
TEST(test_price_improvement) {
    MatchingEngine engine(0, 100'000);
    TradeCollector collector;
    engine.set_trade_callback([&](const Trade& t) { collector.on_trade(t); });

    // Add sell at 10100
    engine.add_order(1, Side::Sell, 10100, 100);

    // Buy at 10200 (willing to pay more)
    engine.add_order(2, Side::Buy, 10200, 100);

    // Trade should execute at resting order's price (10100)
    ASSERT_EQ(collector.trades.size(), 1);
    ASSERT_EQ(collector.trades[0].price, 10100);  // Passive price
}

// Test: Cancel order
TEST(test_cancel_order) {
    MatchingEngine engine(0, 100'000);

    engine.add_order(1, Side::Buy, 10000, 100);
    ASSERT_EQ(engine.best_bid(), 10000);

    ASSERT_TRUE(engine.cancel_order(1));
    ASSERT_EQ(engine.best_bid(), INVALID_PRICE);
}

// Test: Self-trade prevention (same trader)
TEST(test_self_trade_prevention) {
    MatchingEngine engine(0, 100'000);
    TradeCollector collector;
    engine.set_trade_callback([&](const Trade& t) { collector.on_trade(t); });

    // Trader A adds sell
    engine.add_order(1, Side::Sell, 10100, 100, 1001);  // trader_id = 1001

    // Same trader tries to buy
    engine.add_order(2, Side::Buy, 10100, 100, 1001);   // trader_id = 1001

    // No trade - self-trade prevented
    ASSERT_EQ(collector.trades.size(), 0);

    // Both orders should be resting (or one cancelled based on policy)
    // For now, we'll use "cancel aggressive" policy
    ASSERT_EQ(engine.best_ask(), 10100);
    ASSERT_EQ(engine.best_bid(), INVALID_PRICE);  // Aggressive order cancelled
}

int main() {
    std::cout << "=== Matching Engine Tests ===\n";

    RUN_TEST(test_no_match_with_spread);
    RUN_TEST(test_full_match_buy_crosses);
    RUN_TEST(test_partial_match_aggressor_larger);
    RUN_TEST(test_partial_match_aggressor_smaller);
    RUN_TEST(test_price_time_priority);
    RUN_TEST(test_walk_the_book);
    RUN_TEST(test_sell_aggressor);
    RUN_TEST(test_price_improvement);
    RUN_TEST(test_cancel_order);
    RUN_TEST(test_self_trade_prevention);

    std::cout << "\nAll tests PASSED!\n";
    return 0;
}
