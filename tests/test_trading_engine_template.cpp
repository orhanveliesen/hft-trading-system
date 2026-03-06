#include "../include/execution/trading_engine.hpp"

#include <cassert>
#include <iostream>

using namespace hft;
using namespace hft::execution;
using namespace hft::strategy;

#define TEST(name) void name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "Running " << #name << "... ";                                                                    \
        name();                                                                                                        \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

// ============================================================================
// Mock Exchange Adapter
// ============================================================================

class MockExchange : public IExchangeAdapter {
public:
    uint64_t next_order_id = 1;
    uint64_t last_market_order_id = 0;
    uint64_t last_limit_order_id = 0;
    Symbol last_symbol = 0;
    Side last_side = Side::Buy;
    double last_qty = 0.0;
    Price last_limit_price = 0;

    bool fail_cancel = false;
    CancelResult cancel_result = CancelResult::Success;
    bool order_pending_result = false;

    uint64_t send_market_order(Symbol symbol, Side side, double qty, Price expected_price) override {
        (void)expected_price;
        last_symbol = symbol;
        last_side = side;
        last_qty = qty;
        last_market_order_id = next_order_id;
        return next_order_id++;
    }

    uint64_t send_limit_order(Symbol symbol, Side side, double qty, Price limit_price) override {
        last_symbol = symbol;
        last_side = side;
        last_qty = qty;
        last_limit_price = limit_price;
        last_limit_order_id = next_order_id;
        return next_order_id++;
    }

    CancelResult cancel_order(uint64_t order_id) override {
        (void)order_id;
        if (fail_cancel) {
            return cancel_result;
        }
        return CancelResult::Success;
    }

    bool is_order_pending(uint64_t order_id) const override {
        (void)order_id;
        return order_pending_result;
    }

    bool is_paper() const override { return true; }

    void reset() {
        next_order_id = 1;
        last_market_order_id = 0;
        last_limit_order_id = 0;
        last_symbol = 0;
        last_side = Side::Buy;
        last_qty = 0.0;
        last_limit_price = 0;
        fail_cancel = false;
        cancel_result = CancelResult::Success;
        order_pending_result = false;
    }
};

// ============================================================================
// SpotEngine Tests
// ============================================================================

TEST(test_spot_engine_market_order) {
    MockExchange exchange;
    SpotEngine engine(&exchange);

    MarketSnapshot market;
    market.bid = 100'000'000; // 100.00
    market.ask = 100'100'000; // 100.10

    OrderRequest req;
    req.symbol = 0;
    req.side = Side::Buy;
    req.type = OrderType::Market;
    req.qty = 1.0;
    req.venue = Venue::Spot;

    uint64_t order_id = engine.execute(req, market);

    assert(order_id > 0);
    assert(exchange.last_symbol == 0);
    assert(exchange.last_side == Side::Buy);
    assert(exchange.last_qty == 1.0);
}

TEST(test_spot_engine_limit_order) {
    MockExchange exchange;
    SpotEngine engine(&exchange);

    MarketSnapshot market;
    market.bid = 100'000'000;
    market.ask = 100'100'000;

    OrderRequest req;
    req.symbol = 0;
    req.side = Side::Buy;
    req.type = OrderType::Limit;
    req.qty = 1.0;
    req.limit_price = 100'050'000; // 100.05
    req.venue = Venue::Spot;

    uint64_t order_id = engine.execute(req, market);

    assert(order_id > 0);
    assert(exchange.last_limit_price == 100'050'000);
    assert(engine.pending_order_count() == 1);
}

TEST(test_spot_engine_rejects_sell_with_no_position) {
    MockExchange exchange;
    SpotEngine engine(&exchange);

    // Set position callback that returns 0 (no position)
    engine.set_position_callback([](Symbol) { return 0.0; });

    MarketSnapshot market;
    market.bid = 100'000'000;
    market.ask = 100'100'000;

    OrderRequest req;
    req.symbol = 0;
    req.side = Side::Sell;
    req.type = OrderType::Market;
    req.qty = 1.0;
    req.venue = Venue::Spot;

    uint64_t order_id = engine.execute(req, market);

    // Should reject (no position to sell)
    assert(order_id == 0);
    assert(exchange.last_market_order_id == 0); // No order sent
}

TEST(test_spot_engine_allows_sell_with_position) {
    MockExchange exchange;
    SpotEngine engine(&exchange);

    // Set position callback that returns 10.0
    engine.set_position_callback([](Symbol) { return 10.0; });

    MarketSnapshot market;
    market.bid = 100'000'000;
    market.ask = 100'100'000;

    OrderRequest req;
    req.symbol = 0;
    req.side = Side::Sell;
    req.type = OrderType::Market;
    req.qty = 5.0;
    req.venue = Venue::Spot;

    uint64_t order_id = engine.execute(req, market);

    // Should allow (has position)
    assert(order_id > 0);
    assert(exchange.last_qty == 5.0);
}

TEST(test_spot_engine_limits_sell_qty_to_position) {
    MockExchange exchange;
    SpotEngine engine(&exchange);

    // Set position callback that returns 3.0
    engine.set_position_callback([](Symbol) { return 3.0; });

    MarketSnapshot market;
    market.bid = 100'000'000;
    market.ask = 100'100'000;

    OrderRequest req;
    req.symbol = 0;
    req.side = Side::Sell;
    req.type = OrderType::Market;
    req.qty = 10.0; // Request more than available
    req.venue = Venue::Spot;

    uint64_t order_id = engine.execute(req, market);

    // Should allow but limit qty to 3.0
    assert(order_id > 0);
    assert(exchange.last_qty == 3.0); // Limited to position
}

// ============================================================================
// FuturesEngine Tests
// ============================================================================

TEST(test_futures_engine_allows_sell_without_position) {
    MockExchange exchange;
    FuturesEngine engine(&exchange);

    // Set position callback that returns 0 (no position)
    engine.set_position_callback([](Symbol) { return 0.0; });

    MarketSnapshot market;
    market.bid = 100'000'000;
    market.ask = 100'100'000;

    OrderRequest req;
    req.symbol = 0;
    req.side = Side::Sell;
    req.type = OrderType::Market;
    req.qty = 1.0;
    req.venue = Venue::Futures;

    uint64_t order_id = engine.execute(req, market);

    // Should ALLOW (futures can short)
    assert(order_id > 0);
    assert(exchange.last_qty == 1.0);
}

TEST(test_futures_engine_does_not_limit_qty) {
    MockExchange exchange;
    FuturesEngine engine(&exchange);

    // Set position callback that returns 3.0
    engine.set_position_callback([](Symbol) { return 3.0; });

    MarketSnapshot market;
    market.bid = 100'000'000;
    market.ask = 100'100'000;

    OrderRequest req;
    req.symbol = 0;
    req.side = Side::Sell;
    req.type = OrderType::Market;
    req.qty = 10.0; // Request more than position
    req.venue = Venue::Futures;

    uint64_t order_id = engine.execute(req, market);

    // Should allow full qty (no position limiting in futures)
    assert(order_id > 0);
    assert(exchange.last_qty == 10.0); // NOT limited
}

// ============================================================================
// Cancel and Recovery Tests
// ============================================================================

TEST(test_cancel_order) {
    MockExchange exchange;
    SpotEngine engine(&exchange);

    MarketSnapshot market;
    market.bid = 100'000'000;
    market.ask = 100'100'000;

    OrderRequest req;
    req.symbol = 0;
    req.side = Side::Buy;
    req.type = OrderType::Limit;
    req.qty = 1.0;
    req.limit_price = 100'050'000;
    req.venue = Venue::Spot;

    uint64_t order_id = engine.execute(req, market);
    assert(engine.pending_order_count() == 1);

    // Cancel the order
    auto result = engine.cancel_order(order_id);
    assert(result == CancelResult::Success);
    assert(engine.pending_order_count() == 0);
}

TEST(test_cancel_stale_orders) {
    MockExchange exchange;
    SpotEngine engine(&exchange);

    MarketSnapshot market;
    market.bid = 100'000'000;
    market.ask = 100'100'000;

    OrderRequest req;
    req.symbol = 0;
    req.side = Side::Buy;
    req.type = OrderType::Limit;
    req.qty = 1.0;
    req.limit_price = 100'050'000;
    req.venue = Venue::Spot;

    uint64_t order_id = engine.execute(req, market);
    assert(engine.pending_order_count() == 1);

    // Simulate time passing (6 seconds, > 5s timeout)
    uint64_t now = util::now_ns();
    uint64_t future = now + 6'000'000'000;

    engine.cancel_stale_orders(future);

    // Order should be cancelled
    assert(engine.pending_order_count() == 0);
}

// ============================================================================
// New Coverage Tests (targeting uncovered lines)
// ============================================================================

TEST(test_futures_engine_limit_order) {
    // Target: Lines 88-91 for FuturesEngine instantiation
    MockExchange exchange;
    FuturesEngine engine(&exchange);

    MarketSnapshot market;
    market.bid = 100'000'000;
    market.ask = 100'100'000;

    OrderRequest req;
    req.symbol = 0;
    req.side = Side::Buy;
    req.type = OrderType::Limit;
    req.qty = 1.0;
    req.limit_price = 100'050'000;
    req.venue = Venue::Futures;

    uint64_t order_id = engine.execute(req, market);

    // Should execute and track pending order
    assert(order_id > 0);
    assert(exchange.last_limit_price == 100'050'000);
    assert(engine.pending_order_count() == 1);
}

TEST(test_order_callback_invoked) {
    // Target: Lines 97-98 (on_order_ callback)
    MockExchange exchange;
    SpotEngine engine(&exchange);

    // Track callback invocations
    bool callback_invoked = false;
    uint64_t callback_order_id = 0;
    Symbol callback_symbol = 0;
    Side callback_side = Side::Buy;
    double callback_qty = 0.0;
    Price callback_price = 0;
    OrderType callback_type = OrderType::Market;

    engine.set_order_callback([&](uint64_t order_id, Symbol symbol, Side side, double qty, Price price, OrderType type) {
        callback_invoked = true;
        callback_order_id = order_id;
        callback_symbol = symbol;
        callback_side = side;
        callback_qty = qty;
        callback_price = price;
        callback_type = type;
    });

    MarketSnapshot market;
    market.bid = 100'000'000;
    market.ask = 100'100'000;

    // Test with market order
    OrderRequest req;
    req.symbol = 5;
    req.side = Side::Buy;
    req.type = OrderType::Market;
    req.qty = 2.5;
    req.venue = Venue::Spot;

    uint64_t order_id = engine.execute(req, market);

    // Callback should be invoked
    assert(callback_invoked);
    assert(callback_order_id == order_id);
    assert(callback_symbol == 5);
    assert(callback_side == Side::Buy);
    assert(callback_qty == 2.5);
    assert(callback_price == 100'100'000); // ask price for buy
    assert(callback_type == OrderType::Market);
}

TEST(test_order_callback_with_limit_order) {
    // Target: Lines 97-98 with limit order
    MockExchange exchange;
    SpotEngine engine(&exchange);

    bool callback_invoked = false;
    Price callback_price = 0;
    OrderType callback_type = OrderType::Market;

    engine.set_order_callback([&](uint64_t, Symbol, Side, double, Price price, OrderType type) {
        callback_invoked = true;
        callback_price = price;
        callback_type = type;
    });

    MarketSnapshot market;
    market.bid = 100'000'000;
    market.ask = 100'100'000;

    OrderRequest req;
    req.symbol = 0;
    req.side = Side::Buy;
    req.type = OrderType::Limit;
    req.qty = 1.0;
    req.limit_price = 100'050'000;
    req.venue = Venue::Spot;

    engine.execute(req, market);

    assert(callback_invoked);
    assert(callback_price == 100'050'000); // limit_price
    assert(callback_type == OrderType::Limit);
}

TEST(test_execute_with_invalid_request) {
    // Target: Line 58 (return 0 on invalid request)
    MockExchange exchange;
    SpotEngine engine(&exchange);

    MarketSnapshot market;
    market.bid = 100'000'000;
    market.ask = 100'100'000;

    // Create invalid request (qty = 0 makes it invalid)
    OrderRequest req;
    req.symbol = 0;
    req.side = Side::Buy;
    req.type = OrderType::Market;
    req.qty = 0.0; // Invalid!
    req.venue = Venue::Spot;

    uint64_t order_id = engine.execute(req, market);

    // Should reject
    assert(order_id == 0);
    assert(exchange.last_market_order_id == 0); // No order sent
}

TEST(test_execute_with_null_exchange) {
    // Target: Line 58 (return 0 on null exchange)
    SpotEngine engine(nullptr); // Null exchange

    MarketSnapshot market;
    market.bid = 100'000'000;
    market.ask = 100'100'000;

    OrderRequest req;
    req.symbol = 0;
    req.side = Side::Buy;
    req.type = OrderType::Market;
    req.qty = 1.0;
    req.venue = Venue::Spot;

    uint64_t order_id = engine.execute(req, market);

    // Should reject (null exchange)
    assert(order_id == 0);
}

TEST(test_cancel_with_null_exchange) {
    // Target: Line 112 (CancelResult::NetworkError on null exchange)
    SpotEngine engine(nullptr);

    auto result = engine.cancel_order(123);

    assert(result == CancelResult::NetworkError);
}

TEST(test_cancel_non_pending_order) {
    // Target: Line 123 (direct exchange cancel when not in pending list)
    MockExchange exchange;
    SpotEngine engine(&exchange);

    // Cancel an order that's not in pending list
    auto result = engine.cancel_order(999);

    // Should call exchange.cancel_order() directly
    assert(result == CancelResult::Success);
}

TEST(test_limit_order_with_default_price) {
    // Target: Line 88 (limit_price <= 0 case, use expected_price)
    MockExchange exchange;
    SpotEngine engine(&exchange);

    MarketSnapshot market;
    market.bid = 100'000'000;
    market.ask = 100'100'000;

    OrderRequest req;
    req.symbol = 0;
    req.side = Side::Buy;
    req.type = OrderType::Limit;
    req.qty = 1.0;
    req.limit_price = 0; // Not set, should use expected_price (ask)
    req.venue = Venue::Spot;

    uint64_t order_id = engine.execute(req, market);

    // Should use expected_price (ask for buy)
    assert(order_id > 0);
    assert(exchange.last_limit_price == 100'100'000); // ask price
}

int main() {
    RUN_TEST(test_spot_engine_market_order);
    RUN_TEST(test_spot_engine_limit_order);
    RUN_TEST(test_spot_engine_rejects_sell_with_no_position);
    RUN_TEST(test_spot_engine_allows_sell_with_position);
    RUN_TEST(test_spot_engine_limits_sell_qty_to_position);
    RUN_TEST(test_futures_engine_allows_sell_without_position);
    RUN_TEST(test_futures_engine_does_not_limit_qty);
    RUN_TEST(test_cancel_order);
    RUN_TEST(test_cancel_stale_orders);

    // New coverage tests
    RUN_TEST(test_futures_engine_limit_order);
    RUN_TEST(test_order_callback_invoked);
    RUN_TEST(test_order_callback_with_limit_order);
    RUN_TEST(test_execute_with_invalid_request);
    RUN_TEST(test_execute_with_null_exchange);
    RUN_TEST(test_cancel_with_null_exchange);
    RUN_TEST(test_cancel_non_pending_order);
    RUN_TEST(test_limit_order_with_default_price);

    std::cout << "\nAll TradingEngine template tests passed!\n";
    return 0;
}
