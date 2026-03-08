#include "../include/execution/limit_manager.hpp"

#include <cassert>
#include <iostream>
#include <thread>

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
// Mock Exchange and Engine
// ============================================================================

class MockExchange : public IExchangeAdapter {
public:
    uint64_t next_order_id = 1;
    uint64_t last_cancelled_order_id = 0;
    int cancel_count = 0;

    uint64_t send_market_order(Symbol, Side, double, Price) override { return next_order_id++; }

    uint64_t send_limit_order(Symbol, Side, double, Price) override { return next_order_id++; }

    CancelResult cancel_order(uint64_t order_id) override {
        last_cancelled_order_id = order_id;
        cancel_count++;
        return CancelResult::Success;
    }

    bool is_order_pending(uint64_t) const override { return false; }

    bool is_paper() const override { return true; }

    void reset() {
        next_order_id = 1;
        last_cancelled_order_id = 0;
        cancel_count = 0;
    }
};

// ============================================================================
// Tests
// ============================================================================

TEST(test_track_pending_limit) {
    core::EventBus bus;
    MockExchange exchange;
    SpotEngine engine(&exchange);
    LimitManager mgr(&bus, &engine);

    mgr.track(0, 12345, Side::Buy, 100'000'000, 1.0);

    auto* pending = mgr.get_pending(0);
    assert(pending != nullptr);
    assert(pending->order_id == 12345);
    assert(pending->symbol == 0);
    assert(pending->side == Side::Buy);
    assert(pending->limit_price == 100'000'000);
    assert(pending->qty == 1.0);
    assert(pending->active);
}

TEST(test_timeout_publishes_cancel_event) {
    core::EventBus bus;
    MockExchange exchange;
    SpotEngine engine(&exchange);
    LimitManager mgr(&bus, &engine);

    // Set short timeout for testing (100ms)
    mgr.set_timeout_ns(100'000'000);

    // Track a pending limit
    mgr.track(0, 12345, Side::Buy, 100'000'000, 1.0);
    assert(mgr.pending_count() == 1);

    // Subscribe to LimitCancelEvent to verify it's published
    bool event_received = false;
    uint64_t cancelled_order_id = 0;
    bus.subscribe<core::LimitCancelEvent>([&](const core::LimitCancelEvent& e) {
        event_received = true;
        cancelled_order_id = e.order_id;
    });

    // Wait for timeout (simulate 200ms passing)
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Check for timeouts
    mgr.check_timeouts();

    // Verify event was published
    assert(event_received);
    assert(cancelled_order_id == 12345);
}

TEST(test_cancel_specific_order) {
    core::EventBus bus;
    MockExchange exchange;
    SpotEngine engine(&exchange);
    LimitManager mgr(&bus, &engine);

    // Track a pending limit
    mgr.track(0, 12345, Side::Buy, 100'000'000, 1.0);
    assert(mgr.pending_count() == 1);

    // Publish cancel event for specific order
    bus.publish(
        core::LimitCancelEvent{.symbol = 0, .order_id = 12345, .reason = "test", .timestamp_ns = util::now_ns()});

    // Verify exchange received cancel
    assert(exchange.last_cancelled_order_id == 12345);
    assert(exchange.cancel_count == 1);
}

TEST(test_cancel_all_for_symbol) {
    core::EventBus bus;
    MockExchange exchange;
    SpotEngine engine(&exchange);
    LimitManager mgr(&bus, &engine);

    // Track a pending limit
    mgr.track(0, 12345, Side::Buy, 100'000'000, 1.0);
    assert(mgr.pending_count() == 1);

    // Publish cancel event with order_id == 0 (cancel all for symbol)
    bus.publish(
        core::LimitCancelEvent{.symbol = 0, .order_id = 0, .reason = "cancel_all", .timestamp_ns = util::now_ns()});

    // Verify exchange received cancel
    assert(exchange.last_cancelled_order_id == 12345);
    assert(exchange.cancel_count == 1);
}

TEST(test_on_fill_clears_pending) {
    core::EventBus bus;
    MockExchange exchange;
    SpotEngine engine(&exchange);
    LimitManager mgr(&bus, &engine);

    // Track a pending limit
    mgr.track(0, 12345, Side::Buy, 100'000'000, 1.0);
    assert(mgr.pending_count() == 1);

    // Simulate fill
    mgr.on_fill(12345);

    // Verify pending is cleared
    assert(mgr.pending_count() == 0);
    auto* pending = mgr.get_pending(0);
    assert(pending == nullptr);
}

TEST(test_multiple_symbols) {
    core::EventBus bus;
    MockExchange exchange;
    SpotEngine engine(&exchange);
    LimitManager mgr(&bus, &engine);

    // Track limits for multiple symbols
    mgr.track(0, 100, Side::Buy, 100'000'000, 1.0);
    mgr.track(1, 200, Side::Sell, 200'000'000, 2.0);
    mgr.track(2, 300, Side::Buy, 300'000'000, 3.0);

    assert(mgr.pending_count() == 3);

    // Fill order for symbol 1
    mgr.on_fill(200);

    assert(mgr.pending_count() == 2);

    // Verify correct orders remain
    auto* p0 = mgr.get_pending(0);
    auto* p1 = mgr.get_pending(1);
    auto* p2 = mgr.get_pending(2);

    assert(p0 != nullptr && p0->order_id == 100);
    assert(p1 == nullptr); // Filled
    assert(p2 != nullptr && p2->order_id == 300);
}

TEST(test_no_timeout_before_threshold) {
    core::EventBus bus;
    MockExchange exchange;
    SpotEngine engine(&exchange);
    LimitManager mgr(&bus, &engine);

    // Set timeout to 5 seconds
    mgr.set_timeout_ns(5'000'000'000);

    // Track a pending limit
    mgr.track(0, 12345, Side::Buy, 100'000'000, 1.0);
    assert(mgr.pending_count() == 1);

    // Check timeouts immediately (should not timeout)
    mgr.check_timeouts();

    // Verify still pending
    assert(mgr.pending_count() == 1);
    assert(exchange.cancel_count == 0);
}

TEST(test_event_subscription_in_constructor) {
    core::EventBus bus;
    MockExchange exchange;
    SpotEngine engine(&exchange);

    // LimitManager constructor subscribes to LimitCancelEvent
    LimitManager mgr(&bus, &engine);

    // Track a pending limit
    mgr.track(0, 12345, Side::Buy, 100'000'000, 1.0);

    // Publish cancel event BEFORE check_timeouts
    bus.publish(
        core::LimitCancelEvent{.symbol = 0, .order_id = 12345, .reason = "test", .timestamp_ns = util::now_ns()});

    // Verify subscription worked (cancel was executed)
    assert(exchange.last_cancelled_order_id == 12345);
}

int main() {
    RUN_TEST(test_track_pending_limit);
    RUN_TEST(test_timeout_publishes_cancel_event);
    RUN_TEST(test_cancel_specific_order);
    RUN_TEST(test_cancel_all_for_symbol);
    RUN_TEST(test_on_fill_clears_pending);
    RUN_TEST(test_multiple_symbols);
    RUN_TEST(test_no_timeout_before_threshold);
    RUN_TEST(test_event_subscription_in_constructor);

    std::cout << "\nAll LimitManager tests passed!\n";
    return 0;
}
