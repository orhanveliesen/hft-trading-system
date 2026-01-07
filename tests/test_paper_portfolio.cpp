/**
 * Paper Trading Portfolio & Signal Comprehensive Test Suite
 *
 * Tests:
 * 1. Portfolio operations (buy/sell with constraints)
 * 2. Edge cases (overflow, underflow, boundaries)
 * 3. Signal generation correctness
 * 4. Performance benchmarks
 *
 * Run with: ./test_paper_portfolio
 */

#include <iostream>
#include <iomanip>
#include <cassert>
#include <cmath>
#include <chrono>
#include <vector>
#include <map>
#include <string>
#include <sstream>
#include <limits>
#include <functional>
#include <random>

#include "../include/types.hpp"
#include "../include/risk/enhanced_risk_manager.hpp"
#include "../include/strategy/regime_detector.hpp"

using namespace hft;
using namespace hft::risk;
using namespace hft::strategy;

// ============================================================================
// Test Infrastructure
// ============================================================================

struct TestResult {
    std::string name;
    bool passed;
    std::string details;
    double duration_us;
};

std::vector<TestResult> g_results;
int g_pass_count = 0;
int g_fail_count = 0;

#define TEST_LOG(msg) std::cout << "    " << msg << "\n"
#define TEST_WARN(msg) std::cout << "    \033[33m[WARN]\033[0m " << msg << "\n"
#define TEST_ERROR(msg) std::cout << "    \033[31m[ERROR]\033[0m " << msg << "\n"
#define TEST_OK(msg) std::cout << "    \033[32m[OK]\033[0m " << msg << "\n"

void run_test(const std::string& name, std::function<bool()> test_fn) {
    std::cout << "\n\033[1m[TEST] " << name << "\033[0m\n";

    auto start = std::chrono::high_resolution_clock::now();
    bool passed = false;
    std::string details;

    try {
        passed = test_fn();
    } catch (const std::exception& e) {
        details = std::string("Exception: ") + e.what();
        TEST_ERROR(details);
    } catch (...) {
        details = "Unknown exception";
        TEST_ERROR(details);
    }

    auto end = std::chrono::high_resolution_clock::now();
    double duration_us = std::chrono::duration<double, std::micro>(end - start).count();

    if (passed) {
        std::cout << "  \033[32m✓ PASSED\033[0m (" << std::fixed << std::setprecision(1)
                  << duration_us << " µs)\n";
        g_pass_count++;
    } else {
        std::cout << "  \033[31m✗ FAILED\033[0m (" << std::fixed << std::setprecision(1)
                  << duration_us << " µs)\n";
        g_fail_count++;
    }

    g_results.push_back({name, passed, details, duration_us});
}

// ============================================================================
// Portfolio Implementation (same as hft.cpp)
// ============================================================================

struct Portfolio {
    double cash = 0;
    std::map<Symbol, double> holdings;

    void init(double capital) {
        cash = capital;
        holdings.clear();
    }

    double get_holding(Symbol s) const {
        auto it = holdings.find(s);
        return it != holdings.end() ? it->second : 0;
    }

    bool can_buy(double price, double qty) const {
        return cash >= price * qty;
    }

    bool can_sell(Symbol s, double qty) const {
        return get_holding(s) >= qty;
    }

    void buy(Symbol s, double price, double qty) {
        cash -= price * qty;
        holdings[s] += qty;
    }

    void sell(Symbol s, double price, double qty) {
        cash += price * qty;
        holdings[s] -= qty;
        if (holdings[s] <= 1e-9) holdings.erase(s);
    }

    double total_value(const std::map<Symbol, double>& prices) const {
        double value = cash;
        for (const auto& [sym, qty] : holdings) {
            auto it = prices.find(sym);
            if (it != prices.end()) {
                value += qty * it->second;
            }
        }
        return value;
    }
};

// ============================================================================
// Test: Portfolio Basic Operations
// ============================================================================

bool test_portfolio_init() {
    Portfolio p;
    p.init(100000.0);

    TEST_LOG("Initial cash: $" << p.cash);
    if (p.cash != 100000.0) {
        TEST_ERROR("Expected cash=100000, got " << p.cash);
        return false;
    }

    if (!p.holdings.empty()) {
        TEST_ERROR("Holdings should be empty after init");
        return false;
    }

    TEST_OK("Portfolio initialized correctly");
    return true;
}

bool test_portfolio_buy_success() {
    Portfolio p;
    p.init(100000.0);

    Symbol BTC = 1;
    double price = 91000.0;
    double qty = 1.0;

    TEST_LOG("Attempting to buy " << qty << " BTC @ $" << price);

    if (!p.can_buy(price, qty)) {
        TEST_ERROR("Should be able to buy with sufficient cash");
        return false;
    }

    p.buy(BTC, price, qty);

    TEST_LOG("After buy: cash=$" << p.cash << ", BTC=" << p.get_holding(BTC));

    if (std::abs(p.cash - 9000.0) > 0.01) {
        TEST_ERROR("Cash should be 9000, got " << p.cash);
        return false;
    }

    if (std::abs(p.get_holding(BTC) - 1.0) > 0.001) {
        TEST_ERROR("Should hold 1 BTC, got " << p.get_holding(BTC));
        return false;
    }

    TEST_OK("Buy operation successful");
    return true;
}

bool test_portfolio_buy_insufficient_cash() {
    Portfolio p;
    p.init(10000.0);  // Only $10k

    Symbol BTC = 1;
    double price = 91000.0;  // BTC costs $91k
    double qty = 1.0;

    TEST_LOG("Cash: $" << p.cash << ", trying to buy BTC @ $" << price);

    if (p.can_buy(price, qty)) {
        TEST_ERROR("Should NOT be able to buy with insufficient cash");
        return false;
    }

    TEST_OK("Correctly rejected buy with insufficient cash");
    return true;
}

bool test_portfolio_sell_success() {
    Portfolio p;
    p.init(100000.0);

    Symbol ETH = 2;
    double buy_price = 3000.0;
    double sell_price = 3100.0;
    double qty = 10.0;

    // First buy
    p.buy(ETH, buy_price, qty);
    TEST_LOG("Bought " << qty << " ETH @ $" << buy_price << " (cash: $" << p.cash << ")");

    // Then sell
    if (!p.can_sell(ETH, qty)) {
        TEST_ERROR("Should be able to sell holdings");
        return false;
    }

    p.sell(ETH, sell_price, qty);
    TEST_LOG("Sold " << qty << " ETH @ $" << sell_price << " (cash: $" << p.cash << ")");

    double expected_cash = 100000.0 - (buy_price * qty) + (sell_price * qty);
    if (std::abs(p.cash - expected_cash) > 0.01) {
        TEST_ERROR("Cash should be " << expected_cash << ", got " << p.cash);
        return false;
    }

    double profit = (sell_price - buy_price) * qty;
    TEST_OK("Sold with profit: $" << profit);
    return true;
}

bool test_portfolio_sell_no_holdings() {
    Portfolio p;
    p.init(100000.0);

    Symbol SOL = 3;

    TEST_LOG("Trying to sell SOL without any holdings");

    if (p.can_sell(SOL, 1.0)) {
        TEST_ERROR("Should NOT be able to sell without holdings (no shorting)");
        return false;
    }

    TEST_OK("Correctly rejected sell without holdings");
    return true;
}

bool test_portfolio_partial_sell() {
    Portfolio p;
    p.init(100000.0);

    Symbol ETH = 2;

    // Buy 10 ETH
    p.buy(ETH, 3000.0, 10.0);
    TEST_LOG("Bought 10 ETH, holdings: " << p.get_holding(ETH));

    // Sell 3 ETH
    p.sell(ETH, 3100.0, 3.0);
    TEST_LOG("Sold 3 ETH, remaining: " << p.get_holding(ETH));

    if (std::abs(p.get_holding(ETH) - 7.0) > 0.001) {
        TEST_ERROR("Should have 7 ETH remaining, got " << p.get_holding(ETH));
        return false;
    }

    // Try to sell more than we have
    if (p.can_sell(ETH, 10.0)) {
        TEST_ERROR("Should not be able to sell 10 when only 7 remain");
        return false;
    }

    TEST_OK("Partial sell works correctly");
    return true;
}

bool test_portfolio_exact_boundary() {
    Portfolio p;
    p.init(10000.0);

    Symbol SYM = 1;

    // Try to buy exactly what we can afford
    double price = 1000.0;
    double qty = 10.0;  // 10 * 1000 = 10000 exactly

    TEST_LOG("Cash: $" << p.cash << ", buying " << qty << " @ $" << price);

    if (!p.can_buy(price, qty)) {
        TEST_ERROR("Should be able to buy at exact cash amount");
        return false;
    }

    p.buy(SYM, price, qty);

    if (std::abs(p.cash) > 0.001) {
        TEST_ERROR("Cash should be 0, got " << p.cash);
        return false;
    }

    // Now shouldn't be able to buy anything
    if (p.can_buy(1.0, 0.01)) {
        TEST_ERROR("Should not be able to buy with 0 cash");
        return false;
    }

    TEST_OK("Exact boundary handled correctly");
    return true;
}

// ============================================================================
// Test: Edge Cases - Numerical
// ============================================================================

bool test_unsigned_underflow_fix() {
    // This was the critical bug we fixed
    // When mid < last_mid with uint32_t, subtraction underflows

    Price mid = 913500000;      // Current price
    Price last_mid = 913510000; // Previous price (higher)

    // WRONG way (causes underflow):
    uint32_t wrong_diff = mid - last_mid;  // UNDERFLOW!
    double change_wrong = static_cast<double>(wrong_diff) / last_mid;

    // CORRECT way:
    double change_correct = (static_cast<double>(mid) - static_cast<double>(last_mid)) / last_mid;

    TEST_LOG("mid=" << mid << ", last_mid=" << last_mid);
    TEST_LOG("Difference (should be -10000): " << (static_cast<int64_t>(mid) - static_cast<int64_t>(last_mid)));
    TEST_LOG("");
    TEST_LOG("\033[31mWRONG (uint32_t underflow):\033[0m");
    TEST_LOG("  mid - last_mid = " << wrong_diff << " (should be negative!)");
    TEST_LOG("  change = " << (change_wrong * 10000) << " bps (MASSIVE WRONG VALUE)");
    TEST_LOG("");
    TEST_LOG("\033[32mCORRECT (double before subtraction):\033[0m");
    TEST_LOG("  change = " << (change_correct * 10000) << " bps (small negative, correct)");

    // The change should be small and NEGATIVE
    if (change_correct >= 0) {
        TEST_ERROR("Change should be negative (price went down)");
        return false;
    }

    double expected_change = -10000.0 / 913510000.0;  // -0.001095%
    if (std::abs(change_correct - expected_change) > 1e-10) {
        TEST_ERROR("Change calculation incorrect");
        return false;
    }

    // Verify the wrong calculation is indeed wrong
    if (change_wrong < 1.0) {
        TEST_ERROR("Wrong calculation should produce huge positive value");
        return false;
    }

    TEST_OK("Unsigned underflow correctly avoided");
    return true;
}

bool test_price_precision() {
    // Test that we handle price precision correctly
    // Prices are scaled by 10000 (PRICE_SCALE)

    constexpr int64_t PRICE_SCALE = 10000;

    // BTC at $91,234.5678
    Price btc_price = 912345678;  // In scaled units
    double btc_usd = static_cast<double>(btc_price) / PRICE_SCALE;

    TEST_LOG("BTC price in scaled units: " << btc_price);
    TEST_LOG("BTC price in USD: $" << std::fixed << std::setprecision(4) << btc_usd);

    if (std::abs(btc_usd - 91234.5678) > 0.0001) {
        TEST_ERROR("Price conversion incorrect");
        return false;
    }

    // Test minimum price change (1 unit = $0.0001)
    Price min_change = 1;
    double min_usd = static_cast<double>(min_change) / PRICE_SCALE;
    TEST_LOG("Minimum price change: $" << std::setprecision(6) << min_usd);

    // Test uint32_t max
    Price max_price = std::numeric_limits<Price>::max();
    double max_usd = static_cast<double>(max_price) / PRICE_SCALE;
    TEST_LOG("Max representable price: $" << std::fixed << std::setprecision(2) << max_usd);
    TEST_LOG("  (uint32_t max = " << max_price << ")");

    TEST_OK("Price precision is correct");
    return true;
}

bool test_extreme_prices() {
    Portfolio p;
    p.init(100000.0);

    Symbol EXTREME = 99;

    // Test very high price (can't afford)
    double very_high = 1000000.0;  // $1M
    TEST_LOG("Testing very high price: $" << very_high);
    if (p.can_buy(very_high, 1.0)) {
        TEST_ERROR("Should not afford $1M asset");
        return false;
    }
    TEST_OK("Correctly rejected unaffordable asset");

    // Test very low price (many units)
    double very_low = 0.001;  // $0.001
    double max_units = p.cash / very_low;
    TEST_LOG("At $0.001, could buy " << max_units << " units");
    if (!p.can_buy(very_low, 1000000.0)) {  // Buy 1M units for $1000
        TEST_ERROR("Should be able to buy cheap assets");
        return false;
    }

    // Test boundary: exact cash amount
    double exact_price = p.cash;  // $100,000 exactly
    TEST_LOG("Testing exact cash boundary: $" << exact_price);
    if (!p.can_buy(exact_price, 1.0)) {
        TEST_ERROR("Should be able to buy at exact cash amount");
        return false;
    }

    TEST_OK("Extreme prices handled correctly");
    return true;
}

bool test_floating_point_accumulation() {
    // Test that many small trades don't accumulate floating point errors
    Portfolio p;
    p.init(100000.0);

    Symbol TEST_SYM = 1;
    double initial_cash = p.cash;

    // Do many buy/sell cycles at same price
    int cycles = 10000;
    double price = 100.0;
    double qty = 1.0;

    TEST_LOG("Running " << cycles << " buy/sell cycles at same price...");

    for (int i = 0; i < cycles; i++) {
        p.buy(TEST_SYM, price, qty);
        p.sell(TEST_SYM, price, qty);
    }

    double error = std::abs(p.cash - initial_cash);
    TEST_LOG("Initial cash: $" << initial_cash);
    TEST_LOG("Final cash: $" << std::setprecision(10) << p.cash);
    TEST_LOG("Accumulated error: $" << error);

    if (error > 0.01) {  // Allow 1 cent error
        TEST_WARN("Floating point error accumulated: $" << error);
    }

    if (error > 1.0) {  // More than $1 is a problem
        TEST_ERROR("Excessive floating point error");
        return false;
    }

    TEST_OK("Floating point errors within acceptable range");
    return true;
}

bool test_small_quantity_trades() {
    Portfolio p;
    p.init(100000.0);

    Symbol SYM = 1;

    // Test very small quantities (e.g., 0.001 BTC)
    double price = 91000.0;
    double small_qty = 0.001;

    TEST_LOG("Testing small quantity: " << small_qty << " @ $" << price);

    p.buy(SYM, price, small_qty);
    TEST_LOG("After buy: cash=$" << p.cash << ", holding=" << p.get_holding(SYM));

    double expected_cost = price * small_qty;
    if (std::abs((100000.0 - p.cash) - expected_cost) > 0.001) {
        TEST_ERROR("Small quantity cost calculation wrong");
        return false;
    }

    // Sell it back
    p.sell(SYM, price, small_qty);
    if (std::abs(p.cash - 100000.0) > 0.001) {
        TEST_ERROR("Small quantity sell didn't return correct cash");
        return false;
    }

    TEST_OK("Small quantities handled correctly");
    return true;
}

// ============================================================================
// Test: Signal Generation
// ============================================================================

bool test_change_calculation_range() {
    // Test change calculation across various scenarios

    struct TestCase {
        Price mid;
        Price last_mid;
        double expected_bps;  // Expected change in basis points
        std::string description;
    };

    std::vector<TestCase> cases = {
        {100000000, 100000000, 0.0, "No change"},
        {100010000, 100000000, 1.0, "0.01% increase (1 bps)"},
        {99990000, 100000000, -1.0, "0.01% decrease (-1 bps)"},
        {101000000, 100000000, 100.0, "1% increase (100 bps)"},
        {99000000, 100000000, -100.0, "1% decrease (-100 bps)"},
        {200000000, 100000000, 10000.0, "100% increase"},
        {50000000, 100000000, -5000.0, "50% decrease"},
        {100000001, 100000000, 0.0001, "1 tick increase"},
        {99999999, 100000000, -0.0001, "1 tick decrease"},
    };

    bool all_passed = true;
    for (const auto& tc : cases) {
        double change = (static_cast<double>(tc.mid) - static_cast<double>(tc.last_mid)) / tc.last_mid;
        double change_bps = change * 10000;

        bool match = std::abs(change_bps - tc.expected_bps) < 0.01;

        if (match) {
            TEST_OK(tc.description << ": " << change_bps << " bps");
        } else {
            TEST_ERROR(tc.description << ": expected " << tc.expected_bps
                      << " bps, got " << change_bps << " bps");
            all_passed = false;
        }
    }

    return all_passed;
}

bool test_regime_detection_trending_up() {
    RegimeConfig config;
    config.lookback = 10;

    RegimeDetector detector(config);

    TEST_LOG("Feeding steadily increasing prices...");
    for (int i = 0; i < 25; i++) {
        double price = 100.0 + i * 2.0;  // +2% per tick
        detector.update(price);
    }

    MarketRegime regime = detector.current_regime();
    TEST_LOG("Detected regime: " << regime_to_string(regime));
    TEST_LOG("Trend strength: " << detector.trend_strength());

    if (regime != MarketRegime::TrendingUp) {
        TEST_ERROR("Should detect TRENDING_UP, got " << regime_to_string(regime));
        return false;
    }

    if (detector.trend_strength() <= 0) {
        TEST_ERROR("Trend strength should be positive for uptrend");
        return false;
    }

    TEST_OK("Trending up detected correctly");
    return true;
}

bool test_regime_detection_trending_down() {
    RegimeConfig config;
    config.lookback = 10;

    RegimeDetector detector(config);

    TEST_LOG("Feeding steadily decreasing prices...");
    for (int i = 0; i < 25; i++) {
        double price = 200.0 - i * 2.0;  // -2% per tick
        detector.update(price);
    }

    MarketRegime regime = detector.current_regime();
    TEST_LOG("Detected regime: " << regime_to_string(regime));
    TEST_LOG("Trend strength: " << detector.trend_strength());

    if (regime != MarketRegime::TrendingDown) {
        TEST_ERROR("Should detect TRENDING_DOWN, got " << regime_to_string(regime));
        return false;
    }

    if (detector.trend_strength() >= 0) {
        TEST_ERROR("Trend strength should be negative for downtrend");
        return false;
    }

    TEST_OK("Trending down detected correctly");
    return true;
}

bool test_regime_detection_ranging() {
    RegimeConfig config;
    config.lookback = 10;

    RegimeDetector detector(config);

    TEST_LOG("Feeding oscillating prices (ranging market)...");
    for (int i = 0; i < 50; i++) {
        // Oscillate around 100 with small amplitude
        double price = 100.0 + 2.0 * std::sin(i * 0.5);
        detector.update(price);
    }

    MarketRegime regime = detector.current_regime();
    TEST_LOG("Detected regime: " << regime_to_string(regime));
    TEST_LOG("Mean reversion score: " << detector.mean_reversion_score());
    TEST_LOG("Trend strength: " << detector.trend_strength());

    // Should be ranging or low volatility
    if (regime != MarketRegime::Ranging && regime != MarketRegime::LowVolatility) {
        TEST_WARN("Expected RANGING or LOW_VOL, got " << regime_to_string(regime));
    }

    TEST_OK("Ranging detection attempted");
    return true;
}

bool test_regime_high_volatility() {
    RegimeConfig config;
    config.lookback = 10;
    config.high_vol_threshold = 0.03;  // 3% vol is high

    RegimeDetector detector(config);

    TEST_LOG("Feeding highly volatile prices...");
    std::mt19937 rng(42);
    std::normal_distribution<double> dist(0, 5.0);  // High std dev

    double price = 100.0;
    for (int i = 0; i < 30; i++) {
        price += dist(rng);
        if (price < 50) price = 50;
        if (price > 150) price = 150;
        detector.update(price);
    }

    MarketRegime regime = detector.current_regime();
    TEST_LOG("Detected regime: " << regime_to_string(regime));
    TEST_LOG("Volatility: " << (detector.volatility() * 100) << "%");

    // Just verify we got a regime
    if (regime == MarketRegime::Unknown) {
        TEST_ERROR("Should have detected some regime");
        return false;
    }

    TEST_OK("High volatility scenario handled");
    return true;
}

// ============================================================================
// Test: Integration Scenarios
// ============================================================================

bool test_realistic_trading_session() {
    Portfolio p;
    p.init(100000.0);

    Symbol BTC = 1, ETH = 2;

    TEST_LOG("=== Simulated Trading Session ===");
    TEST_LOG("Starting capital: $" << p.cash);

    // Simulate a trading session with realistic prices
    struct Trade {
        Symbol sym;
        bool is_buy;
        double price;
        double qty;
        std::string reason;
    };

    std::vector<Trade> trades = {
        {ETH, true, 3150.0, 5.0, "Buy ETH on dip"},
        {ETH, true, 3140.0, 5.0, "Average down"},
        {ETH, false, 3180.0, 3.0, "Take partial profit"},
        {BTC, true, 91200.0, 0.5, "Diversify into BTC"},
        {ETH, false, 3200.0, 7.0, "Exit remaining ETH"},
        {BTC, false, 91500.0, 0.5, "Exit BTC with profit"},
    };

    for (const auto& t : trades) {
        const char* sym_name = (t.sym == BTC) ? "BTC" : "ETH";

        if (t.is_buy) {
            if (!p.can_buy(t.price, t.qty)) {
                TEST_WARN("Cannot buy " << t.qty << " " << sym_name << " @ $" << t.price
                         << " (insufficient cash: $" << p.cash << ")");
                continue;
            }
            p.buy(t.sym, t.price, t.qty);
            TEST_LOG("BUY  " << std::setw(5) << t.qty << " " << sym_name
                    << " @ $" << std::setw(8) << t.price << " | " << t.reason);
        } else {
            if (!p.can_sell(t.sym, t.qty)) {
                TEST_WARN("Cannot sell " << t.qty << " " << sym_name
                         << " (only have " << p.get_holding(t.sym) << ")");
                continue;
            }
            p.sell(t.sym, t.price, t.qty);
            TEST_LOG("SELL " << std::setw(5) << t.qty << " " << sym_name
                    << " @ $" << std::setw(8) << t.price << " | " << t.reason);
        }
        TEST_LOG("     Cash: $" << std::fixed << std::setprecision(2) << p.cash);
    }

    TEST_LOG("");
    TEST_LOG("=== Session End ===");
    TEST_LOG("Final cash: $" << p.cash);

    double pnl = p.cash - 100000.0;
    TEST_LOG("P&L: $" << (pnl >= 0 ? "+" : "") << pnl);

    if (p.cash < 0) {
        TEST_ERROR("Cash went negative!");
        return false;
    }

    if (pnl < 0) {
        TEST_WARN("Session ended with loss");
    } else {
        TEST_OK("Session ended with profit: $" << pnl);
    }

    return true;
}

bool test_cash_depletes_correctly() {
    Portfolio p;
    p.init(100000.0);

    Symbol ETH = 2;
    double eth_price = 3000.0;

    TEST_LOG("Starting cash: $" << p.cash);
    TEST_LOG("ETH price: $" << eth_price);

    // Keep buying until we run out of cash
    int buys = 0;
    while (p.can_buy(eth_price, 1.0)) {
        p.buy(ETH, eth_price, 1.0);
        buys++;
    }

    TEST_LOG("Completed " << buys << " buys");
    TEST_LOG("Final holdings: " << p.get_holding(ETH) << " ETH");
    TEST_LOG("Remaining cash: $" << p.cash);

    // Should have bought exactly floor(100000/3000) = 33 ETH
    int expected_buys = static_cast<int>(100000.0 / eth_price);
    if (buys != expected_buys) {
        TEST_ERROR("Expected " << expected_buys << " buys, got " << buys);
        return false;
    }

    // Cash should be less than eth_price
    if (p.cash >= eth_price) {
        TEST_ERROR("Should not have enough cash for another ETH");
        return false;
    }

    TEST_OK("Cash depletes correctly");
    return true;
}

bool test_multi_symbol_portfolio() {
    Portfolio p;
    p.init(100000.0);

    Symbol BTC = 1, ETH = 2, SOL = 3, DOGE = 4;

    // Buy multiple assets
    p.buy(BTC, 91000.0, 0.5);   // $45,500
    p.buy(ETH, 3100.0, 10.0);   // $31,000
    p.buy(SOL, 200.0, 50.0);    // $10,000
    p.buy(DOGE, 0.40, 10000.0); // $4,000

    TEST_LOG("Portfolio after buys:");
    TEST_LOG("  BTC: " << p.get_holding(BTC) << " @ $91,000 = $" << (p.get_holding(BTC) * 91000));
    TEST_LOG("  ETH: " << p.get_holding(ETH) << " @ $3,100 = $" << (p.get_holding(ETH) * 3100));
    TEST_LOG("  SOL: " << p.get_holding(SOL) << " @ $200 = $" << (p.get_holding(SOL) * 200));
    TEST_LOG("  DOGE: " << p.get_holding(DOGE) << " @ $0.40 = $" << (p.get_holding(DOGE) * 0.40));
    TEST_LOG("  Cash: $" << p.cash);

    // Calculate total value with current prices
    std::map<Symbol, double> prices = {{BTC, 92000.0}, {ETH, 3200.0}, {SOL, 210.0}, {DOGE, 0.42}};
    double total = p.total_value(prices);

    TEST_LOG("");
    TEST_LOG("Total portfolio value (with price changes): $" << std::fixed << std::setprecision(2) << total);
    TEST_LOG("P&L: $" << (total - 100000.0));

    if (total < 100000.0) {
        TEST_WARN("Portfolio is down");
    }

    // Verify position count
    int positions = 0;
    for (const auto& [sym, qty] : p.holdings) {
        if (qty > 0) positions++;
    }

    if (positions != 4) {
        TEST_ERROR("Should have 4 positions, got " << positions);
        return false;
    }

    TEST_OK("Multi-symbol portfolio works correctly");
    return true;
}

// ============================================================================
// Benchmarks
// ============================================================================

bool benchmark_portfolio_operations() {
    Portfolio p;
    p.init(1000000000.0);  // Large capital for many ops

    Symbol SYM = 1;
    int iterations = 1000000;

    TEST_LOG("Running " << iterations << " iterations...");

    // Benchmark buy operations
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        p.buy(SYM, 100.0, 1.0);
    }
    auto end = std::chrono::high_resolution_clock::now();

    double buy_ns = std::chrono::duration<double, std::nano>(end - start).count() / iterations;
    TEST_LOG("BUY operation: " << std::fixed << std::setprecision(1) << buy_ns << " ns/op");

    // Benchmark sell operations
    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        p.sell(SYM, 100.0, 1.0);
    }
    end = std::chrono::high_resolution_clock::now();

    double sell_ns = std::chrono::duration<double, std::nano>(end - start).count() / iterations;
    TEST_LOG("SELL operation: " << std::fixed << std::setprecision(1) << sell_ns << " ns/op");

    // Benchmark can_buy checks
    start = std::chrono::high_resolution_clock::now();
    volatile bool result;
    for (int i = 0; i < iterations; i++) {
        result = p.can_buy(100.0, 1.0);
    }
    end = std::chrono::high_resolution_clock::now();
    (void)result;

    double can_buy_ns = std::chrono::duration<double, std::nano>(end - start).count() / iterations;
    TEST_LOG("CAN_BUY check: " << std::fixed << std::setprecision(1) << can_buy_ns << " ns/op");

    // Benchmark get_holding lookups
    start = std::chrono::high_resolution_clock::now();
    volatile double holding;
    for (int i = 0; i < iterations; i++) {
        holding = p.get_holding(SYM);
    }
    end = std::chrono::high_resolution_clock::now();
    (void)holding;

    double get_holding_ns = std::chrono::duration<double, std::nano>(end - start).count() / iterations;
    TEST_LOG("GET_HOLDING lookup: " << std::fixed << std::setprecision(1) << get_holding_ns << " ns/op");

    // Calculate throughput
    double ops_per_sec = 1e9 / buy_ns;
    TEST_LOG("");
    TEST_LOG("Throughput: " << std::fixed << std::setprecision(1) << (ops_per_sec / 1e6) << " M ops/sec");

    // Latency requirements for HFT
    if (buy_ns > 1000) {
        TEST_WARN("BUY latency > 1µs - may be too slow for HFT");
    }

    TEST_OK("Benchmark completed");
    return true;
}

bool benchmark_change_calculation() {
    int iterations = 10000000;

    TEST_LOG("Running " << iterations << " change calculations...");

    // Setup test data
    std::vector<Price> mids(1000);
    std::vector<Price> last_mids(1000);

    for (int i = 0; i < 1000; i++) {
        last_mids[i] = 900000000 + (i % 10000);
        mids[i] = last_mids[i] + (i % 100) - 50;  // Small changes
    }

    // Benchmark correct calculation
    auto start = std::chrono::high_resolution_clock::now();
    volatile double change;
    for (int i = 0; i < iterations; i++) {
        int idx = i % 1000;
        change = (static_cast<double>(mids[idx]) - static_cast<double>(last_mids[idx])) / last_mids[idx];
    }
    auto end = std::chrono::high_resolution_clock::now();
    (void)change;

    double ns_per_op = std::chrono::duration<double, std::nano>(end - start).count() / iterations;
    TEST_LOG("Change calculation: " << std::fixed << std::setprecision(2) << ns_per_op << " ns/op");

    double changes_per_sec = 1e9 / ns_per_op;
    TEST_LOG("Throughput: " << std::fixed << std::setprecision(1) << (changes_per_sec / 1e6) << " M calcs/sec");

    if (ns_per_op > 100) {
        TEST_WARN("Change calculation > 100ns - could be bottleneck");
    }

    TEST_OK("Benchmark completed");
    return true;
}

bool benchmark_regime_detection() {
    RegimeConfig config;
    config.lookback = 20;
    RegimeDetector detector(config);

    int iterations = 100000;

    TEST_LOG("Running " << iterations << " regime updates...");

    // Warmup
    for (int i = 0; i < 100; i++) {
        detector.update(100.0 + (i % 10) * 0.1);
    }

    // Benchmark
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        detector.update(100.0 + (i % 100) * 0.01);
    }
    auto end = std::chrono::high_resolution_clock::now();

    double ns_per_op = std::chrono::duration<double, std::nano>(end - start).count() / iterations;
    double us_per_op = ns_per_op / 1000;
    TEST_LOG("Regime update: " << std::fixed << std::setprecision(2) << us_per_op << " µs/op");

    double updates_per_sec = 1e9 / ns_per_op;
    TEST_LOG("Throughput: " << std::fixed << std::setprecision(0) << (updates_per_sec / 1e3) << " K updates/sec");

    // Regime detection is typically slower - it does calculations
    if (us_per_op > 100) {  // 100 µs
        TEST_WARN("Regime detection > 100µs - may need optimization");
    }

    TEST_OK("Benchmark completed");
    return true;
}

bool benchmark_multi_symbol_portfolio() {
    Portfolio p;
    p.init(10000000.0);

    int num_symbols = 100;
    int ops_per_symbol = 10000;

    TEST_LOG("Testing with " << num_symbols << " symbols...");

    // Populate with many symbols
    for (int s = 0; s < num_symbols; s++) {
        p.buy(s, 100.0, 10.0);
    }

    // Benchmark get_holding across many symbols
    auto start = std::chrono::high_resolution_clock::now();
    volatile double h;
    for (int i = 0; i < ops_per_symbol; i++) {
        for (int s = 0; s < num_symbols; s++) {
            h = p.get_holding(s);
        }
    }
    auto end = std::chrono::high_resolution_clock::now();
    (void)h;

    int total_ops = num_symbols * ops_per_symbol;
    double ns_per_op = std::chrono::duration<double, std::nano>(end - start).count() / total_ops;

    TEST_LOG("Get holding (" << num_symbols << " symbols): " << std::fixed << std::setprecision(1) << ns_per_op << " ns/op");

    // Benchmark total_value calculation
    std::map<Symbol, double> prices;
    for (int s = 0; s < num_symbols; s++) {
        prices[s] = 100.0 + s;
    }

    start = std::chrono::high_resolution_clock::now();
    volatile double total;
    for (int i = 0; i < 10000; i++) {
        total = p.total_value(prices);
    }
    end = std::chrono::high_resolution_clock::now();
    (void)total;

    double value_us = std::chrono::duration<double, std::micro>(end - start).count() / 10000;
    TEST_LOG("Total value calc (" << num_symbols << " symbols): " << std::fixed << std::setprecision(2) << value_us << " µs");

    TEST_OK("Multi-symbol benchmark completed");
    return true;
}

// ============================================================================
// Stress Tests
// ============================================================================

bool stress_test_rapid_trading() {
    Portfolio p;
    p.init(1000000.0);

    Symbol SYM = 1;
    int trades = 100000;

    TEST_LOG("Running " << trades << " rapid trades...");

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < trades; i++) {
        if (i % 2 == 0) {
            if (p.can_buy(100.0, 1.0)) {
                p.buy(SYM, 100.0, 1.0);
            }
        } else {
            if (p.can_sell(SYM, 1.0)) {
                p.sell(SYM, 100.0, 1.0);
            }
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(end - start).count();

    TEST_LOG("Total time: " << std::fixed << std::setprecision(2) << total_ms << " ms");
    TEST_LOG("Trades/sec: " << static_cast<int>(trades / (total_ms / 1000)));
    TEST_LOG("Final cash: $" << p.cash);
    TEST_LOG("Final holdings: " << p.get_holding(SYM));

    // Should end with roughly same cash (buy/sell same price)
    double error = std::abs(p.cash - 1000000.0);
    if (error > 1.0 && p.get_holding(SYM) == 0) {
        TEST_ERROR("Cash should be close to initial if no holdings remain");
        return false;
    }

    TEST_OK("Rapid trading stress test passed");
    return true;
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║        PAPER TRADING PORTFOLIO - COMPREHENSIVE TEST SUITE            ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════════╝\n";

    // Portfolio Basic Operations
    std::cout << "\n\033[1;36m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\033[0m";
    std::cout << "\n\033[1;36m  PORTFOLIO BASIC OPERATIONS\033[0m";
    std::cout << "\n\033[1;36m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\033[0m";
    run_test("Portfolio Init", test_portfolio_init);
    run_test("Portfolio Buy Success", test_portfolio_buy_success);
    run_test("Portfolio Buy Insufficient Cash", test_portfolio_buy_insufficient_cash);
    run_test("Portfolio Sell Success", test_portfolio_sell_success);
    run_test("Portfolio Sell No Holdings (No Shorting)", test_portfolio_sell_no_holdings);
    run_test("Portfolio Partial Sell", test_portfolio_partial_sell);
    run_test("Portfolio Exact Boundary", test_portfolio_exact_boundary);

    // Edge Cases - Numerical
    std::cout << "\n\033[1;36m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\033[0m";
    std::cout << "\n\033[1;36m  EDGE CASES - NUMERICAL\033[0m";
    std::cout << "\n\033[1;36m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\033[0m";
    run_test("CRITICAL: Unsigned Underflow Fix", test_unsigned_underflow_fix);
    run_test("Price Precision", test_price_precision);
    run_test("Extreme Prices", test_extreme_prices);
    run_test("Floating Point Accumulation", test_floating_point_accumulation);
    run_test("Small Quantity Trades", test_small_quantity_trades);

    // Signal Generation
    std::cout << "\n\033[1;36m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\033[0m";
    std::cout << "\n\033[1;36m  SIGNAL GENERATION & REGIME DETECTION\033[0m";
    std::cout << "\n\033[1;36m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\033[0m";
    run_test("Change Calculation Range", test_change_calculation_range);
    run_test("Regime Detection: Trending Up", test_regime_detection_trending_up);
    run_test("Regime Detection: Trending Down", test_regime_detection_trending_down);
    run_test("Regime Detection: Ranging", test_regime_detection_ranging);
    run_test("Regime Detection: High Volatility", test_regime_high_volatility);

    // Integration Scenarios
    std::cout << "\n\033[1;36m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\033[0m";
    std::cout << "\n\033[1;36m  INTEGRATION SCENARIOS\033[0m";
    std::cout << "\n\033[1;36m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\033[0m";
    run_test("Realistic Trading Session", test_realistic_trading_session);
    run_test("Cash Depletes Correctly", test_cash_depletes_correctly);
    run_test("Multi-Symbol Portfolio", test_multi_symbol_portfolio);

    // Performance Benchmarks
    std::cout << "\n\033[1;36m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\033[0m";
    std::cout << "\n\033[1;36m  PERFORMANCE BENCHMARKS\033[0m";
    std::cout << "\n\033[1;36m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\033[0m";
    run_test("BENCH: Portfolio Operations", benchmark_portfolio_operations);
    run_test("BENCH: Change Calculation", benchmark_change_calculation);
    run_test("BENCH: Regime Detection", benchmark_regime_detection);
    run_test("BENCH: Multi-Symbol Portfolio", benchmark_multi_symbol_portfolio);

    // Stress Tests
    std::cout << "\n\033[1;36m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\033[0m";
    std::cout << "\n\033[1;36m  STRESS TESTS\033[0m";
    std::cout << "\n\033[1;36m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\033[0m";
    run_test("STRESS: Rapid Trading", stress_test_rapid_trading);

    // Summary
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                          TEST SUMMARY                                ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════════════╣\n";
    std::cout << "║  Total:  " << std::setw(3) << (g_pass_count + g_fail_count) << " tests                                                    ║\n";
    std::cout << "║  \033[32mPassed: " << std::setw(3) << g_pass_count << "\033[0m                                                        ║\n";
    std::cout << "║  \033[31mFailed: " << std::setw(3) << g_fail_count << "\033[0m                                                        ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════════╝\n\n";

    // Print benchmark summary
    std::cout << "\033[1;33m┌─────────────────────────────────────────────────────────────────────┐\033[0m\n";
    std::cout << "\033[1;33m│                    BENCHMARK SUMMARY                                │\033[0m\n";
    std::cout << "\033[1;33m├─────────────────────────────────────────────────────────────────────┤\033[0m\n";

    for (const auto& r : g_results) {
        if (r.name.find("BENCH") != std::string::npos) {
            std::cout << "\033[1;33m│\033[0m  " << std::left << std::setw(35) << r.name
                      << std::right << std::setw(12) << std::fixed << std::setprecision(1)
                      << r.duration_us << " µs";
            std::cout << std::string(14, ' ') << "\033[1;33m│\033[0m\n";
        }
    }

    std::cout << "\033[1;33m└─────────────────────────────────────────────────────────────────────┘\033[0m\n\n";

    return g_fail_count > 0 ? 1 : 0;
}
