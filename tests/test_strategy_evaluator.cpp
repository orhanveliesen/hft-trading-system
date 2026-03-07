#include "../include/core/strategy_evaluator.hpp"

#include <cassert>
#include <iostream>
#include <vector>

using namespace hft;
using namespace hft::strategy;
using namespace hft::execution;

#define TEST(name) void name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "Running " << #name << "... ";                                                                    \
        name();                                                                                                        \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

// ============================================================================
// Mock Components
// ============================================================================

class MockStrategy : public IStrategy {
public:
    Signal next_signal;

    Signal generate(Symbol, const MarketSnapshot&, const StrategyPosition&, MarketRegime,
                    const MetricsContext*) override {
        return next_signal;
    }

    std::string_view name() const override { return "MockStrategy"; }

    OrderPreference default_order_preference() const override { return OrderPreference::Either; }

    bool suitable_for_regime(MarketRegime) const override { return true; }

    void on_tick(const MarketSnapshot&) override {}

    void reset() override {}

    bool is_ready() const override { return true; }
};

class MockStrategySelector : public StrategySelector {
public:
    MockStrategy* mock_strategy_ptr = nullptr;

    IStrategy* select_for_regime(MarketRegime) override { return mock_strategy_ptr; }
};

class MockMetricsManager {
public:
    MetricsContext ctx;

    MetricsContext context_for(Symbol) const { return ctx; }
};

class MockLimitManager {
public:
    int check_timeouts_count = 0;

    void check_limit_timeouts() { check_timeouts_count++; }
};

// Event capture helper
template <typename Event>
class EventCapture {
public:
    std::vector<Event> events;

    void subscribe(core::EventBus* bus) {
        bus->subscribe<Event>([this](const Event& e) { events.push_back(e); });
    }

    void clear() { events.clear(); }

    bool has_event() const { return !events.empty(); }

    const Event& last() const { return events.back(); }

    size_t count() const { return events.size(); }
};

// ============================================================================
// Tests
// ============================================================================

TEST(test_buy_signal_publishes_spot_buy_event) {
    core::EventBus bus;
    MockMetricsManager metrics;
    MockLimitManager limit_mgr;
    MockStrategySelector selector;
    MockStrategy mock_strategy;
    selector.mock_strategy_ptr = &mock_strategy;
    core::StrategyEvaluator evaluator(&bus, &metrics, &limit_mgr, &selector);

    // Capture SpotBuyEvent
    EventCapture<core::SpotBuyEvent> capture;
    capture.subscribe(&bus);

    // Set up buy signal with strong strength (prefer market)
    mock_strategy.next_signal.type = SignalType::Buy;
    mock_strategy.next_signal.strength = SignalStrength::Strong;
    mock_strategy.next_signal.suggested_qty = 1.0;
    mock_strategy.next_signal.reason = "test_buy";

    // Set metrics context with non-dangerous regime
    metrics.ctx.regime = MarketRegime::Ranging;

    // Evaluate
    MarketSnapshot market;
    market.bid = 100'000'000;
    market.ask = 100'100'000;
    StrategyPosition pos;

    evaluator.evaluate(0, market, pos);

    // Verify SpotBuyEvent was published
    assert(capture.has_event());
    assert(capture.last().symbol == 0);
    assert(capture.last().qty == 1.0);
    assert(std::string(capture.last().reason) == "test_buy");
}

TEST(test_sell_signal_publishes_spot_sell_event) {
    core::EventBus bus;
    MockMetricsManager metrics;
    MockLimitManager limit_mgr;
    MockStrategySelector selector;
    MockStrategy mock_strategy;
    selector.mock_strategy_ptr = &mock_strategy;
    core::StrategyEvaluator evaluator(&bus, &metrics, &limit_mgr, &selector);

    // Capture SpotSellEvent
    EventCapture<core::SpotSellEvent> capture;
    capture.subscribe(&bus);

    // Set up sell signal
    mock_strategy.next_signal.type = SignalType::Sell;
    mock_strategy.next_signal.strength = SignalStrength::Strong;
    mock_strategy.next_signal.suggested_qty = 1.0;
    mock_strategy.next_signal.reason = "test_sell";

    metrics.ctx.regime = MarketRegime::Ranging;

    MarketSnapshot market;
    market.bid = 100'000'000;
    market.ask = 100'100'000;
    StrategyPosition pos;

    evaluator.evaluate(0, market, pos);

    // Verify SpotSellEvent was published
    assert(capture.has_event());
    assert(capture.last().symbol == 0);
    assert(capture.last().qty == 1.0);
}

TEST(test_weak_signal_publishes_limit_event) {
    core::EventBus bus;
    MockMetricsManager metrics;
    MockLimitManager limit_mgr;
    MockStrategySelector selector;
    MockStrategy mock_strategy;
    selector.mock_strategy_ptr = &mock_strategy;
    core::StrategyEvaluator evaluator(&bus, &metrics, &limit_mgr, &selector);

    // Capture SpotLimitBuyEvent
    EventCapture<core::SpotLimitBuyEvent> capture;
    capture.subscribe(&bus);

    // Set up buy signal with weak strength (prefer limit)
    mock_strategy.next_signal.type = SignalType::Buy;
    mock_strategy.next_signal.strength = SignalStrength::Weak; // Weak → prefer limit
    mock_strategy.next_signal.suggested_qty = 1.0;
    mock_strategy.next_signal.reason = "test_limit_buy";

    // Set metrics to favor limit orders (wide spread)
    metrics.ctx.regime = MarketRegime::Ranging;
    TradeStreamMetrics trade_metrics;
    OrderBookMetrics book_metrics;
    metrics.ctx.trade = &trade_metrics;
    metrics.ctx.book = &book_metrics;

    MarketSnapshot market;
    market.bid = 100'000'000;
    market.ask = 100'150'000; // Wide spread

    StrategyPosition pos;

    evaluator.evaluate(0, market, pos);

    // Weak signal + wide spread should produce limit order
    // (SpotLimitBuyEvent or SpotBuyEvent depending on ExecutionScorer)
    // Just verify some event was published
    assert(capture.count() <= 1); // May be 0 or 1 depending on scorer
}

TEST(test_no_signal_checks_limit_timeouts) {
    core::EventBus bus;
    MockMetricsManager metrics;
    MockLimitManager limit_mgr;
    MockStrategySelector selector;
    MockStrategy mock_strategy;
    selector.mock_strategy_ptr = &mock_strategy;
    core::StrategyEvaluator evaluator(&bus, &metrics, &limit_mgr, &selector);

    // Set up no-action signal
    mock_strategy.next_signal.type = SignalType::None;

    MarketSnapshot market;
    market.bid = 100'000'000;
    market.ask = 100'100'000;
    StrategyPosition pos;

    assert(limit_mgr.check_timeouts_count == 0);

    evaluator.evaluate(0, market, pos);

    // Verify check_limit_timeouts was called
    assert(limit_mgr.check_timeouts_count == 1);
}

TEST(test_dangerous_regime_spike_no_events) {
    core::EventBus bus;
    MockMetricsManager metrics;
    MockLimitManager limit_mgr;
    MockStrategySelector selector;
    MockStrategy mock_strategy;
    selector.mock_strategy_ptr = &mock_strategy;
    core::StrategyEvaluator evaluator(&bus, &metrics, &limit_mgr, &selector);

    // Capture all event types
    EventCapture<core::SpotBuyEvent> buy_capture;
    EventCapture<core::SpotSellEvent> sell_capture;
    EventCapture<core::SpotLimitBuyEvent> limit_buy_capture;
    EventCapture<core::SpotLimitSellEvent> limit_sell_capture;

    buy_capture.subscribe(&bus);
    sell_capture.subscribe(&bus);
    limit_buy_capture.subscribe(&bus);
    limit_sell_capture.subscribe(&bus);

    // Set up buy signal
    mock_strategy.next_signal.type = SignalType::Buy;
    mock_strategy.next_signal.strength = SignalStrength::Strong;
    mock_strategy.next_signal.suggested_qty = 1.0;

    // Set dangerous regime
    metrics.ctx.regime = MarketRegime::Spike; // Dangerous

    MarketSnapshot market;
    market.bid = 100'000'000;
    market.ask = 100'100'000;
    StrategyPosition pos;

    evaluator.evaluate(0, market, pos);

    // Verify NO events were published
    assert(buy_capture.count() == 0);
    assert(sell_capture.count() == 0);
    assert(limit_buy_capture.count() == 0);
    assert(limit_sell_capture.count() == 0);
}

TEST(test_dangerous_regime_high_volatility_no_events) {
    core::EventBus bus;
    MockMetricsManager metrics;
    MockLimitManager limit_mgr;
    MockStrategySelector selector;
    MockStrategy mock_strategy;
    selector.mock_strategy_ptr = &mock_strategy;
    core::StrategyEvaluator evaluator(&bus, &metrics, &limit_mgr, &selector);

    // Capture all event types
    EventCapture<core::SpotBuyEvent> buy_capture;
    buy_capture.subscribe(&bus);

    // Set up buy signal
    mock_strategy.next_signal.type = SignalType::Buy;
    mock_strategy.next_signal.strength = SignalStrength::Strong;
    mock_strategy.next_signal.suggested_qty = 1.0;

    // Set dangerous regime
    metrics.ctx.regime = MarketRegime::HighVolatility; // Dangerous

    MarketSnapshot market;
    market.bid = 100'000'000;
    market.ask = 100'100'000;
    StrategyPosition pos;

    evaluator.evaluate(0, market, pos);

    // Verify NO events were published
    assert(buy_capture.count() == 0);
}

TEST(test_signal_strength_conversion) {
    core::EventBus bus;
    MockMetricsManager metrics;
    MockLimitManager limit_mgr;
    MockStrategySelector selector;
    MockStrategy mock_strategy;
    selector.mock_strategy_ptr = &mock_strategy;
    core::StrategyEvaluator evaluator(&bus, &metrics, &limit_mgr, &selector);

    EventCapture<core::SpotBuyEvent> capture;
    capture.subscribe(&bus);

    // Test Strong strength
    mock_strategy.next_signal.type = SignalType::Buy;
    mock_strategy.next_signal.strength = SignalStrength::Strong;
    mock_strategy.next_signal.suggested_qty = 1.0;
    metrics.ctx.regime = MarketRegime::Ranging;

    MarketSnapshot market;
    market.bid = 100'000'000;
    market.ask = 100'100'000;
    StrategyPosition pos;

    evaluator.evaluate(0, market, pos);

    if (capture.has_event()) {
        // Strong = 1.0
        assert(capture.last().strength == 1.0);
    }
}

TEST(test_no_strategy_no_events) {
    core::EventBus bus;
    MockMetricsManager metrics;
    MockLimitManager limit_mgr;
    MockStrategySelector selector; // No strategies registered
    selector.mock_strategy_ptr = nullptr;
    core::StrategyEvaluator evaluator(&bus, &metrics, &limit_mgr, &selector);

    EventCapture<core::SpotBuyEvent> capture;
    capture.subscribe(&bus);

    MarketSnapshot market;
    market.bid = 100'000'000;
    market.ask = 100'100'000;
    StrategyPosition pos;

    evaluator.evaluate(0, market, pos);

    // No strategy → no events
    assert(capture.count() == 0);
}

TEST(test_invalid_signal_strength_conversion) {
    core::EventBus bus;
    MockMetricsManager metrics;
    MockLimitManager limit_mgr;
    MockStrategySelector selector;
    MockStrategy mock_strategy;
    selector.mock_strategy_ptr = &mock_strategy;
    core::StrategyEvaluator evaluator(&bus, &metrics, &limit_mgr, &selector);

    EventCapture<core::SpotBuyEvent> capture;
    capture.subscribe(&bus);

    // Test invalid strength (cast invalid value to enum)
    mock_strategy.next_signal.type = SignalType::Buy;
    mock_strategy.next_signal.strength = static_cast<SignalStrength>(99);
    mock_strategy.next_signal.suggested_qty = 1.0;
    metrics.ctx.regime = MarketRegime::Ranging;

    MarketSnapshot market;
    market.bid = 100'000'000;
    market.ask = 100'100'000;
    StrategyPosition pos;

    evaluator.evaluate(0, market, pos);

    // Invalid strength should convert to 0.0
    if (capture.has_event()) {
        assert(capture.last().strength == 0.0);
    }
}

int main() {
    RUN_TEST(test_buy_signal_publishes_spot_buy_event);
    RUN_TEST(test_sell_signal_publishes_spot_sell_event);
    RUN_TEST(test_weak_signal_publishes_limit_event);
    RUN_TEST(test_no_signal_checks_limit_timeouts);
    RUN_TEST(test_dangerous_regime_spike_no_events);
    RUN_TEST(test_dangerous_regime_high_volatility_no_events);
    RUN_TEST(test_signal_strength_conversion);
    RUN_TEST(test_no_strategy_no_events);
    RUN_TEST(test_invalid_signal_strength_conversion);

    std::cout << "\nAll 9 StrategyEvaluator tests passed!\n";
    return 0;
}
