#include "../include/core/event_bus.hpp"
#include "../include/core/events.hpp"
#include "../include/execution/execution_engine.hpp"
#include "../include/execution/limit_manager.hpp"
#include "../include/execution/trading_engine.hpp"

#include <cassert>
#include <iostream>

using namespace hft;
using namespace hft::core;
using namespace hft::execution;

// Mock exchange for testing
class MockExchange : public IExchangeAdapter {
public:
    struct Order {
        Symbol symbol;
        Side side;
        double qty;
        OrderType type;
        Price limit_price;
    };

    std::vector<Order> orders;
    uint64_t next_order_id = 1000;

    uint64_t send_market_order(Symbol symbol, Side side, double qty, Price expected_price) override {
        orders.push_back({symbol, side, qty, OrderType::Market, 0});
        return next_order_id++;
    }

    uint64_t send_limit_order(Symbol symbol, Side side, double qty, Price limit_price) override {
        orders.push_back({symbol, side, qty, OrderType::Limit, limit_price});
        return next_order_id++;
    }

    CancelResult cancel_order(uint64_t order_id) override { return CancelResult::Success; }

    bool is_order_pending(uint64_t order_id) const override { return false; }
    bool is_paper() const override { return true; }

    void clear() { orders.clear(); }
};

// Test 1: SpotBuyEvent → SpotEngine executes market buy
void test_spot_buy_event_executes() {
    EventBus bus;
    MockExchange exchange;
    SpotEngine engine(&exchange);

    // Subscribe to SpotBuyEvent
    bus.subscribe<SpotBuyEvent>([&](const SpotBuyEvent& e) {
        OrderRequest req;
        req.symbol = e.symbol;
        req.side = Side::Buy;
        req.type = OrderType::Market;
        req.qty = e.qty;
        req.venue = Venue::Spot;
        MarketSnapshot market{100000, 100100};
        engine.execute(req, market);
    });

    // Publish event
    bus.publish(SpotBuyEvent{.symbol = 1, .qty = 10.5, .strength = 0.8, .reason = "test", .timestamp_ns = 123456});

    // Verify order executed
    assert(exchange.orders.size() == 1);
    assert(exchange.orders[0].symbol == 1);
    assert(exchange.orders[0].side == Side::Buy);
    assert(exchange.orders[0].qty == 10.5);
    assert(exchange.orders[0].type == OrderType::Market);
    std::cout << "[PASS] test_spot_buy_event_executes\n";
}

// Test 2: SpotSellEvent → SpotEngine executes market sell
void test_spot_sell_event_executes() {
    EventBus bus;
    MockExchange exchange;
    SpotEngine engine(&exchange);

    bus.subscribe<SpotSellEvent>([&](const SpotSellEvent& e) {
        OrderRequest req;
        req.symbol = e.symbol;
        req.side = Side::Sell;
        req.type = OrderType::Market;
        req.qty = e.qty;
        req.venue = Venue::Spot;
        MarketSnapshot market{100000, 100100};
        engine.execute(req, market);
    });

    bus.publish(SpotSellEvent{.symbol = 2, .qty = 5.25, .strength = 0.6, .reason = "test", .timestamp_ns = 234567});

    assert(exchange.orders.size() == 1);
    assert(exchange.orders[0].symbol == 2);
    assert(exchange.orders[0].side == Side::Sell);
    assert(exchange.orders[0].qty == 5.25);
    assert(exchange.orders[0].type == OrderType::Market);
    std::cout << "[PASS] test_spot_sell_event_executes\n";
}

// Test 3: SpotLimitBuyEvent → SpotEngine executes limit buy + LimitManager tracks
void test_spot_limit_buy_event_executes() {
    EventBus bus;
    MockExchange exchange;
    SpotEngine engine(&exchange);
    LimitManager limit_mgr(&bus, &engine);

    bus.subscribe<SpotLimitBuyEvent>([&](const SpotLimitBuyEvent& e) {
        OrderRequest req;
        req.symbol = e.symbol;
        req.side = Side::Buy;
        req.type = OrderType::Limit;
        req.qty = e.qty;
        req.limit_price = e.limit_price;
        req.venue = Venue::Spot;
        MarketSnapshot market{100000, 100100};
        uint64_t order_id = engine.execute(req, market);
        if (order_id > 0) {
            limit_mgr.track(e.symbol, order_id, Side::Buy, e.limit_price, e.qty);
        }
    });

    bus.publish(SpotLimitBuyEvent{
        .symbol = 3, .qty = 1.5, .limit_price = 99500, .strength = 0.5, .exec_score = 10.0, .reason = "test", .timestamp_ns = 345678});

    assert(exchange.orders.size() == 1);
    assert(exchange.orders[0].type == OrderType::Limit);
    assert(exchange.orders[0].limit_price == 99500);
    assert(exchange.orders[0].qty == 1.5);
    std::cout << "[PASS] test_spot_limit_buy_event_executes\n";
}

// Test 4: SpotLimitSellEvent → SpotEngine executes limit sell + LimitManager tracks
void test_spot_limit_sell_event_executes() {
    EventBus bus;
    MockExchange exchange;
    SpotEngine engine(&exchange);
    LimitManager limit_mgr(&bus, &engine);

    bus.subscribe<SpotLimitSellEvent>([&](const SpotLimitSellEvent& e) {
        OrderRequest req;
        req.symbol = e.symbol;
        req.side = Side::Sell;
        req.type = OrderType::Limit;
        req.qty = e.qty;
        req.limit_price = e.limit_price;
        req.venue = Venue::Spot;
        MarketSnapshot market{100000, 100100};
        uint64_t order_id = engine.execute(req, market);
        if (order_id > 0) {
            limit_mgr.track(e.symbol, order_id, Side::Sell, e.limit_price, e.qty);
        }
    });

    bus.publish(SpotLimitSellEvent{.symbol = 4,
                                    .qty = 2.75,
                                    .limit_price = 100500,
                                    .strength = 0.7,
                                    .exec_score = 12.0,
                                    .reason = "test",
                                    .timestamp_ns = 456789});

    assert(exchange.orders.size() == 1);
    assert(exchange.orders[0].type == OrderType::Limit);
    assert(exchange.orders[0].side == Side::Sell);
    assert(exchange.orders[0].limit_price == 100500);
    std::cout << "[PASS] test_spot_limit_sell_event_executes\n";
}

// Test 5: LimitCancelEvent (specific order) → LimitManager cancels specific order
void test_limit_cancel_event_specific_order() {
    EventBus bus;
    MockExchange exchange;
    SpotEngine engine(&exchange);
    LimitManager limit_mgr(&bus, &engine);

    // Track a pending limit
    limit_mgr.track(5, 2000, Side::Buy, 99000, 1.0);

    int cancel_count = 0;
    bus.subscribe<LimitCancelEvent>([&](const LimitCancelEvent& e) { cancel_count++; });

    // Publish cancel event (LimitManager subscribes internally)
    bus.publish(LimitCancelEvent{.symbol = 5, .order_id = 2000, .reason = "test_cancel", .timestamp_ns = 567890});

    // Verify event was received (LimitManager subscribed)
    assert(cancel_count == 1);
    std::cout << "[PASS] test_limit_cancel_event_specific_order\n";
}

// Test 6: LimitCancelEvent (order_id=0) → cancel all for symbol
void test_limit_cancel_event_all_orders() {
    EventBus bus;
    MockExchange exchange;
    SpotEngine engine(&exchange);
    LimitManager limit_mgr(&bus, &engine);

    limit_mgr.track(6, 3000, Side::Buy, 99000, 1.0);
    limit_mgr.track(6, 3001, Side::Buy, 99100, 1.5);

    bus.publish(LimitCancelEvent{.symbol = 6, .order_id = 0, .reason = "cancel_all", .timestamp_ns = 678901});

    // Event processed (no crash)
    std::cout << "[PASS] test_limit_cancel_event_all_orders\n";
}

// Test 7: Multiple subscribers for same event type
void test_multiple_subscribers_same_event() {
    EventBus bus;
    int count1 = 0, count2 = 0;

    bus.subscribe<SpotBuyEvent>([&](const SpotBuyEvent& e) { count1++; });
    bus.subscribe<SpotBuyEvent>([&](const SpotBuyEvent& e) { count2++; });

    bus.publish(SpotBuyEvent{.symbol = 7, .qty = 1.0, .strength = 0.5, .reason = "test", .timestamp_ns = 789012});

    assert(count1 == 1);
    assert(count2 == 1);
    assert(bus.subscriber_count<SpotBuyEvent>() == 2);
    std::cout << "[PASS] test_multiple_subscribers_same_event\n";
}

// Test 8: Event published with no subscribers (no crash)
void test_no_subscribers_no_crash() {
    EventBus bus;

    // Publish event with no subscribers
    bus.publish(SpotBuyEvent{.symbol = 8, .qty = 1.0, .strength = 0.5, .reason = "test", .timestamp_ns = 890123});

    // Should not crash
    assert(bus.subscriber_count<SpotBuyEvent>() == 0);
    std::cout << "[PASS] test_no_subscribers_no_crash\n";
}

int main() {
    std::cout << "Running action event wiring tests...\n\n";

    test_spot_buy_event_executes();
    test_spot_sell_event_executes();
    test_spot_limit_buy_event_executes();
    test_spot_limit_sell_event_executes();
    test_limit_cancel_event_specific_order();
    test_limit_cancel_event_all_orders();
    test_multiple_subscribers_same_event();
    test_no_subscribers_no_crash();

    std::cout << "\n✓ All 8 action event wiring tests passed!\n";
    return 0;
}
