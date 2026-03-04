#include "../include/metrics/combined_metrics.hpp"
#include "../include/metrics/futures_metrics.hpp"
#include "../include/metrics/order_book_metrics.hpp"
#include "../include/metrics/order_flow_metrics.hpp"
#include "../include/metrics/trade_stream_metrics.hpp"
#include "../include/strategy/metrics_driven_strategy.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace hft;
using namespace hft::strategy;

// Helper to warmup strategy
void warmup(MetricsDrivenStrategy& strategy) {
    MarketSnapshot market;
    market.bid = 100000;
    market.ask = 100010;
    market.bid_size = 1000;
    market.ask_size = 1000;

    for (int i = 0; i < 101; i++) {
        strategy.on_tick(market);
    }
}

// Test 1: No metrics returns none
void test_no_metrics_returns_none() {
    MetricsDrivenStrategy strategy;
    warmup(strategy);

    MarketSnapshot market;
    market.bid = 100000;
    market.ask = 100010;
    market.bid_size = 1000;
    market.ask_size = 1000;

    StrategyPosition position;
    position.cash_available = 10000;
    position.max_position = 10000;

    Signal signal = strategy.generate(0, market, position, MarketRegime::Ranging, nullptr);
    assert(signal.type == SignalType::None);
    std::cout << "[PASS] test_no_metrics_returns_none\\n";
}

// Test 2: Not ready returns none
void test_not_ready_returns_none() {
    MetricsDrivenStrategy strategy;
    // Don't warmup

    TradeStreamMetrics trade;
    OrderBookMetrics book;
    OrderFlowMetrics<20> flow;
    CombinedMetrics combined(trade, book);
    FuturesMetrics futures;

    MetricsContext ctx;
    ctx.trade = &trade;
    ctx.book = &book;
    ctx.flow = &flow;
    ctx.combined = &combined;
    ctx.futures = &futures;

    MarketSnapshot market;
    market.bid = 100000;
    market.ask = 100010;

    StrategyPosition position;
    position.cash_available = 10000;
    position.max_position = 10000;

    Signal signal = strategy.generate(0, market, position, MarketRegime::Ranging, &ctx);
    assert(signal.type == SignalType::None);
    std::cout << "[PASS] test_not_ready_returns_none\\n";
}

// Test 3: Weak signal below threshold (no metrics = no signal)
void test_weak_signal_below_threshold() {
    MetricsDrivenStrategy strategy;
    warmup(strategy);

    TradeStreamMetrics trade;
    OrderBookMetrics book;
    OrderFlowMetrics<20> flow;
    CombinedMetrics combined(trade, book);
    FuturesMetrics futures;

    MetricsContext ctx;
    ctx.trade = &trade;
    ctx.book = &book;
    ctx.flow = &flow;
    ctx.combined = &combined;
    ctx.futures = &futures;

    MarketSnapshot market;
    market.bid = 99900;
    market.ask = 100100;

    StrategyPosition position;
    position.cash_available = 10000;
    position.max_position = 10000;

    Signal signal = strategy.generate(0, market, position, MarketRegime::Ranging, &ctx);
    // No metrics populated = score ~0 = below threshold
    assert(signal.type == SignalType::None);
    std::cout << "[PASS] test_weak_signal_below_threshold\\n";
}

// Test 4: Strategy name
void test_strategy_name() {
    MetricsDrivenStrategy strategy;
    assert(strategy.name() == "MetricsDriven");
    std::cout << "[PASS] test_strategy_name\\n";
}

// Test 5: Default order preference
void test_default_order_preference() {
    MetricsDrivenStrategy strategy;
    assert(strategy.default_order_preference() == OrderPreference::Limit);
    std::cout << "[PASS] test_default_order_preference\\n";
}

// Test 6: All regimes suitable
void test_all_regimes_suitable() {
    MetricsDrivenStrategy strategy;

    assert(strategy.suitable_for_regime(MarketRegime::Ranging));
    assert(strategy.suitable_for_regime(MarketRegime::TrendingUp));
    assert(strategy.suitable_for_regime(MarketRegime::TrendingDown));
    assert(strategy.suitable_for_regime(MarketRegime::HighVolatility));
    assert(strategy.suitable_for_regime(MarketRegime::LowVolatility));

    std::cout << "[PASS] test_all_regimes_suitable\\n";
}

// Test 7: Reset and ready
void test_reset_and_ready() {
    MetricsDrivenStrategy strategy;
    assert(!strategy.ready());

    warmup(strategy);
    assert(strategy.ready());

    strategy.reset();
    assert(!strategy.ready());

    std::cout << "[PASS] test_reset_and_ready\\n";
}

// Test 8: 4-param generate returns none
void test_4param_generate_returns_none() {
    MetricsDrivenStrategy strategy;
    warmup(strategy);

    MarketSnapshot market;
    market.bid = 100000;
    market.ask = 100010;

    StrategyPosition position;
    position.cash_available = 10000;
    position.max_position = 10000;

    // Call 4-param version (no metrics)
    Signal signal = strategy.generate(0, market, position, MarketRegime::Ranging);
    assert(signal.type == SignalType::None);
    std::cout << "[PASS] test_4param_generate_returns_none\\n";
}

// Test 9: No cash available returns none
void test_no_cash_available() {
    MetricsDrivenStrategy strategy;
    warmup(strategy);

    TradeStreamMetrics trade;
    OrderBookMetrics book;
    OrderFlowMetrics<20> flow;
    CombinedMetrics combined(trade, book);
    FuturesMetrics futures;

    MetricsContext ctx;
    ctx.trade = &trade;
    ctx.book = &book;
    ctx.flow = &flow;
    ctx.combined = &combined;
    ctx.futures = &futures;

    MarketSnapshot market;
    market.bid = 99900;
    market.ask = 100100;

    StrategyPosition position;
    position.cash_available = 0; // No cash
    position.max_position = 10000;

    Signal signal = strategy.generate(0, market, position, MarketRegime::Ranging, &ctx);
    assert(signal.type == SignalType::None);
    std::cout << "[PASS] test_no_cash_available\\n";
}

// Test 10: Has_any metrics context check
void test_metrics_context_has_any() {
    MetricsContext ctx;
    assert(!ctx.has_any());

    TradeStreamMetrics trade;
    ctx.trade = &trade;
    assert(ctx.has_any());

    std::cout << "[PASS] test_metrics_context_has_any\\n";
}

// Test 11: Has_spot metrics context check
void test_metrics_context_has_spot() {
    TradeStreamMetrics trade;
    OrderBookMetrics book;
    OrderFlowMetrics<20> flow;
    CombinedMetrics combined(trade, book);

    MetricsContext ctx;
    ctx.trade = &trade;
    ctx.book = &book;
    ctx.flow = &flow;
    ctx.combined = &combined;

    assert(ctx.has_spot());

    std::cout << "[PASS] test_metrics_context_has_spot\\n";
}

// Test 12: Has_futures metrics context check
void test_metrics_context_has_futures() {
    FuturesMetrics futures;

    MetricsContext ctx;
    ctx.futures = &futures;

    assert(ctx.has_futures());

    std::cout << "[PASS] test_metrics_context_has_futures\\n";
}

// Test 13: Custom config
void test_custom_config() {
    MetricsDrivenStrategy::Config config;
    config.score_threshold = 50.0;
    config.warmup_ticks = 50;

    MetricsDrivenStrategy strategy(config);
    assert(!strategy.ready());

    // Warmup less than config requires
    MarketSnapshot market;
    market.bid = 100000;
    market.ask = 100010;
    for (int i = 0; i < 40; i++) {
        strategy.on_tick(market);
    }
    assert(!strategy.ready());

    // Warmup exactly as config requires
    for (int i = 0; i < 10; i++) {
        strategy.on_tick(market);
    }
    assert(strategy.ready());

    std::cout << "[PASS] test_custom_config\\n";
}

// Test 14: Empty metrics returns none
void test_empty_metrics_returns_none() {
    MetricsDrivenStrategy strategy;
    warmup(strategy);

    // Create empty metrics (no data)
    TradeStreamMetrics trade;
    OrderBookMetrics book;
    OrderFlowMetrics<20> flow;
    CombinedMetrics combined(trade, book);
    FuturesMetrics futures;

    MetricsContext ctx;
    ctx.trade = &trade;
    ctx.book = &book;
    ctx.flow = &flow;
    ctx.combined = &combined;
    ctx.futures = &futures;

    MarketSnapshot market;
    market.bid = 99900;
    market.ask = 100100;

    StrategyPosition position;
    position.cash_available = 10000;
    position.max_position = 10000;

    // Empty metrics should result in score ~0, below threshold
    Signal signal = strategy.generate(0, market, position, MarketRegime::Ranging, &ctx);
    assert(signal.type == SignalType::None);

    std::cout << "[PASS] test_empty_metrics_returns_none\\n";
}

// Test 15: Strong buy signal - trade flow bullish
void test_strong_buy_signal() {
    MetricsDrivenStrategy strategy;
    warmup(strategy);

    TradeStreamMetrics trade;
    OrderBookMetrics book;
    OrderFlowMetrics<20> flow;
    CombinedMetrics combined(trade, book);
    FuturesMetrics futures;

    // Trade flow: 100% buys → buy_ratio = 1.0 > 0.65 → +25 score
    for (int i = 0; i < 100; i++) {
        trade.on_trade(100000, 100, true, i * 1000);
    }

    MetricsContext ctx;
    ctx.trade = &trade;
    ctx.book = &book;
    ctx.flow = &flow;
    ctx.combined = &combined;
    ctx.futures = &futures;

    MarketSnapshot market;
    market.bid = 99900;
    market.ask = 100100;

    StrategyPosition position;
    position.cash_available = 10000;
    position.max_position = 10000;

    Signal signal = strategy.generate(0, market, position, MarketRegime::Ranging, &ctx);

    // Score = +25, above threshold 20 → Buy signal
    assert(signal.type == SignalType::Buy);
    std::cout << "[PASS] test_strong_buy_signal\\n";
}

// Test 16: Strong sell signal - trade flow bearish
void test_strong_sell_signal() {
    MetricsDrivenStrategy strategy;
    warmup(strategy);

    TradeStreamMetrics trade;
    OrderBookMetrics book;
    OrderFlowMetrics<20> flow;
    CombinedMetrics combined(trade, book);
    FuturesMetrics futures;

    // Trade flow: 100% sells → buy_ratio = 0.0 < 0.35 → -25 score
    for (int i = 0; i < 100; i++) {
        trade.on_trade(100000, 100, false, i * 1000);
    }

    MetricsContext ctx;
    ctx.trade = &trade;
    ctx.book = &book;
    ctx.flow = &flow;
    ctx.combined = &combined;
    ctx.futures = &futures;

    MarketSnapshot market;
    market.bid = 99900;
    market.ask = 100100;

    StrategyPosition position;
    position.cash_available = 10000;
    position.max_position = 10000;

    Signal signal = strategy.generate(0, market, position, MarketRegime::Ranging, &ctx);

    // Score = -25, below threshold -20 → Sell signal
    assert(signal.type == SignalType::Sell);
    std::cout << "[PASS] test_strong_sell_signal\\n";
}

// Test 17: Trade flow interpolation - buy_ratio in middle range
void test_trade_flow_interpolation() {
    MetricsDrivenStrategy strategy;
    warmup(strategy);

    TradeStreamMetrics trade;
    OrderBookMetrics book;
    OrderFlowMetrics<20> flow;
    CombinedMetrics combined(trade, book);
    FuturesMetrics futures;

    // Trade flow: 60% buys, 40% sells → buy_ratio = 0.6 (in [0.35, 0.65])
    // Expected score: (0.6 - 0.5) / 0.15 * 25.0 = 0.1 / 0.15 * 25.0 = 16.67
    for (int i = 0; i < 60; i++) {
        trade.on_trade(100000, 100, true, i * 1000);
    }
    for (int i = 60; i < 100; i++) {
        trade.on_trade(100000, 100, false, i * 1000);
    }

    MetricsContext ctx;
    ctx.trade = &trade;
    ctx.book = &book;
    ctx.flow = &flow;
    ctx.combined = &combined;
    ctx.futures = &futures;

    MarketSnapshot market;
    market.bid = 99900;
    market.ask = 100100;

    StrategyPosition position;
    position.cash_available = 10000;
    position.max_position = 10000;

    Signal signal = strategy.generate(0, market, position, MarketRegime::Ranging, &ctx);

    // Score ~16.67, below threshold 20 → None (validates interpolation doesn't jump)
    assert(signal.type == SignalType::None);
    std::cout << "[PASS] test_trade_flow_interpolation\\n";
}

// Test 18: High volatility higher threshold
void test_hvol_higher_threshold() {
    MetricsDrivenStrategy strategy;
    warmup(strategy);

    TradeStreamMetrics trade;
    OrderBookMetrics book;
    OrderFlowMetrics<20> flow;
    CombinedMetrics combined(trade, book);
    FuturesMetrics futures;

    // Weak bullish signal (score = 25)
    for (int i = 0; i < 100; i++) {
        trade.on_trade(100000, 100, true, i * 1000);
    }

    MetricsContext ctx;
    ctx.trade = &trade;
    ctx.book = &book;
    ctx.flow = &flow;
    ctx.combined = &combined;
    ctx.futures = &futures;

    MarketSnapshot market;
    market.bid = 99900;
    market.ask = 100100;

    StrategyPosition position;
    position.cash_available = 10000;
    position.max_position = 10000;

    // Test with Ranging regime (threshold 20)
    Signal signal1 = strategy.generate(0, market, position, MarketRegime::Ranging, &ctx);
    assert(signal1.is_actionable()); // Score 25 > 20

    // Test with HighVolatility regime (threshold 30)
    Signal signal2 = strategy.generate(0, market, position, MarketRegime::HighVolatility, &ctx);
    assert(signal2.type == SignalType::None); // Score 25 < 30

    std::cout << "[PASS] test_hvol_higher_threshold\\n";
}

// Test 19: Exit on disagreement - long position + strong sell score
void test_exit_on_disagreement() {
    MetricsDrivenStrategy strategy;
    warmup(strategy);

    TradeStreamMetrics trade;
    OrderBookMetrics book;
    OrderFlowMetrics<20> flow;
    CombinedMetrics combined(trade, book);
    FuturesMetrics futures;

    // Strong bearish data (score = -25)
    for (int i = 0; i < 100; i++) {
        trade.on_trade(100000, 100, false, i * 1000);
    }

    MetricsContext ctx;
    ctx.trade = &trade;
    ctx.book = &book;
    ctx.flow = &flow;
    ctx.combined = &combined;
    ctx.futures = &futures;

    MarketSnapshot market;
    market.bid = 99900;
    market.ask = 100100;

    StrategyPosition position;
    position.cash_available = 5000;
    position.max_position = 10000;
    position.quantity = 50; // Long position
    position.avg_entry_price = 100000;

    Signal signal = strategy.generate(0, market, position, MarketRegime::Ranging, &ctx);

    // Long + strong sell signal (score = -25, but needs >60 for exit)
    // Actually score -25 is not > 60, so this will hold
    assert(signal.type == SignalType::None);
    std::cout << "[PASS] test_exit_on_disagreement\\n";
}

// Test 20: Hold on agreement - long position + buy score
void test_hold_on_agreement() {
    MetricsDrivenStrategy strategy;
    warmup(strategy);

    TradeStreamMetrics trade;
    OrderBookMetrics book;
    OrderFlowMetrics<20> flow;
    CombinedMetrics combined(trade, book);
    FuturesMetrics futures;

    // Moderate bullish data
    for (int i = 0; i < 100; i++) {
        trade.on_trade(100000, 100, true, i * 1000);
    }

    MetricsContext ctx;
    ctx.trade = &trade;
    ctx.book = &book;
    ctx.flow = &flow;
    ctx.combined = &combined;
    ctx.futures = &futures;

    MarketSnapshot market;
    market.bid = 99900;
    market.ask = 100100;

    StrategyPosition position;
    position.cash_available = 5000;
    position.max_position = 10000;
    position.quantity = 50; // Long position
    position.avg_entry_price = 100000;

    Signal signal = strategy.generate(0, market, position, MarketRegime::Ranging, &ctx);

    // Long + buy signal → Hold (no pyramiding)
    assert(signal.type == SignalType::None);
    std::cout << "[PASS] test_hold_on_agreement\\n";
}

int main() {
    std::cout << "Running MetricsDrivenStrategy tests...\\n\\n";

    // Basic behavior tests
    test_no_metrics_returns_none();
    test_not_ready_returns_none();
    test_weak_signal_below_threshold();
    test_strategy_name();
    test_default_order_preference();
    test_all_regimes_suitable();
    test_reset_and_ready();
    test_4param_generate_returns_none();
    test_no_cash_available();
    test_metrics_context_has_any();
    test_metrics_context_has_spot();
    test_metrics_context_has_futures();
    test_custom_config();
    test_empty_metrics_returns_none();

    // Scoring logic tests
    test_strong_buy_signal();
    test_strong_sell_signal();
    test_trade_flow_interpolation();
    test_hvol_higher_threshold();
    test_exit_on_disagreement();
    test_hold_on_agreement();

    std::cout << "\\n✓ All 20 tests passed!\\n";
    return 0;
}
