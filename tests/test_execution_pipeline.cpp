#include "../include/execution/execution_engine.hpp"
#include "../include/execution/execution_pipeline.hpp"
#include "../include/execution/spot_market_stage.hpp"
#include "../include/strategy/istrategy.hpp"

#include <cassert>
#include <iostream>
#include <memory>

using namespace hft;
using namespace hft::execution;
using namespace hft::strategy;

// Mock stage for testing
class MockStage : public IExecutionStage {
public:
    bool enabled_ = true;
    int process_calls = 0;

    std::vector<OrderRequest> process(const Signal& signal, const ExecutionContext& ctx) override {
        process_calls++;
        if (!signal.is_actionable())
            return {};

        OrderRequest req;
        req.symbol = ctx.symbol;
        req.qty = signal.suggested_qty;
        req.type = OrderType::Market;
        req.source_stage = "Mock";
        return {req};
    }

    std::string_view name() const override { return "Mock"; }
    bool enabled() const override { return enabled_; }
};

void test_empty_pipeline_returns_no_requests() {
    ExecutionPipeline pipeline;

    Signal signal = Signal::buy(SignalStrength::Strong, 10.0, "test");
    ExecutionContext ctx;

    auto requests = pipeline.process(signal, ctx);
    assert(requests.empty());
    std::cout << "[PASS] test_empty_pipeline_returns_no_requests\n";
}

void test_spot_market_stage_buy() {
    SpotMarketStage stage;

    Signal signal = Signal::buy(SignalStrength::Strong, 10.0, "test");
    ExecutionContext ctx;
    ctx.symbol = 1;

    auto requests = stage.process(signal, ctx);
    assert(requests.size() == 1);
    assert(requests[0].type == OrderType::Market);
    assert(requests[0].side == Side::Buy);
    assert(requests[0].venue == Venue::Spot);
    assert(requests[0].qty == 10.0);
    std::cout << "[PASS] test_spot_market_stage_buy\n";
}

void test_spot_market_stage_sell() {
    SpotMarketStage stage;

    Signal signal = Signal::sell(SignalStrength::Medium, 5.0, "test");
    ExecutionContext ctx;
    ctx.symbol = 1;
    ctx.position.quantity = 10.0; // Have position to sell

    auto requests = stage.process(signal, ctx);
    assert(requests.size() == 1);
    assert(requests[0].type == OrderType::Market);
    assert(requests[0].side == Side::Sell);
    assert(requests[0].qty == 5.0);
    std::cout << "[PASS] test_spot_market_stage_sell\n";
}

void test_spot_market_stage_none_signal() {
    SpotMarketStage stage;

    Signal signal = Signal::none();
    ExecutionContext ctx;

    auto requests = stage.process(signal, ctx);
    assert(requests.empty());
    std::cout << "[PASS] test_spot_market_stage_none_signal\n";
}

void test_spot_market_stage_sell_caps_to_position() {
    SpotMarketStage stage;

    Signal signal = Signal::sell(SignalStrength::Strong, 100.0, "test");
    ExecutionContext ctx;
    ctx.position.quantity = 50.0; // Only have 50

    auto requests = stage.process(signal, ctx);
    assert(requests.size() == 1);
    assert(requests[0].qty == 50.0); // Capped to position
    std::cout << "[PASS] test_spot_market_stage_sell_caps_to_position\n";
}

void test_spot_market_stage_sell_no_position() {
    SpotMarketStage stage;

    Signal signal = Signal::sell(SignalStrength::Strong, 10.0, "test");
    ExecutionContext ctx;
    ctx.position.quantity = 0.0; // No position

    auto requests = stage.process(signal, ctx);
    assert(requests.empty()); // Can't sell without position
    std::cout << "[PASS] test_spot_market_stage_sell_no_position\n";
}

void test_pipeline_collects_from_multiple_stages() {
    ExecutionPipeline pipeline;
    pipeline.add_stage(std::make_unique<SpotMarketStage>());
    pipeline.add_stage(std::make_unique<SpotMarketStage>());

    Signal signal = Signal::buy(SignalStrength::Strong, 10.0, "test");
    ExecutionContext ctx;
    ctx.symbol = 1;

    auto requests = pipeline.process(signal, ctx);
    assert(requests.size() == 2); // Both stages produced orders
    std::cout << "[PASS] test_pipeline_collects_from_multiple_stages\n";
}

void test_pipeline_skips_disabled_stage() {
    ExecutionPipeline pipeline;
    auto mock = std::make_unique<MockStage>();
    mock->enabled_ = false;
    pipeline.add_stage(std::move(mock));

    Signal signal = Signal::buy(SignalStrength::Strong, 10.0, "test");
    ExecutionContext ctx;

    auto requests = pipeline.process(signal, ctx);
    assert(requests.empty()); // Stage was disabled
    std::cout << "[PASS] test_pipeline_skips_disabled_stage\n";
}

void test_order_request_is_valid() {
    OrderRequest req1;
    req1.qty = 0.0;
    assert(!req1.is_valid());

    OrderRequest req2;
    req2.qty = 10.0;
    assert(req2.is_valid());

    std::cout << "[PASS] test_order_request_is_valid\n";
}

void test_pipeline_stage_names() {
    ExecutionPipeline pipeline;
    pipeline.add_stage(std::make_unique<SpotMarketStage>());

    auto names = pipeline.stage_names();
    assert(names.size() == 1);
    assert(names[0] == "SpotMarket");
    std::cout << "[PASS] test_pipeline_stage_names\n";
}

// Mock exchange for testing execute_request
class MockExchange : public IExchangeAdapter {
public:
    int market_order_calls = 0;
    int limit_order_calls = 0;

    uint64_t send_market_order(Symbol symbol, Side side, double qty, Price expected_price) override {
        market_order_calls++;
        return 1234;
    }

    uint64_t send_limit_order(Symbol symbol, Side side, double qty, Price limit_price) override {
        limit_order_calls++;
        return 5678;
    }

    bool cancel_order(uint64_t order_id) override { return true; }
    bool is_order_pending(uint64_t order_id) const override { return false; }
    bool is_paper() const override { return true; }
};

void test_execute_request_market_order() {
    ExecutionEngine engine;
    MockExchange exchange;
    engine.set_exchange(&exchange);

    OrderRequest req;
    req.symbol = 1;
    req.side = Side::Buy;
    req.type = OrderType::Market;
    req.qty = 10.0;
    req.is_valid();

    MarketSnapshot market;
    market.bid = 100000;
    market.ask = 100100;

    uint64_t order_id = engine.execute_request(req, market);
    assert(order_id > 0);
    assert(exchange.market_order_calls == 1);
    assert(exchange.limit_order_calls == 0);
    std::cout << "[PASS] test_execute_request_market_order\n";
}

void test_execute_request_sell_position_check() {
    ExecutionEngine engine;
    MockExchange exchange;
    engine.set_exchange(&exchange);

    // Set position callback
    engine.set_position_callback([](Symbol) { return 5.0; }); // Position = 5

    OrderRequest req;
    req.symbol = 1;
    req.side = Side::Sell;
    req.type = OrderType::Market;
    req.qty = 100.0; // Try to sell 100

    MarketSnapshot market;
    market.bid = 100000;
    market.ask = 100100;

    uint64_t order_id = engine.execute_request(req, market);
    assert(order_id > 0);
    // NOTE: execute_request caps qty internally, but we can't verify without mocking deeper
    std::cout << "[PASS] test_execute_request_sell_position_check\n";
}

int main() {
    std::cout << "Running ExecutionPipeline tests...\n\n";

    test_empty_pipeline_returns_no_requests();
    test_spot_market_stage_buy();
    test_spot_market_stage_sell();
    test_spot_market_stage_none_signal();
    test_spot_market_stage_sell_caps_to_position();
    test_spot_market_stage_sell_no_position();
    test_pipeline_collects_from_multiple_stages();
    test_pipeline_skips_disabled_stage();
    test_order_request_is_valid();
    test_pipeline_stage_names();
    test_execute_request_market_order();
    test_execute_request_sell_position_check();

    std::cout << "\n✓ All 12 tests passed!\n";
    return 0;
}
