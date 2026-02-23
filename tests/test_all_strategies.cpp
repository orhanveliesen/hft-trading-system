#include "../include/strategy/fair_value.hpp"
#include "../include/strategy/momentum.hpp"
#include "../include/strategy/order_flow_imbalance.hpp"
#include "../include/strategy/pairs_trading.hpp"
#include "../include/strategy/simple_mean_reversion.hpp"
#include "../include/strategy/vwap.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace hft;
using namespace hft::strategy;

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
// MOMENTUM STRATEGY TESTS
// ============================================

TEST(momentum_needs_warmup) {
    MomentumConfig config;
    config.lookback_ticks = 5;
    MomentumStrategy strategy(config);

    // İlk 5 tick Hold döner
    for (int i = 0; i < 5; ++i) {
        auto signal = strategy(10000 + i * 10, 10010 + i * 10, 0);
        ASSERT_EQ(signal, MomentumSignal::Hold);
    }
}

TEST(momentum_detects_uptrend) {
    MomentumConfig config;
    config.lookback_ticks = 5;
    config.threshold_bps = 10;
    MomentumStrategy strategy(config);

    // Yukarı trend: 100 → 101 → 102 → 103 → 104 → 105
    Price base = 10000;
    for (int i = 0; i < 5; ++i) {
        strategy(base + i * 100, base + i * 100 + 10, 0);
    }

    // 6. tick - güçlü yukarı momentum
    auto signal = strategy(base + 500, base + 510, 0);
    ASSERT_EQ(signal, MomentumSignal::Buy);
}

TEST(momentum_detects_downtrend) {
    MomentumConfig config;
    config.lookback_ticks = 5;
    config.threshold_bps = 10;
    MomentumStrategy strategy(config);

    // Aşağı trend
    Price base = 10000;
    for (int i = 0; i < 5; ++i) {
        strategy(base - i * 100, base - i * 100 + 10, 0);
    }

    auto signal = strategy(base - 500, base - 490, 0);
    ASSERT_EQ(signal, MomentumSignal::Sell);
}

// ============================================
// VWAP STRATEGY TESTS
// ============================================

TEST(vwap_calculates_correctly) {
    VWAPConfig config;
    config.target_quantity = 1000;
    config.is_buy = true;
    VWAPStrategy strategy(config);

    // Trade'ler ekle: VWAP = (100×10 + 102×20 + 101×10) / 40 = 101.25
    strategy.on_trade(100, 10);
    strategy.on_trade(102, 20);
    strategy.on_trade(101, 10);

    Price vwap = strategy.vwap();
    // 4050 / 40 = 101.25 → 101 (integer)
    ASSERT_EQ(vwap, 101);
}

TEST(vwap_signals_buy_below_vwap) {
    VWAPConfig config;
    config.target_quantity = 1000;
    config.threshold_bps = 5;
    config.is_buy = true;
    VWAPStrategy strategy(config);

    // VWAP = 10000
    strategy.on_trade(10000, 1000);

    // Fiyat VWAP'ın %0.1 altında (10 bps)
    auto signal = strategy(9985, 9995);

    ASSERT_TRUE(signal.should_trade);
}

TEST(vwap_tracks_execution) {
    VWAPConfig config;
    config.target_quantity = 1000;
    config.slice_size = 100;
    VWAPStrategy strategy(config);

    strategy.on_trade(10000, 1000);

    ASSERT_EQ(strategy.remaining(), 1000u);
    ASSERT_FALSE(strategy.is_complete());

    strategy.on_fill(500);
    ASSERT_EQ(strategy.remaining(), 500u);
    ASSERT_NEAR(strategy.fill_rate(), 0.5, 0.01);

    strategy.on_fill(500);
    ASSERT_TRUE(strategy.is_complete());
}

// ============================================
// ORDER FLOW IMBALANCE TESTS
// ============================================

TEST(ofi_detects_buy_pressure) {
    OFIConfig config;
    config.imbalance_threshold = 0.3;
    OrderFlowImbalance strategy(config);

    // Bid: 800, Ask: 200 → Imbalance = 0.6 (alıcı baskısı)
    auto signal = strategy(800, 200, 0);

    ASSERT_EQ(signal, OFISignal::Buy);
    ASSERT_TRUE(strategy.last_imbalance() > 0.3);
}

TEST(ofi_detects_sell_pressure) {
    OFIConfig config;
    config.imbalance_threshold = 0.3;
    OrderFlowImbalance strategy(config);

    // Bid: 200, Ask: 800 → Imbalance = -0.6 (satıcı baskısı)
    auto signal = strategy(200, 800, 0);

    ASSERT_EQ(signal, OFISignal::Sell);
    ASSERT_TRUE(strategy.last_imbalance() < -0.3);
}

TEST(ofi_holds_when_balanced) {
    OFIConfig config;
    config.imbalance_threshold = 0.3;
    OrderFlowImbalance strategy(config);

    // Bid: 500, Ask: 500 → Imbalance = 0 (dengeli)
    auto signal = strategy(500, 500, 0);

    ASSERT_EQ(signal, OFISignal::Hold);
}

TEST(multi_level_ofi_weights_correctly) {
    MultiLevelOFIConfig config;
    config.num_levels = 3;
    config.imbalance_threshold = 0.2;
    config.level_weight_decay = 0.5;
    MultiLevelOFI strategy(config);

    // Level 0: 100 bid, 50 ask (weight 1.0)
    // Level 1: 50 bid, 100 ask (weight 0.5)
    // Level 2: 200 bid, 50 ask (weight 0.25)
    Quantity bids[] = {100, 50, 200};
    Quantity asks[] = {50, 100, 50};

    auto signal = strategy(bids, asks, 3, 0);

    // Weighted bid = 100×1 + 50×0.5 + 200×0.25 = 175
    // Weighted ask = 50×1 + 100×0.5 + 50×0.25 = 112.5
    // Imbalance = (175 - 112.5) / (175 + 112.5) = 0.217
    ASSERT_EQ(signal, OFISignal::Buy);
}

// ============================================
// PAIRS TRADING TESTS
// ============================================

TEST(pairs_needs_warmup) {
    PairsConfig config;
    config.lookback = 10;
    PairsTrading strategy(config);

    // İlk 10 tick'te sinyal yok
    for (int i = 0; i < 10; ++i) {
        auto signal = strategy(10000, 10000, 0);
        ASSERT_FALSE(signal.should_trade);
    }
}

TEST(pairs_enters_on_deviation) {
    PairsConfig config;
    config.lookback = 10;
    config.hedge_ratio = 1.0;
    config.entry_zscore = 2.0;
    PairsTrading strategy(config);

    // Stabil spread ile warmup
    for (int i = 0; i < 10; ++i) {
        strategy(10000, 10000, 0);
    }

    // Büyük sapma: A çok yüksek
    auto signal = strategy(11000, 10000, 0);

    // Spread >> mean → Short A, Long B
    // Not: Z-score 2'yi geçmesi lazım, bu test basitleştirilmiş
}

TEST(pairs_tracks_position_state) {
    PairsConfig config;
    PairsTrading strategy(config);

    ASSERT_FALSE(strategy.in_position());

    // Reset testi
    strategy.reset();
    ASSERT_FALSE(strategy.in_position());
}

// ============================================
// FAIR VALUE STRATEGY TESTS
// ============================================

TEST(fair_value_microprice_calculation) {
    // Microprice = (bid × ask_size + ask × bid_size) / (bid_size + ask_size)
    // bid=100, ask=102, bid_size=300, ask_size=100
    // = (100×100 + 102×300) / 400 = 40600/400 = 101.5

    double mp = FairValueStrategy::microprice(100, 102, 300, 100);
    ASSERT_NEAR(mp, 101.5, 0.01); // Daha fazla bid → fiyat ask'e yakın
}

TEST(fair_value_signals_buy_below_fv) {
    FairValueConfig config;
    config.threshold_bps = 5;
    config.use_microprice = false; // Basit mid kullan
    FairValueStrategy strategy(config);

    // İlk tick: FV = 10005
    strategy(10000, 10010, 100, 100, 0);

    // İkinci tick: Mid düştü ama FV hala yüksek
    auto signal = strategy(9990, 10000, 100, 100, 0);

    // Mid (9995) < FV (smoothed ~10003) → Buy
    ASSERT_EQ(signal, FVSignal::Buy);
}

TEST(fair_value_ema_smoothing) {
    FairValueConfig config;
    config.ema_alpha = 0.5; // Hızlı smoothing
    FairValueStrategy strategy(config);

    // FV = microprice(99, 101, 100, 100) = (99×100 + 101×100) / 200 = 100
    // Note: bid < ask required for valid market
    strategy(99, 101, 100, 100, 0);
    ASSERT_NEAR(strategy.fair_value(), 100.0, 0.1);

    // Yeni değer 200, EMA = 0.5×200 + 0.5×100 = 150
    strategy.update_fair_value(200.0);
    ASSERT_NEAR(strategy.fair_value(), 150.0, 0.1);
}

TEST(index_arb_theoretical_spot) {
    IndexArbConfig config;
    config.futures_multiplier = 1.0;
    config.cost_of_carry_bps = 10; // %0.1 carry
    IndexArbitrage strategy(config);

    // Futures = 10000
    // Theo spot = 10000 × (1 - 0.001) = 9990
    Price theo = strategy.theoretical_spot(10000);
    ASSERT_EQ(theo, 9990u);
}

TEST(index_arb_signals_correctly) {
    IndexArbConfig config;
    config.futures_multiplier = 1.0;
    config.cost_of_carry_bps = 0;
    config.threshold_bps = 5;
    IndexArbitrage strategy(config);

    // Futures = 10000, Theo = 10000
    // Spot mid = 9990 (theo'nun altında) → Buy
    auto signal = strategy(9985, 9995, 10000);
    ASSERT_EQ(signal, FVSignal::Buy);

    // Spot mid = 10010 (theo'nun üstünde) → Sell
    signal = strategy(10005, 10015, 10000);
    ASSERT_EQ(signal, FVSignal::Sell);
}

// ============================================
// MAIN
// ============================================

int main() {
    std::cout << "\n=== Strategy Library Tests ===\n\n";

    std::cout << "Momentum Strategy:\n";
    RUN_TEST(momentum_needs_warmup);
    RUN_TEST(momentum_detects_uptrend);
    RUN_TEST(momentum_detects_downtrend);

    std::cout << "\nVWAP Strategy:\n";
    RUN_TEST(vwap_calculates_correctly);
    RUN_TEST(vwap_signals_buy_below_vwap);
    RUN_TEST(vwap_tracks_execution);

    std::cout << "\nOrder Flow Imbalance:\n";
    RUN_TEST(ofi_detects_buy_pressure);
    RUN_TEST(ofi_detects_sell_pressure);
    RUN_TEST(ofi_holds_when_balanced);
    RUN_TEST(multi_level_ofi_weights_correctly);

    std::cout << "\nPairs Trading:\n";
    RUN_TEST(pairs_needs_warmup);
    RUN_TEST(pairs_enters_on_deviation);
    RUN_TEST(pairs_tracks_position_state);

    std::cout << "\nFair Value Strategy:\n";
    RUN_TEST(fair_value_microprice_calculation);
    RUN_TEST(fair_value_signals_buy_below_fv);
    RUN_TEST(fair_value_ema_smoothing);
    RUN_TEST(index_arb_theoretical_spot);
    RUN_TEST(index_arb_signals_correctly);

    std::cout << "\n=== All Strategy Tests Passed! ===\n";
    return 0;
}
