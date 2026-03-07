#include "../include/core/event_bus.hpp"
#include "../include/core/events.hpp"

#include <cassert>
#include <iostream>
#include <vector>
#include <cstring>

using namespace hft;
using namespace hft::core;

#define TEST(name) void name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "Running " << #name << "... ";                                                                    \
        name();                                                                                                        \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

// ============================================================================
// Basic Subscription and Publishing
// ============================================================================

TEST(test_subscribe_and_publish_single_event) {
    EventBus bus;
    int received_count = 0;
    Symbol received_symbol = 0;
    Quantity received_qty = 0;

    bus.subscribe<SpotBuyEvent>([&](const SpotBuyEvent& e) {
        received_count++;
        received_symbol = e.symbol;
        received_qty = e.qty;
    });

    SpotBuyEvent event{.symbol = 42, .qty = 1000, .strength = 0.8, .reason = "test", .timestamp_ns = 123456};

    bus.publish(event);

    assert(received_count == 1);
    assert(received_symbol == 42);
    assert(received_qty == 1000);
}

TEST(test_publish_with_no_subscribers) {
    EventBus bus;
    SpotBuyEvent event{.symbol = 1, .qty = 100, .strength = 0.5, .reason = "test", .timestamp_ns = 1000};
    bus.publish(event); // Should not crash
}

// ============================================================================
// Multiple Subscribers
// ============================================================================

TEST(test_multiple_subscribers_same_event) {
    EventBus bus;
    int count1 = 0;
    int count2 = 0;
    int count3 = 0;

    bus.subscribe<SpotBuyEvent>([&](const SpotBuyEvent&) { count1++; });
    bus.subscribe<SpotBuyEvent>([&](const SpotBuyEvent&) { count2++; });
    bus.subscribe<SpotBuyEvent>([&](const SpotBuyEvent&) { count3++; });

    SpotBuyEvent event{.symbol = 1, .qty = 100, .strength = 0.5, .reason = "test", .timestamp_ns = 1000};

    bus.publish(event);

    assert(count1 == 1);
    assert(count2 == 1);
    assert(count3 == 1);
}

TEST(test_subscriber_execution_order) {
    EventBus bus;
    std::vector<int> execution_order;

    bus.subscribe<SpotBuyEvent>([&](const SpotBuyEvent&) { execution_order.push_back(1); });
    bus.subscribe<SpotBuyEvent>([&](const SpotBuyEvent&) { execution_order.push_back(2); });
    bus.subscribe<SpotBuyEvent>([&](const SpotBuyEvent&) { execution_order.push_back(3); });

    SpotBuyEvent event{.symbol = 1, .qty = 100, .strength = 0.5, .reason = "test", .timestamp_ns = 1000};

    bus.publish(event);

    assert(execution_order.size() == 3);
    assert(execution_order[0] == 1);
    assert(execution_order[1] == 2);
    assert(execution_order[2] == 3);
}

// ============================================================================
// Multiple Event Types
// ============================================================================

TEST(test_multiple_event_types) {
    EventBus bus;
    int buy_count = 0;
    int sell_count = 0;
    int cancel_count = 0;

    bus.subscribe<SpotBuyEvent>([&](const SpotBuyEvent&) { buy_count++; });
    bus.subscribe<SpotSellEvent>([&](const SpotSellEvent&) { sell_count++; });
    bus.subscribe<LimitCancelEvent>([&](const LimitCancelEvent&) { cancel_count++; });

    bus.publish(SpotBuyEvent{.symbol = 1, .qty = 100, .strength = 0.5, .reason = "buy", .timestamp_ns = 1000});
    bus.publish(SpotSellEvent{.symbol = 1, .qty = 50, .strength = 0.6, .reason = "sell", .timestamp_ns = 2000});
    bus.publish(LimitCancelEvent{.symbol = 1, .order_id = 123, .reason = "cancel", .timestamp_ns = 3000});

    assert(buy_count == 1);
    assert(sell_count == 1);
    assert(cancel_count == 1);
}

TEST(test_event_type_isolation) {
    EventBus bus;
    int buy_count = 0;
    int sell_count = 0;

    bus.subscribe<SpotBuyEvent>([&](const SpotBuyEvent&) { buy_count++; });
    bus.subscribe<SpotSellEvent>([&](const SpotSellEvent&) { sell_count++; });

    bus.publish(SpotBuyEvent{.symbol = 1, .qty = 100, .strength = 0.5, .reason = "test", .timestamp_ns = 1000});
    bus.publish(SpotBuyEvent{.symbol = 2, .qty = 200, .strength = 0.6, .reason = "test", .timestamp_ns = 2000});

    assert(buy_count == 2);
    assert(sell_count == 0);
}

// ============================================================================
// Subscriber Count
// ============================================================================

TEST(test_subscriber_count) {
    EventBus bus;

    assert(bus.subscriber_count<SpotBuyEvent>() == 0);

    bus.subscribe<SpotBuyEvent>([](const SpotBuyEvent&) {});
    assert(bus.subscriber_count<SpotBuyEvent>() == 1);

    bus.subscribe<SpotBuyEvent>([](const SpotBuyEvent&) {});
    assert(bus.subscriber_count<SpotBuyEvent>() == 2);

    bus.subscribe<SpotSellEvent>([](const SpotSellEvent&) {});
    assert(bus.subscriber_count<SpotBuyEvent>() == 2);
    assert(bus.subscriber_count<SpotSellEvent>() == 1);
}

// ============================================================================
// Clear
// ============================================================================

TEST(test_clear) {
    EventBus bus;
    int count = 0;

    bus.subscribe<SpotBuyEvent>([&](const SpotBuyEvent&) { count++; });
    bus.subscribe<SpotSellEvent>([&](const SpotSellEvent&) { count++; });

    bus.clear();

    assert(bus.subscriber_count<SpotBuyEvent>() == 0);
    assert(bus.subscriber_count<SpotSellEvent>() == 0);

    bus.publish(SpotBuyEvent{.symbol = 1, .qty = 100, .strength = 0.5, .reason = "test", .timestamp_ns = 1000});
    assert(count == 0);
}

// ============================================================================
// Event Data Integrity
// ============================================================================

TEST(test_event_data_integrity) {
    EventBus bus;
    SpotLimitBuyEvent received_event{};

    bus.subscribe<SpotLimitBuyEvent>([&](const SpotLimitBuyEvent& e) { received_event = e; });

    SpotLimitBuyEvent sent_event{.symbol = 99,
                                 .qty = 5000,
                                 .limit_price = 12345,
                                 .strength = 0.95,
                                 .exec_score = 0.75,
                                 .reason = "high_conviction",
                                 .timestamp_ns = 9876543210};

    bus.publish(sent_event);

    assert(received_event.symbol == 99);
    assert(received_event.qty == 5000);
    assert(received_event.limit_price == 12345);
    assert(received_event.strength == 0.95);
    assert(received_event.exec_score == 0.75);
    assert(std::strcmp(received_event.reason, "high_conviction") == 0);
    assert(received_event.timestamp_ns == 9876543210);
}

// ============================================================================
// Synchronous Execution
// ============================================================================

TEST(test_synchronous_execution) {
    EventBus bus;
    bool handler1_done = false;
    bool handler2_done = false;

    bus.subscribe<SpotBuyEvent>([&](const SpotBuyEvent&) {
        handler1_done = true;
        assert(!handler2_done); // Handler 2 should not have executed yet
    });

    bus.subscribe<SpotBuyEvent>([&](const SpotBuyEvent&) {
        assert(handler1_done); // Handler 1 should have executed
        handler2_done = true;
    });

    bus.publish(SpotBuyEvent{.symbol = 1, .qty = 100, .strength = 0.5, .reason = "test", .timestamp_ns = 1000});

    assert(handler1_done);
    assert(handler2_done);
}

int main() {
    RUN_TEST(test_subscribe_and_publish_single_event);
    RUN_TEST(test_publish_with_no_subscribers);
    RUN_TEST(test_multiple_subscribers_same_event);
    RUN_TEST(test_subscriber_execution_order);
    RUN_TEST(test_multiple_event_types);
    RUN_TEST(test_event_type_isolation);
    RUN_TEST(test_subscriber_count);
    RUN_TEST(test_clear);
    RUN_TEST(test_event_data_integrity);
    RUN_TEST(test_synchronous_execution);

    std::cout << "\nAll EventBus tests passed!\n";
    return 0;
}
