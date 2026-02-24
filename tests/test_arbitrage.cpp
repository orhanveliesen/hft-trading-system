#include "../include/strategy/arbitrage/symbol_pair.hpp"
#include "../include/strategy/arbitrage/triangular_arb.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace hft;
using namespace hft::strategy::arbitrage;

#define TEST(name) void name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "  " << #name << "... ";                                                                          \
        name();                                                                                                        \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

#define ASSERT_EQ(a, b) assert((a) == (b))
#define ASSERT_TRUE(x) assert(x)
#define ASSERT_FALSE(x) assert(!(x))
#define ASSERT_NEAR(a, b, eps) assert(std::abs((a) - (b)) < (eps))

// ============================================
// SymbolPair Tests
// ============================================

TEST(test_parse_slash_separator) {
    auto pair = SymbolPair::parse("BTC/USDT");
    ASSERT_TRUE(pair.has_value());
    ASSERT_EQ(pair->base, "BTC");
    ASSERT_EQ(pair->quote, "USDT");
}

TEST(test_parse_dash_separator) {
    auto pair = SymbolPair::parse("ETH-BTC");
    ASSERT_TRUE(pair.has_value());
    ASSERT_EQ(pair->base, "ETH");
    ASSERT_EQ(pair->quote, "BTC");
}

TEST(test_parse_underscore_separator) {
    auto pair = SymbolPair::parse("SOL_USDC");
    ASSERT_TRUE(pair.has_value());
    ASSERT_EQ(pair->base, "SOL");
    ASSERT_EQ(pair->quote, "USDC");
}

TEST(test_parse_no_separator) {
    auto pair = SymbolPair::parse("BTCUSDT");
    ASSERT_TRUE(pair.has_value());
    ASSERT_EQ(pair->base, "BTC");
    ASSERT_EQ(pair->quote, "USDT");
}

TEST(test_parse_no_separator_eth) {
    auto pair = SymbolPair::parse("ETHBTC");
    ASSERT_TRUE(pair.has_value());
    ASSERT_EQ(pair->base, "ETH");
    ASSERT_EQ(pair->quote, "BTC");
}

TEST(test_parse_invalid) {
    auto pair = SymbolPair::parse("");
    ASSERT_FALSE(pair.has_value());
}

TEST(test_symbol_to_string) {
    SymbolPair pair{"BTC", "USDT", "BTCUSDT"};
    ASSERT_EQ(pair.to_string(), "BTC/USDT");
}

TEST(test_shares_currency) {
    SymbolPair btc_usdt{"BTC", "USDT"};
    SymbolPair eth_btc{"ETH", "BTC"};
    SymbolPair sol_usdc{"SOL", "USDC"};

    ASSERT_TRUE(SymbolPair::shares_currency(btc_usdt, eth_btc));   // BTC
    ASSERT_FALSE(SymbolPair::shares_currency(btc_usdt, sol_usdc)); // No common
}

TEST(test_common_currency) {
    SymbolPair btc_usdt{"BTC", "USDT"};
    SymbolPair eth_btc{"ETH", "BTC"};

    auto common = SymbolPair::common_currency(btc_usdt, eth_btc);
    ASSERT_TRUE(common.has_value());
    ASSERT_EQ(*common, "BTC");
}

// ============================================
// TriangularArbDetector Tests
// ============================================

TEST(test_detect_triangular_relationship) {
    ArbitrageConfig config;
    config.auto_detect = true;
    TriangularArbDetector detector(config);

    std::vector<std::string> symbols = {"BTC/USDT", "ETH/BTC", "ETH/USDT"};

    size_t count = detector.detect_relationships(symbols);
    ASSERT_EQ(count, 1);

    const auto& rel = detector.relations()[0];
    ASSERT_EQ(rel.leg1.base, "BTC");
    ASSERT_EQ(rel.leg1.quote, "USDT");
    ASSERT_EQ(rel.leg2.base, "ETH");
    ASSERT_EQ(rel.leg2.quote, "BTC");
    ASSERT_EQ(rel.leg3.base, "ETH");
    ASSERT_EQ(rel.leg3.quote, "USDT");
}

TEST(test_detect_multiple_relationships) {
    ArbitrageConfig config;
    TriangularArbDetector detector(config);

    std::vector<std::string> symbols = {"BTC/USDT", "ETH/BTC", "ETH/USDT", "SOL/BTC", "SOL/USDT"};

    size_t count = detector.detect_relationships(symbols);
    // Should find: BTC/USDT-ETH/BTC-ETH/USDT and BTC/USDT-SOL/BTC-SOL/USDT
    ASSERT_EQ(count, 2);
}

TEST(test_detect_no_separator_symbols) {
    ArbitrageConfig config;
    TriangularArbDetector detector(config);

    std::vector<std::string> symbols = {"BTCUSDT", "ETHBTC", "ETHUSDT"};

    size_t count = detector.detect_relationships(symbols);
    ASSERT_EQ(count, 1);
}

TEST(test_price_update_and_spread_calculation) {
    ArbitrageConfig config;
    config.default_min_spread_pct = 0.001; // 0.1%
    TriangularArbDetector detector(config);

    std::vector<std::string> symbols = {"BTC/USDT", "ETH/BTC", "ETH/USDT"};
    detector.detect_relationships(symbols);

    // Update prices - no arbitrage opportunity (prices balanced)
    detector.on_price_update("BTC/USDT", 50000, 50010); // BTC = $50000
    detector.on_price_update("ETH/BTC", 0.06, 0.0601);  // ETH = 0.06 BTC
    detector.on_price_update("ETH/USDT", 3000, 3005);   // ETH = $3000

    // Implied ETH/USDT = 50010 * 0.0601 = 3005.6 (ask)
    // Actual ETH/USDT bid = 3000
    // Spread = (3000 / 3005.6) - 1 = -0.19% (no opportunity)

    const auto& rel = detector.relations()[0];
    ASSERT_TRUE(rel.state.has_all_prices());
    ASSERT_TRUE(rel.state.forward_spread < config.default_min_spread_pct);
}

TEST(test_arbitrage_opportunity_detection) {
    ArbitrageConfig config;
    config.default_min_spread_pct = 0.001; // 0.1%
    TriangularArbDetector detector(config);

    std::vector<std::string> symbols = {"BTC/USDT", "ETH/BTC", "ETH/USDT"};
    detector.detect_relationships(symbols);

    // Create an arbitrage opportunity
    // BTC/USDT ask = 50000
    // ETH/BTC ask = 0.06
    // Implied ETH/USDT = 50000 * 0.06 = 3000
    // If actual ETH/USDT bid = 3010, spread = (3010/3000) - 1 = 0.33%
    detector.on_price_update("BTC/USDT", 49990, 50000);
    detector.on_price_update("ETH/BTC", 0.0599, 0.06);

    auto opportunities = detector.on_price_update("ETH/USDT", 3010, 3015);

    ASSERT_EQ(opportunities.size(), 1);
    ASSERT_EQ(opportunities[0].direction, 1); // Forward
    ASSERT_TRUE(opportunities[0].spread > 0.001);
}

TEST(test_order_generation) {
    ArbitrageConfig config;
    config.default_min_spread_pct = 0.001;
    config.default_max_quantity = 0.5;
    TriangularArbDetector detector(config);

    std::vector<std::string> symbols = {"BTC/USDT", "ETH/BTC", "ETH/USDT"};
    detector.detect_relationships(symbols);

    detector.on_price_update("BTC/USDT", 49990, 50000);
    detector.on_price_update("ETH/BTC", 0.0599, 0.06);
    auto opportunities = detector.on_price_update("ETH/USDT", 3010, 3015);

    ASSERT_EQ(opportunities.size(), 1);
    ASSERT_EQ(opportunities[0].orders.size(), 3);

    // Forward: Buy A/B, Buy C/A, Sell C/B
    ASSERT_EQ(opportunities[0].orders[0].side, Side::Buy);  // Buy BTC/USDT
    ASSERT_EQ(opportunities[0].orders[1].side, Side::Buy);  // Buy ETH/BTC
    ASSERT_EQ(opportunities[0].orders[2].side, Side::Sell); // Sell ETH/USDT
}

TEST(test_excluded_symbols) {
    ArbitrageConfig config;
    config.excluded_symbols = {"SOL"};
    TriangularArbDetector detector(config);

    std::vector<std::string> symbols = {
        "BTC/USDT", "ETH/BTC", "ETH/USDT", "SOL/BTC", "SOL/USDT" // These should be excluded
    };

    size_t count = detector.detect_relationships(symbols);
    ASSERT_EQ(count, 1); // Only BTC-ETH-USDT triangle
}

TEST(test_max_relationships_limit) {
    ArbitrageConfig config;
    config.max_auto_relationships = 2;
    TriangularArbDetector detector(config);

    std::vector<std::string> symbols = {"BTC/USDT", "ETH/BTC", "ETH/USDT", "SOL/BTC",
                                        "SOL/USDT", "ADA/BTC", "ADA/USDT"};

    size_t count = detector.detect_relationships(symbols);
    ASSERT_EQ(count, 2); // Limited to 2
}

TEST(test_get_monitored_symbols) {
    ArbitrageConfig config;
    TriangularArbDetector detector(config);

    std::vector<std::string> symbols = {"BTC/USDT", "ETH/BTC", "ETH/USDT"};
    detector.detect_relationships(symbols);

    auto monitored = detector.get_monitored_symbols();
    ASSERT_EQ(monitored.size(), 3);
}

TEST(test_statistics) {
    ArbitrageConfig config;
    config.default_min_spread_pct = 0.001;
    TriangularArbDetector detector(config);

    std::vector<std::string> symbols = {"BTC/USDT", "ETH/BTC", "ETH/USDT"};
    detector.detect_relationships(symbols);

    // Create opportunity
    detector.on_price_update("BTC/USDT", 49990, 50000);
    detector.on_price_update("ETH/BTC", 0.0599, 0.06);
    auto opportunities = detector.on_price_update("ETH/USDT", 3010, 3015);

    auto stats = detector.get_stats();
    ASSERT_EQ(stats.total_relations, 1);
    ASSERT_EQ(stats.active_relations, 1);
    ASSERT_EQ(stats.total_opportunities, 1);
}

TEST(test_opportunity_callback) {
    ArbitrageConfig config;
    config.default_min_spread_pct = 0.001;
    TriangularArbDetector detector(config);

    std::vector<std::string> symbols = {"BTC/USDT", "ETH/BTC", "ETH/USDT"};
    detector.detect_relationships(symbols);

    int callback_count = 0;
    detector.set_opportunity_callback([&](const ArbOpportunity& opp) {
        callback_count++;
        ASSERT_TRUE(opp.spread > 0);
    });

    detector.on_price_update("BTC/USDT", 49990, 50000);
    detector.on_price_update("ETH/BTC", 0.0599, 0.06);
    detector.on_price_update("ETH/USDT", 3010, 3015);

    ASSERT_EQ(callback_count, 1);
}

// ============================================
// Main
// ============================================

int main() {
    std::cout << "=== Arbitrage Tests ===\n\n";

    std::cout << "SymbolPair Tests:\n";
    RUN_TEST(test_parse_slash_separator);
    RUN_TEST(test_parse_dash_separator);
    RUN_TEST(test_parse_underscore_separator);
    RUN_TEST(test_parse_no_separator);
    RUN_TEST(test_parse_no_separator_eth);
    RUN_TEST(test_parse_invalid);
    RUN_TEST(test_symbol_to_string);
    RUN_TEST(test_shares_currency);
    RUN_TEST(test_common_currency);

    std::cout << "\nTriangularArbDetector Tests:\n";
    RUN_TEST(test_detect_triangular_relationship);
    RUN_TEST(test_detect_multiple_relationships);
    RUN_TEST(test_detect_no_separator_symbols);
    RUN_TEST(test_price_update_and_spread_calculation);
    RUN_TEST(test_arbitrage_opportunity_detection);
    RUN_TEST(test_order_generation);
    RUN_TEST(test_excluded_symbols);
    RUN_TEST(test_max_relationships_limit);
    RUN_TEST(test_get_monitored_symbols);
    RUN_TEST(test_statistics);
    RUN_TEST(test_opportunity_callback);

    std::cout << "\n=== All 20 tests passed! ===\n";
    return 0;
}
