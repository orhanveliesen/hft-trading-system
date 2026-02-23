#include "../include/feed_handler.hpp"
#include "../include/itch_messages.hpp"
#include "../include/types.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

using namespace hft;

#define TEST(name) void name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "Running " << #name << "... ";                                                                    \
        name();                                                                                                        \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

#define ASSERT_EQ(a, b) assert((a) == (b))
#define ASSERT_TRUE(x) assert(x)

// Helper: Create big-endian uint16
void write_be16(uint8_t* buf, uint16_t val) {
    buf[0] = (val >> 8) & 0xFF;
    buf[1] = val & 0xFF;
}

// Helper: Create big-endian uint32
void write_be32(uint8_t* buf, uint32_t val) {
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8) & 0xFF;
    buf[3] = val & 0xFF;
}

// Helper: Create big-endian uint64
void write_be64(uint8_t* buf, uint64_t val) {
    for (int i = 7; i >= 0; --i) {
        buf[7 - i] = (val >> (i * 8)) & 0xFF;
    }
}

// Captured order event (generic, exchange-agnostic)
struct OrderEvent {
    OrderId order_id;
    Side side;
    Price price;
    Quantity qty;
};

// Test callback to capture parsed messages (uses generic interface)
struct TestCallback {
    std::vector<OrderEvent> add_orders;
    std::vector<std::pair<OrderId, Quantity>> executions;
    std::vector<std::pair<OrderId, Quantity>> cancels;
    std::vector<OrderId> deletes;

    void on_add_order(OrderId id, Side side, Price price, Quantity qty) {
        add_orders.push_back({id, side, price, qty});
    }

    void on_order_executed(OrderId id, Quantity qty) { executions.push_back({id, qty}); }

    void on_order_cancelled(OrderId id, Quantity qty) { cancels.push_back({id, qty}); }

    void on_order_deleted(OrderId id) { deletes.push_back(id); }
};

// Build ITCH Add Order message (Type 'A')
// Format: type(1) + locate(2) + tracking(2) + timestamp(6) + ref(8) + side(1) + shares(4) + stock(8) + price(4)
std::vector<uint8_t> build_add_order(uint64_t order_ref, char side, uint32_t shares, uint32_t price) {
    std::vector<uint8_t> msg(36);
    msg[0] = 'A';           // Message type
    write_be16(&msg[1], 1); // Stock locate
    write_be16(&msg[3], 0); // Tracking number
    // Timestamp (6 bytes) - leave as 0
    write_be64(&msg[11], order_ref);      // Order reference
    msg[19] = side;                       // Buy/Sell indicator
    write_be32(&msg[20], shares);         // Shares
    std::memcpy(&msg[24], "AAPL    ", 8); // Stock symbol (8 chars, space padded)
    write_be32(&msg[32], price);          // Price (4 decimal places)
    return msg;
}

// Build ITCH Order Executed message (Type 'E')
// Format: type(1) + locate(2) + tracking(2) + timestamp(6) + ref(8) + shares(4) + match(8)
std::vector<uint8_t> build_order_executed(uint64_t order_ref, uint32_t shares) {
    std::vector<uint8_t> msg(31);
    msg[0] = 'E';           // Message type
    write_be16(&msg[1], 1); // Stock locate
    write_be16(&msg[3], 0); // Tracking number
    // Timestamp (6 bytes) - leave as 0
    write_be64(&msg[11], order_ref); // Order reference
    write_be32(&msg[19], shares);    // Executed shares
    write_be64(&msg[23], 12345);     // Match number
    return msg;
}

// Build ITCH Order Cancel message (Type 'X')
// Format: type(1) + locate(2) + tracking(2) + timestamp(6) + ref(8) + shares(4)
std::vector<uint8_t> build_order_cancel(uint64_t order_ref, uint32_t shares) {
    std::vector<uint8_t> msg(23);
    msg[0] = 'X';           // Message type
    write_be16(&msg[1], 1); // Stock locate
    write_be16(&msg[3], 0); // Tracking number
    // Timestamp (6 bytes) - leave as 0
    write_be64(&msg[11], order_ref); // Order reference
    write_be32(&msg[19], shares);    // Cancelled shares
    return msg;
}

// Build ITCH Order Delete message (Type 'D')
// Format: type(1) + locate(2) + tracking(2) + timestamp(6) + ref(8)
std::vector<uint8_t> build_order_delete(uint64_t order_ref) {
    std::vector<uint8_t> msg(19);
    msg[0] = 'D';           // Message type
    write_be16(&msg[1], 1); // Stock locate
    write_be16(&msg[3], 0); // Tracking number
    // Timestamp (6 bytes) - leave as 0
    write_be64(&msg[11], order_ref); // Order reference
    return msg;
}

// Test: Parse Add Order message
TEST(test_parse_add_order) {
    TestCallback callback;
    FeedHandler<TestCallback> handler(callback);

    auto msg = build_add_order(1001, 'B', 100, 150000); // Buy 100 @ $15.00
    handler.process_message(msg.data(), msg.size());

    ASSERT_EQ(callback.add_orders.size(), 1);
    ASSERT_EQ(callback.add_orders[0].order_id, 1001);
    ASSERT_EQ(callback.add_orders[0].side, Side::Buy);
    ASSERT_EQ(callback.add_orders[0].qty, 100);
    ASSERT_EQ(callback.add_orders[0].price, 150000);
}

// Test: Parse Order Executed message
TEST(test_parse_order_executed) {
    TestCallback callback;
    FeedHandler<TestCallback> handler(callback);

    auto msg = build_order_executed(1001, 50);
    handler.process_message(msg.data(), msg.size());

    ASSERT_EQ(callback.executions.size(), 1);
    ASSERT_EQ(callback.executions[0].first, 1001);
    ASSERT_EQ(callback.executions[0].second, 50);
}

// Test: Parse Order Cancel message
TEST(test_parse_order_cancel) {
    TestCallback callback;
    FeedHandler<TestCallback> handler(callback);

    auto msg = build_order_cancel(1001, 30);
    handler.process_message(msg.data(), msg.size());

    ASSERT_EQ(callback.cancels.size(), 1);
    ASSERT_EQ(callback.cancels[0].first, 1001);
    ASSERT_EQ(callback.cancels[0].second, 30);
}

// Test: Parse Order Delete message
TEST(test_parse_order_delete) {
    TestCallback callback;
    FeedHandler<TestCallback> handler(callback);

    auto msg = build_order_delete(1001);
    handler.process_message(msg.data(), msg.size());

    ASSERT_EQ(callback.deletes.size(), 1);
    ASSERT_EQ(callback.deletes[0], 1001);
}

// Test: Multiple messages in sequence
TEST(test_multiple_messages) {
    TestCallback callback;
    FeedHandler<TestCallback> handler(callback);

    handler.process_message(build_add_order(1, 'B', 100, 10000).data(), 36);
    handler.process_message(build_add_order(2, 'S', 200, 10100).data(), 36);
    handler.process_message(build_order_executed(1, 50).data(), 31);
    handler.process_message(build_order_delete(2).data(), 19);

    ASSERT_EQ(callback.add_orders.size(), 2);
    ASSERT_EQ(callback.executions.size(), 1);
    ASSERT_EQ(callback.deletes.size(), 1);
}

// Test: Unknown message type is ignored
TEST(test_unknown_message_ignored) {
    TestCallback callback;
    FeedHandler<TestCallback> handler(callback);

    std::vector<uint8_t> msg(20, 0);
    msg[0] = 'Z'; // Unknown type
    handler.process_message(msg.data(), msg.size());

    ASSERT_EQ(callback.add_orders.size(), 0);
    ASSERT_EQ(callback.executions.size(), 0);
}

int main() {
    std::cout << "=== Feed Handler Tests ===\n";

    RUN_TEST(test_parse_add_order);
    RUN_TEST(test_parse_order_executed);
    RUN_TEST(test_parse_order_cancel);
    RUN_TEST(test_parse_order_delete);
    RUN_TEST(test_multiple_messages);
    RUN_TEST(test_unknown_message_ignored);

    std::cout << "\nAll tests PASSED!\n";
    return 0;
}
