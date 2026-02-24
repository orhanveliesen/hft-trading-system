#include "../include/mock_order_sender.hpp"
#include "../include/strategy/simple_mean_reversion.hpp"
#include "../include/trading_engine.hpp"

#include <cassert>
#include <iostream>

using namespace hft;
using namespace hft::strategy;

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
#define EXPECT_EQ(a, b) ASSERT_EQ(a, b)
#define EXPECT_NE(a, b) ASSERT_NE(a, b)

// ============================================
// Temel Davranış Testleri
// ============================================

TEST(test_first_tick_returns_hold) {
    SimpleMRConfig config;
    config.order_size = 100;
    config.max_position = 1000;
    SimpleMeanReversion strategy(config);

    // İlk tick'te referans alır, Hold döner
    auto signal = strategy(10000, 10010, 0);

    EXPECT_EQ(signal, Signal::None);
}

TEST(test_price_drop_triggers_buy) {
    SimpleMRConfig config;
    SimpleMeanReversion strategy(config);

    // İlk tick: mid = 10005
    strategy(10000, 10010, 0);

    // İkinci tick: mid = 10003 (düştü)
    auto signal = strategy(9998, 10008, 0);

    EXPECT_EQ(signal, Signal::Buy);
}

TEST(test_price_rise_triggers_sell) {
    SimpleMRConfig config;
    SimpleMeanReversion strategy(config);

    // İlk tick: mid = 10005
    strategy(10000, 10010, 0);

    // İkinci tick: mid = 10008 (çıktı)
    auto signal = strategy(10003, 10013, 0);

    EXPECT_EQ(signal, Signal::Sell);
}

TEST(test_no_price_change_returns_hold) {
    SimpleMRConfig config;
    SimpleMeanReversion strategy(config);

    // İlk tick
    strategy(10000, 10010, 0);

    // Aynı mid price
    auto signal = strategy(10000, 10010, 0);

    EXPECT_EQ(signal, Signal::None);
}

// ============================================
// Pozisyon Limiti Testleri
// ============================================

TEST(test_respects_max_long_position) {
    SimpleMRConfig config;
    config.max_position = 1000;
    SimpleMeanReversion strategy(config);

    strategy(10000, 10010, 0);

    // Fiyat düştü ama pozisyon limitte
    auto signal = strategy(9998, 10008, config.max_position);

    EXPECT_EQ(signal, Signal::None); // Al diyemez, limit dolu
}

TEST(test_respects_max_short_position) {
    SimpleMRConfig config;
    config.max_position = 1000;
    SimpleMeanReversion strategy(config);

    strategy(10000, 10010, 0);

    // Fiyat çıktı ama short limit dolu
    auto signal = strategy(10003, 10013, -config.max_position);

    EXPECT_EQ(signal, Signal::None); // Sat diyemez, limit dolu
}

// ============================================
// Edge Case Testleri
// ============================================

TEST(test_invalid_prices_return_hold) {
    SimpleMRConfig config;
    SimpleMeanReversion strategy(config);

    // Invalid bid
    EXPECT_EQ(strategy(INVALID_PRICE, 10010, 0), Signal::None);

    // Invalid ask
    EXPECT_EQ(strategy(10000, INVALID_PRICE, 0), Signal::None);

    // Crossed market (bid >= ask)
    EXPECT_EQ(strategy(10010, 10000, 0), Signal::None);
}

TEST(test_reset_clears_state) {
    SimpleMRConfig config;
    SimpleMeanReversion strategy(config);

    // Birkaç tick işle
    strategy(10000, 10010, 0);
    strategy(9998, 10008, 0);

    // Reset
    strategy.reset();

    // Tekrar ilk tick gibi davranmalı
    auto signal = strategy(10000, 10010, 0);
    EXPECT_EQ(signal, Signal::None);
}

// ============================================
// Entegrasyon Testi - TradingEngine ile
// ============================================

TEST(test_integration_with_trading_engine) {
    // Engine oluştur
    MockOrderSender sender;
    TradingEngine<MockOrderSender> engine(sender);

    // Symbol ekle
    SymbolConfig sym_config;
    sym_config.symbol = "TEST";
    sym_config.base_price = 10000;
    sym_config.max_position = 1000;
    Symbol sym = engine.add_symbol(sym_config);

    // Strategy oluştur
    SimpleMRConfig mr_config;
    SimpleMeanReversion strategy(mr_config);

    // SymbolWorld'e eriş
    auto* world = engine.get_symbol_world(sym);
    ASSERT_NE(world, nullptr);

    // Simüle: İlk market data
    world->book().add_order(1, Side::Buy, 10000, 100);  // Bid
    world->book().add_order(2, Side::Sell, 10010, 100); // Ask

    Price bid = world->best_bid();
    Price ask = world->best_ask();

    // Strategy çalıştır
    auto signal = strategy(bid, ask, world->position_qty());
    EXPECT_EQ(signal, Signal::None); // İlk tick

    // Simüle: Fiyat düştü
    world->book().cancel_order(1);
    world->book().cancel_order(2);
    world->book().add_order(3, Side::Buy, 9995, 100);
    world->book().add_order(4, Side::Sell, 10005, 100);

    bid = world->best_bid();
    ask = world->best_ask();

    signal = strategy(bid, ask, world->position_qty());
    EXPECT_EQ(signal, Signal::Buy); // Fiyat düştü, al sinyali
}

// ============================================
// Tam Döngü Testi
// ============================================

TEST(test_full_trading_cycle) {
    SimpleMRConfig config;
    config.order_size = 100;
    SimpleMeanReversion strategy(config);
    int64_t position = 0;

    // Tick 1: Başlangıç (Hold)
    EXPECT_EQ(strategy(10000, 10010, position), Signal::None);

    // Tick 2: Fiyat düştü (Buy)
    auto signal = strategy(9990, 10000, position);
    EXPECT_EQ(signal, Signal::Buy);
    position += config.order_size; // Simüle fill

    // Tick 3: Fiyat düştü (Buy - pozisyon artıyor)
    signal = strategy(9980, 9990, position);
    EXPECT_EQ(signal, Signal::Buy);
    position += config.order_size;

    // Tick 4: Fiyat çıktı (Sell)
    signal = strategy(9990, 10000, position);
    EXPECT_EQ(signal, Signal::Sell);
    position -= config.order_size;

    // Tick 5: Fiyat çıktı (Sell)
    signal = strategy(10000, 10010, position);
    EXPECT_EQ(signal, Signal::Sell);
    position -= config.order_size;

    // Pozisyon 0'a döndü
    EXPECT_EQ(position, 0);
}

int main() {
    std::cout << "\n=== Simple Mean Reversion Strategy Tests ===\n\n";

    RUN_TEST(test_first_tick_returns_hold);
    RUN_TEST(test_price_drop_triggers_buy);
    RUN_TEST(test_price_rise_triggers_sell);
    RUN_TEST(test_no_price_change_returns_hold);
    RUN_TEST(test_respects_max_long_position);
    RUN_TEST(test_respects_max_short_position);
    RUN_TEST(test_invalid_prices_return_hold);
    RUN_TEST(test_reset_clears_state);
    RUN_TEST(test_integration_with_trading_engine);
    RUN_TEST(test_full_trading_cycle);

    std::cout << "\nAll tests passed!\n";
    return 0;
}
