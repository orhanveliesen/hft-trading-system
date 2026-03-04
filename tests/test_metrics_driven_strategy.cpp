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
    std::cout << "[PASS] test_no_metrics_returns_none\n";
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
    std::cout << "[PASS] test_not_ready_returns_none\n";
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
    std::cout << "[PASS] test_weak_signal_below_threshold\n";
}

// Test 4: Strategy name
void test_strategy_name() {
    MetricsDrivenStrategy strategy;
    assert(strategy.name() == "MetricsDriven");
    std::cout << "[PASS] test_strategy_name\n";
}

// Test 5: Default order preference
void test_default_order_preference() {
    MetricsDrivenStrategy strategy;
    assert(strategy.default_order_preference() == OrderPreference::Limit);
    std::cout << "[PASS] test_default_order_preference\n";
}

// Test 6: All regimes suitable
void test_all_regimes_suitable() {
    MetricsDrivenStrategy strategy;

    assert(strategy.suitable_for_regime(MarketRegime::Ranging));
    assert(strategy.suitable_for_regime(MarketRegime::TrendingUp));
    assert(strategy.suitable_for_regime(MarketRegime::TrendingDown));
    assert(strategy.suitable_for_regime(MarketRegime::HighVolatility));
    assert(strategy.suitable_for_regime(MarketRegime::LowVolatility));

    std::cout << "[PASS] test_all_regimes_suitable\n";
}

// Test 7: Reset and ready
void test_reset_and_ready() {
    MetricsDrivenStrategy strategy;
    assert(!strategy.ready());

    warmup(strategy);
    assert(strategy.ready());

    strategy.reset();
    assert(!strategy.ready());

    std::cout << "[PASS] test_reset_and_ready\n";
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
    std::cout << "[PASS] test_4param_generate_returns_none\n";
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
    std::cout << "[PASS] test_no_cash_available\n";
}

// Test 10: Has_any metrics context check
void test_metrics_context_has_any() {
    MetricsContext ctx;
    assert(!ctx.has_any());

    TradeStreamMetrics trade;
    ctx.trade = &trade;
    assert(ctx.has_any());

    std::cout << "[PASS] test_metrics_context_has_any\n";
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

    std::cout << "[PASS] test_metrics_context_has_spot\n";
}

// Test 12: Has_futures metrics context check
void test_metrics_context_has_futures() {
    FuturesMetrics futures;

    MetricsContext ctx;
    ctx.futures = &futures;

    assert(ctx.has_futures());

    std::cout << "[PASS] test_metrics_context_has_futures\n";
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

    std::cout << "[PASS] test_custom_config\n";
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

    std::cout << "[PASS] test_empty_metrics_returns_none\n";
}

int main() {
    std::cout << "Running MetricsDrivenStrategy tests...\n\n";

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

    std::cout << "\n✓ All 14 tests passed!\n";
    return 0;
}
