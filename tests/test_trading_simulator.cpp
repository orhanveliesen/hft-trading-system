#include <cassert>
#include <iostream>
#include <vector>
#include "../include/types.hpp"
#include "../include/trading_simulator.hpp"

using namespace hft;
using namespace hft::strategy;

#define TEST(name) void name()
#define RUN_TEST(name) do { \
    std::cout << "Running " << #name << "... "; \
    name(); \
    std::cout << "PASSED\n"; \
} while(0)

#define ASSERT_EQ(a, b) assert((a) == (b))
#define ASSERT_TRUE(x) assert(x)
#define ASSERT_FALSE(x) assert(!(x))
#define ASSERT_GT(a, b) assert((a) > (b))
#define ASSERT_LT(a, b) assert((a) < (b))

// Simulated market tick data
struct MarketTick {
    Price bid;
    Price ask;
    Quantity bid_size;
    Quantity ask_size;
};

// Test: Market maker places two-sided quotes
TEST(test_market_maker_places_quotes) {
    SimulatorConfig config;
    config.spread_bps = 20;    // 20 bps = 0.2%
    config.quote_size = 100;
    config.max_position = 500;

    TradingSimulator sim(config);

    // Market is at 10000 bid / 10010 ask (mid = 10005)
    MarketTick tick{10000, 10010, 1000, 1000};
    auto quotes = sim.on_market_data(tick.bid, tick.ask, tick.bid_size, tick.ask_size);

    // Should have two-sided quotes around mid
    ASSERT_TRUE(quotes.has_bid);
    ASSERT_TRUE(quotes.has_ask);

    // Bid should be below mid, ask above
    Price mid = (tick.bid + tick.ask) / 2;
    ASSERT_LT(quotes.bid_price, mid);
    ASSERT_GT(quotes.ask_price, mid);
}

// Test: Market maker fills update position
TEST(test_fills_update_position) {
    SimulatorConfig config;
    config.quote_size = 100;
    config.max_position = 500;

    TradingSimulator sim(config);

    // Initial position should be 0
    ASSERT_EQ(sim.position(), 0);

    // Simulate a buy fill
    sim.on_fill(Side::Buy, 50, 10000);
    ASSERT_EQ(sim.position(), 50);

    // Simulate a sell fill
    sim.on_fill(Side::Sell, 30, 10010);
    ASSERT_EQ(sim.position(), 20);
}

// Test: P&L tracking
TEST(test_pnl_tracking) {
    SimulatorConfig config;
    TradingSimulator sim(config);

    // Buy 100 at 10000
    sim.on_fill(Side::Buy, 100, 10000);
    ASSERT_EQ(sim.realized_pnl(), 0);

    // Sell 100 at 10050 (profit of 50 per share)
    sim.on_fill(Side::Sell, 100, 10050);
    ASSERT_EQ(sim.realized_pnl(), 100 * 50);  // 5000 profit

    // Should be flat now
    ASSERT_EQ(sim.position(), 0);
}

// Test: Risk manager halts trading on max loss
TEST(test_risk_halt_on_loss) {
    SimulatorConfig config;
    config.daily_loss_limit = 1000;  // Halt at -1000 daily loss

    TradingSimulator sim(config);

    // Buy 100 at 10000
    sim.on_fill(Side::Buy, 100, 10000);

    // Sell at huge loss: 10000 - 9000 = 1000 loss per share
    sim.on_fill(Side::Sell, 100, 8990);  // Loss = 100 * 1010 = 101000

    // Should be halted
    ASSERT_TRUE(sim.is_halted());

    // No more quotes
    auto quotes = sim.on_market_data(9000, 9010, 1000, 1000);
    ASSERT_FALSE(quotes.has_bid);
    ASSERT_FALSE(quotes.has_ask);
}

// Test: Position limit reduces quote size
TEST(test_position_limit_reduces_size) {
    SimulatorConfig config;
    config.quote_size = 100;
    config.max_position = 150;

    TradingSimulator sim(config);

    // Buy 100 - position is 100, room to buy is 50
    sim.on_fill(Side::Buy, 100, 10000);

    auto quotes = sim.on_market_data(10000, 10010, 1000, 1000);

    // Bid size should be reduced (only 50 more room)
    ASSERT_EQ(quotes.bid_size, 50);
    // Ask size should be full (can sell all 100 + 150 more = 250)
    ASSERT_EQ(quotes.ask_size, 100);  // Capped at quote_size
}

// Test: Inventory skew adjusts prices
TEST(test_inventory_skew) {
    SimulatorConfig config;
    config.quote_size = 100;
    config.max_position = 200;
    config.skew_factor = 1.0;  // Full skew

    TradingSimulator sim(config);

    // Get neutral quotes
    auto neutral = sim.on_market_data(10000, 10010, 1000, 1000);
    Price neutral_bid = neutral.bid_price;
    Price neutral_ask = neutral.ask_price;

    // Buy 100 - now long, should lower bids to discourage more buying
    sim.on_fill(Side::Buy, 100, 10000);

    auto skewed = sim.on_market_data(10000, 10010, 1000, 1000);

    // Bid should be lower than neutral (less willing to buy)
    ASSERT_LT(skewed.bid_price, neutral_bid);
}

// Test: Backtest with simulated ticks
TEST(test_backtest_simple) {
    SimulatorConfig config;
    config.spread_bps = 10;
    config.quote_size = 100;
    config.max_position = 500;

    TradingSimulator sim(config);

    // Simulate a simple up-down-up market
    std::vector<MarketTick> ticks = {
        {10000, 10010, 1000, 1000},
        {10005, 10015, 1000, 1000},  // Market moves up
        {10000, 10010, 1000, 1000},  // Back down
        {10010, 10020, 1000, 1000},  // Up again
        {10005, 10015, 1000, 1000},  // Settle
    };

    for (const auto& tick : ticks) {
        sim.on_market_data(tick.bid, tick.ask, tick.bid_size, tick.ask_size);
    }

    // After simulation, should have generated some quotes
    ASSERT_GT(sim.total_quotes_generated(), 0);
}

// Test: Full simulation with order execution
TEST(test_full_simulation_with_execution) {
    SimulatorConfig config;
    config.spread_bps = 50;  // Wide spread for easier fills
    config.quote_size = 10;
    config.max_position = 100;

    TradingSimulator sim(config);

    // First tick - place quotes
    auto quotes = sim.on_market_data(10000, 10100, 1000, 1000);
    Price our_bid = quotes.bid_price;
    Price our_ask = quotes.ask_price;

    // Market moves - our bid gets hit (someone sells to us)
    sim.on_fill(Side::Buy, quotes.bid_size, our_bid);
    ASSERT_EQ(sim.position(), static_cast<int64_t>(quotes.bid_size));

    // Market moves - our ask gets hit (someone buys from us)
    sim.on_fill(Side::Sell, quotes.ask_size, our_ask);

    // Should have profit from spread capture
    ASSERT_GT(sim.realized_pnl(), 0);
}

int main() {
    std::cout << "=== Trading Simulator Tests ===\n";

    RUN_TEST(test_market_maker_places_quotes);
    RUN_TEST(test_fills_update_position);
    RUN_TEST(test_pnl_tracking);
    RUN_TEST(test_risk_halt_on_loss);
    RUN_TEST(test_position_limit_reduces_size);
    RUN_TEST(test_inventory_skew);
    RUN_TEST(test_backtest_simple);
    RUN_TEST(test_full_simulation_with_execution);

    std::cout << "\nAll tests PASSED!\n";
    return 0;
}
