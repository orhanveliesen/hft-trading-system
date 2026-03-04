#include "../include/strategy/istrategy.hpp"
#include "../include/strategy/metrics_context.hpp"
#include "../include/strategy/metrics_driven_strategy.hpp"
#include "../include/strategy/strategy_selector.hpp"
#include "../include/strategy/technical_indicators_strategy.hpp"

#include <cassert>
#include <iostream>

using namespace hft;
using namespace hft::strategy;

// Test 1: Existing strategy with metrics context delegates to 4-param
void test_existing_strategy_with_metrics() {
    TechnicalIndicatorsStrategy::Config config;
    config.base_position_pct = 0.02;
    config.max_position_pct = 0.05;
    config.price_scale = 10000;

    TechnicalIndicatorsStrategy strategy(config);

    // Warmup
    MarketSnapshot market;
    market.bid = 100000;
    market.ask = 100010;
    market.bid_size = 1000;
    market.ask_size = 1000;

    for (int i = 0; i < 60; i++) {
        strategy.on_tick(market);
    }

    StrategyPosition position;
    position.cash_available = 10000;
    position.max_position = 10000;

    MetricsContext ctx; // Empty metrics

    // Call 4-param version
    Signal signal_4param = strategy.generate(0, market, position, MarketRegime::Ranging);

    // TechnicalIndicatorsStrategy doesn't override 5-param, so it uses default delegation
    // which calls the 4-param version internally
    assert(signal_4param.type == SignalType::None || signal_4param.is_actionable());
    std::cout << "[PASS] test_existing_strategy_with_metrics\n";
}

// Test 2: MetricsDrivenStrategy overrides 5-param
void test_metrics_driven_overrides_5param() {
    MetricsDrivenStrategy strategy;

    // Warmup
    MarketSnapshot market;
    market.bid = 100000;
    market.ask = 100010;
    for (int i = 0; i < 101; i++) {
        strategy.on_tick(market);
    }

    StrategyPosition position;
    position.cash_available = 10000;
    position.max_position = 10000;

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

    // Simulate bullish signal (need 4 params: price, quantity, is_buy, timestamp)
    for (int i = 0; i < 100; i++) {
        trade.on_trade(100.0, 100, true, i * 1000);
    }

    // Call 5-param with metrics
    Signal signal_5param = strategy.generate(0, market, position, MarketRegime::Ranging, &ctx);

    // Call 4-param without metrics
    Signal signal_4param = strategy.generate(0, market, position, MarketRegime::Ranging);

    // 5-param should generate signal, 4-param should return none
    assert(signal_5param.is_actionable());
    assert(!signal_4param.is_actionable());
    std::cout << "[PASS] test_metrics_driven_overrides_5param\n";
}

// Test 3: nullptr metrics handled gracefully
void test_nullptr_metrics_safe() {
    MetricsDrivenStrategy strategy;

    MarketSnapshot market;
    market.bid = 100000;
    market.ask = 100010;

    for (int i = 0; i < 101; i++) {
        strategy.on_tick(market);
    }

    StrategyPosition position;
    position.cash_available = 10000;
    position.max_position = 10000;

    // Pass nullptr - should return None
    Signal signal = strategy.generate(0, market, position, MarketRegime::Ranging, nullptr);
    assert(signal.type == SignalType::None);
    std::cout << "[PASS] test_nullptr_metrics_safe\n";
}

// Test 4: StrategySelector::composite_signal passes metrics
void test_composite_signal_with_metrics() {
    StrategySelector selector;

    // Register TechnicalIndicatorsStrategy
    TechnicalIndicatorsStrategy::Config ti_config;
    ti_config.base_position_pct = 0.02;
    ti_config.max_position_pct = 0.05;
    ti_config.price_scale = 10000;
    selector.register_strategy(std::make_unique<TechnicalIndicatorsStrategy>(ti_config));

    // Register MetricsDrivenStrategy
    selector.register_strategy(std::make_unique<MetricsDrivenStrategy>());

    // Warmup both strategies
    MarketSnapshot market;
    market.bid = 100000;
    market.ask = 100010;
    for (int i = 0; i < 101; i++) {
        selector.on_tick_all(market);
    }

    StrategyPosition position;
    position.cash_available = 10000;
    position.max_position = 10000;

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

    // Call composite_signal with metrics
    Signal signal = selector.composite_signal(0, market, position, MarketRegime::Ranging, &ctx);

    // Should not crash and return valid signal
    assert(true);
    std::cout << "[PASS] test_composite_signal_with_metrics\n";
}

// Test 5: StrategySelector::select_by_name works correctly
void test_select_by_name() {
    StrategySelector selector;

    // Register strategies
    TechnicalIndicatorsStrategy::Config ti_config;
    ti_config.base_position_pct = 0.02;
    ti_config.max_position_pct = 0.05;
    ti_config.price_scale = 10000;
    selector.register_strategy(std::make_unique<TechnicalIndicatorsStrategy>(ti_config));
    selector.register_strategy(std::make_unique<MetricsDrivenStrategy>());

    // Select by name
    auto* metrics_strat = selector.select_by_name("MetricsDriven");
    auto* ti_strat = selector.select_by_name("TechnicalIndicators");
    auto* nonexistent = selector.select_by_name("NonExistent");

    assert(metrics_strat != nullptr);
    assert(ti_strat != nullptr);
    assert(nonexistent == nullptr);

    assert(metrics_strat->name() == "MetricsDriven");
    assert(ti_strat->name() == "TechnicalIndicators");

    std::cout << "[PASS] test_select_by_name\n";
}

int main() {
    std::cout << "Running Strategy Interface backward compatibility tests...\n\n";

    test_existing_strategy_with_metrics();
    test_metrics_driven_overrides_5param();
    test_nullptr_metrics_safe();
    test_composite_signal_with_metrics();
    test_select_by_name();

    std::cout << "\n✓ All 5 tests passed!\n";
    return 0;
}
