#include "../include/exchange/ouch_order_sender.hpp"
#include "../include/ouch/ouch_messages.hpp"
#include "../include/ouch/ouch_session.hpp"

#include <cassert>
#include <cstring>
#include <iostream>

using namespace hft;
using namespace hft::ouch;

#define TEST(name) void name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "  " << #name << "... ";                                                                          \
        name();                                                                                                        \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

#define ASSERT_EQ(a, b) assert((a) == (b))
#define ASSERT_NE(a, b) assert((a) != (b))
#define ASSERT_TRUE(x) assert(x)
#define ASSERT_FALSE(x) assert(!(x))

// ============================================
// Message Structure Tests
// ============================================

TEST(test_enter_order_size) {
    ASSERT_EQ(sizeof(EnterOrder), 48);
}

TEST(test_replace_order_size) {
    ASSERT_EQ(sizeof(ReplaceOrder), 47);
}

TEST(test_cancel_order_size) {
    ASSERT_EQ(sizeof(CancelOrder), 19);
}

TEST(test_modify_order_size) {
    ASSERT_EQ(sizeof(ModifyOrder), 20);
}

TEST(test_accepted_size) {
    ASSERT_EQ(sizeof(Accepted), 66);
}

TEST(test_executed_size) {
    ASSERT_EQ(sizeof(Executed), 40);
}

TEST(test_canceled_size) {
    ASSERT_EQ(sizeof(Canceled), 28);
}

TEST(test_rejected_size) {
    ASSERT_EQ(sizeof(Rejected), 24);
}

TEST(test_replaced_size) {
    ASSERT_EQ(sizeof(Replaced), 80);
}

// ============================================
// Big-Endian Encoding Tests
// ============================================

TEST(test_write_be16) {
    uint8_t buf[2] = {0, 0};
    write_be16(buf, 0x1234);
    ASSERT_EQ(buf[0], 0x12);
    ASSERT_EQ(buf[1], 0x34);
}

TEST(test_write_be32) {
    uint8_t buf[4] = {0, 0, 0, 0};
    write_be32(buf, 0x12345678);
    ASSERT_EQ(buf[0], 0x12);
    ASSERT_EQ(buf[1], 0x34);
    ASSERT_EQ(buf[2], 0x56);
    ASSERT_EQ(buf[3], 0x78);
}

TEST(test_write_be64) {
    uint8_t buf[8] = {0};
    write_be64(buf, 0x123456789ABCDEF0ULL);
    ASSERT_EQ(buf[0], 0x12);
    ASSERT_EQ(buf[1], 0x34);
    ASSERT_EQ(buf[2], 0x56);
    ASSERT_EQ(buf[3], 0x78);
    ASSERT_EQ(buf[4], 0x9A);
    ASSERT_EQ(buf[5], 0xBC);
    ASSERT_EQ(buf[6], 0xDE);
    ASSERT_EQ(buf[7], 0xF0);
}

TEST(test_read_be16) {
    uint8_t buf[2] = {0x12, 0x34};
    ASSERT_EQ(read_be16(buf), 0x1234);
}

TEST(test_read_be32) {
    uint8_t buf[4] = {0x12, 0x34, 0x56, 0x78};
    ASSERT_EQ(read_be32(buf), 0x12345678u);
}

TEST(test_read_be64) {
    uint8_t buf[8] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};
    ASSERT_EQ(read_be64(buf), 0x123456789ABCDEF0ULL);
}

// ============================================
// EnterOrder Message Tests
// ============================================

TEST(test_enter_order_init) {
    EnterOrder order;
    order.init();

    ASSERT_EQ(order.type, MSG_ENTER_ORDER);
    ASSERT_EQ(order.display, DISPLAY_VISIBLE);
    ASSERT_EQ(order.capacity, CAPACITY_AGENCY);
    ASSERT_EQ(order.intermarket_sweep, 'N');
    ASSERT_EQ(order.cross_type, 'N');
}

TEST(test_enter_order_set_token) {
    EnterOrder order;
    order.init();
    order.set_token("ABC123");

    // Token should be space-padded to 14 chars
    ASSERT_EQ(order.token[0], 'A');
    ASSERT_EQ(order.token[1], 'B');
    ASSERT_EQ(order.token[2], 'C');
    ASSERT_EQ(order.token[3], '1');
    ASSERT_EQ(order.token[4], '2');
    ASSERT_EQ(order.token[5], '3');
    ASSERT_EQ(order.token[6], ' ');
    ASSERT_EQ(order.token[13], ' ');
}

TEST(test_enter_order_set_stock) {
    EnterOrder order;
    order.init();
    order.set_stock("AAPL");

    ASSERT_EQ(order.stock[0], 'A');
    ASSERT_EQ(order.stock[1], 'A');
    ASSERT_EQ(order.stock[2], 'P');
    ASSERT_EQ(order.stock[3], 'L');
    ASSERT_EQ(order.stock[4], ' ');
    ASSERT_EQ(order.stock[7], ' ');
}

TEST(test_enter_order_set_quantity) {
    EnterOrder order;
    order.init();
    order.set_quantity(1000);

    // Quantity is stored in big-endian
    const uint8_t* qty_bytes = reinterpret_cast<const uint8_t*>(&order.quantity);
    ASSERT_EQ(read_be32(qty_bytes), 1000u);
}

TEST(test_enter_order_set_price) {
    EnterOrder order;
    order.init();
    order.set_price(1500000); // $150.0000 (4 decimals)

    const uint8_t* price_bytes = reinterpret_cast<const uint8_t*>(&order.price);
    ASSERT_EQ(read_be32(price_bytes), 1500000u);
}

TEST(test_enter_order_buy_side) {
    EnterOrder order;
    order.init();
    order.side = SIDE_BUY;

    ASSERT_EQ(order.side, 'B');
}

TEST(test_enter_order_sell_side) {
    EnterOrder order;
    order.init();
    order.side = SIDE_SELL;

    ASSERT_EQ(order.side, 'S');
}

// ============================================
// CancelOrder Message Tests
// ============================================

TEST(test_cancel_order_init) {
    CancelOrder cancel;
    cancel.init();

    ASSERT_EQ(cancel.type, MSG_CANCEL_ORDER);
}

TEST(test_cancel_order_full_cancel) {
    CancelOrder cancel;
    cancel.init();
    cancel.set_token("ORDER12345");
    cancel.set_quantity(0); // 0 = full cancel

    const uint8_t* qty_bytes = reinterpret_cast<const uint8_t*>(&cancel.quantity);
    ASSERT_EQ(read_be32(qty_bytes), 0u);
}

TEST(test_cancel_order_partial) {
    CancelOrder cancel;
    cancel.init();
    cancel.set_token("ORDER12345");
    cancel.set_quantity(500);

    const uint8_t* qty_bytes = reinterpret_cast<const uint8_t*>(&cancel.quantity);
    ASSERT_EQ(read_be32(qty_bytes), 500u);
}

// ============================================
// ReplaceOrder Message Tests
// ============================================

TEST(test_replace_order_init) {
    ReplaceOrder replace;
    replace.init();

    ASSERT_EQ(replace.type, MSG_REPLACE_ORDER);
    ASSERT_EQ(replace.display, DISPLAY_VISIBLE);
    ASSERT_EQ(replace.intermarket_sweep, 'N');
}

TEST(test_replace_order_tokens) {
    ReplaceOrder replace;
    replace.init();
    replace.set_existing_token("OLD_ORDER");
    replace.set_replacement_token("NEW_ORDER");

    ASSERT_EQ(std::strncmp(replace.existing_token, "OLD_ORDER", 9), 0);
    ASSERT_EQ(std::strncmp(replace.replacement_token, "NEW_ORDER", 9), 0);
}

// ============================================
// Response Message Parsing Tests
// ============================================

TEST(test_accepted_parse) {
    // Create a mock Accepted message buffer
    Accepted msg;
    std::memset(&msg, 0, sizeof(msg));
    msg.type = MSG_ACCEPTED;

    // Set timestamp (big-endian)
    uint64_t ts = 1234567890123456789ULL;
    write_be64(reinterpret_cast<uint8_t*>(&msg.timestamp), ts);

    // Set quantity
    write_be32(reinterpret_cast<uint8_t*>(&msg.quantity), 1000);

    // Set price
    write_be32(reinterpret_cast<uint8_t*>(&msg.price), 1500000);

    // Verify getters
    ASSERT_EQ(msg.get_timestamp(), ts);
    ASSERT_EQ(msg.get_quantity(), 1000u);
    ASSERT_EQ(msg.get_price(), 1500000u);
}

TEST(test_executed_parse) {
    Executed msg;
    std::memset(&msg, 0, sizeof(msg));
    msg.type = MSG_EXECUTED;

    write_be32(reinterpret_cast<uint8_t*>(&msg.executed_quantity), 500);
    write_be32(reinterpret_cast<uint8_t*>(&msg.execution_price), 1510000);
    write_be64(reinterpret_cast<uint8_t*>(&msg.match_number), 9876543210ULL);

    ASSERT_EQ(msg.get_executed_quantity(), 500u);
    ASSERT_EQ(msg.get_execution_price(), 1510000u);
    ASSERT_EQ(msg.get_match_number(), 9876543210ULL);
}

TEST(test_canceled_parse) {
    Canceled msg;
    std::memset(&msg, 0, sizeof(msg));
    msg.type = MSG_CANCELED;

    write_be32(reinterpret_cast<uint8_t*>(&msg.decrement_quantity), 300);
    msg.reason = CANCEL_USER_REQUESTED;

    ASSERT_EQ(msg.get_decrement_quantity(), 300u);
    ASSERT_EQ(msg.reason, 'U');
}

// ============================================
// SoupBinTCP Tests
// ============================================

TEST(test_login_request_size) {
    ASSERT_EQ(sizeof(LoginRequest), 47);
}

TEST(test_login_accepted_size) {
    ASSERT_EQ(sizeof(LoginAccepted), 31);
}

TEST(test_login_request_init) {
    LoginRequest login;
    login.init();

    ASSERT_EQ(login.packet_type, SOUP_LOGIN_REQUEST);
}

TEST(test_login_request_credentials) {
    LoginRequest login;
    login.init();
    login.set_username("USER01");
    login.set_password("PASS123456");

    ASSERT_EQ(std::strncmp(login.username, "USER01", 6), 0);
    ASSERT_EQ(std::strncmp(login.password, "PASS123456", 10), 0);
}

// ============================================
// Session Config Tests
// ============================================

TEST(test_session_config_defaults) {
    OuchSessionConfig config;

    ASSERT_EQ(config.port, 15000);
    ASSERT_EQ(config.heartbeat_interval_ms, 1000u);
    ASSERT_EQ(config.connect_timeout_ms, 5000u);
    ASSERT_TRUE(config.tcp_nodelay);
}

// ============================================
// OuchOrderSender Tests
// ============================================

TEST(test_ouch_sender_register_symbol) {
    OuchSessionConfig config;
    OuchSession session(config);
    exchange::OuchOrderSender sender(session);

    sender.register_symbol(1, "AAPL");
    sender.register_symbol(2, "GOOGL");
    sender.register_symbol(3, "MSFT");

    // Initial state
    ASSERT_EQ(sender.orders_sent(), 0u);
    ASSERT_EQ(sender.pending_count(), 0u);
    ASSERT_EQ(sender.live_count(), 0u);
}

TEST(test_ouch_sender_not_connected) {
    OuchSessionConfig config;
    OuchSession session(config);
    exchange::OuchOrderSender sender(session);

    sender.register_symbol(1, "AAPL");

    // Should fail because not connected
    ASSERT_FALSE(sender.send_order(1, Side::Buy, 100, false));
    ASSERT_FALSE(sender.cancel_order(1, 12345));
}

TEST(test_ouch_sender_unknown_symbol) {
    OuchSessionConfig config;
    OuchSession session(config);
    exchange::OuchOrderSender sender(session);

    // Register only AAPL
    sender.register_symbol(1, "AAPL");

    // Even if connected, unknown symbol should fail
    // (Can't test this fully without mock connection)
    ASSERT_FALSE(sender.send_order(999, Side::Buy, 100, false));
}

TEST(test_ouch_sender_set_tif) {
    OuchSessionConfig config;
    OuchSession session(config);
    exchange::OuchOrderSender sender(session);

    sender.set_default_tif(TIF_DAY);
    // No assertion - just verify it doesn't crash
}

// ============================================
// Constants Tests
// ============================================

TEST(test_side_constants) {
    ASSERT_EQ(SIDE_BUY, 'B');
    ASSERT_EQ(SIDE_SELL, 'S');
    ASSERT_EQ(SIDE_SHORT, 'T');
    ASSERT_EQ(SIDE_SHORT_EXEMPT, 'E');
}

TEST(test_tif_constants) {
    ASSERT_EQ(TIF_DAY, 0u);
    ASSERT_EQ(TIF_IOC, 99998u);
    ASSERT_EQ(TIF_GTX, 99999u);
}

TEST(test_display_constants) {
    ASSERT_EQ(DISPLAY_VISIBLE, 'Y');
    ASSERT_EQ(DISPLAY_HIDDEN, 'N');
    ASSERT_EQ(DISPLAY_POST_ONLY, 'P');
    ASSERT_EQ(DISPLAY_MIDPOINT, 'M');
}

TEST(test_liquidity_constants) {
    ASSERT_EQ(LIQUIDITY_ADDED, 'A');
    ASSERT_EQ(LIQUIDITY_REMOVED, 'R');
    ASSERT_EQ(LIQUIDITY_ROUTED, 'X');
}

TEST(test_reject_reason_constants) {
    ASSERT_EQ(REJECT_HALTED, 'H');
    ASSERT_EQ(REJECT_DUPLICATE, 'D');
    ASSERT_EQ(REJECT_REGULATORY, 'R');
}

// ============================================
// Main
// ============================================

int main() {
    std::cout << "\n=== OUCH Protocol Tests ===\n\n";

    std::cout << "Message Structure Sizes:\n";
    RUN_TEST(test_enter_order_size);
    RUN_TEST(test_replace_order_size);
    RUN_TEST(test_cancel_order_size);
    RUN_TEST(test_modify_order_size);
    RUN_TEST(test_accepted_size);
    RUN_TEST(test_executed_size);
    RUN_TEST(test_canceled_size);
    RUN_TEST(test_rejected_size);
    RUN_TEST(test_replaced_size);

    std::cout << "\nBig-Endian Encoding:\n";
    RUN_TEST(test_write_be16);
    RUN_TEST(test_write_be32);
    RUN_TEST(test_write_be64);
    RUN_TEST(test_read_be16);
    RUN_TEST(test_read_be32);
    RUN_TEST(test_read_be64);

    std::cout << "\nEnterOrder Message:\n";
    RUN_TEST(test_enter_order_init);
    RUN_TEST(test_enter_order_set_token);
    RUN_TEST(test_enter_order_set_stock);
    RUN_TEST(test_enter_order_set_quantity);
    RUN_TEST(test_enter_order_set_price);
    RUN_TEST(test_enter_order_buy_side);
    RUN_TEST(test_enter_order_sell_side);

    std::cout << "\nCancelOrder Message:\n";
    RUN_TEST(test_cancel_order_init);
    RUN_TEST(test_cancel_order_full_cancel);
    RUN_TEST(test_cancel_order_partial);

    std::cout << "\nReplaceOrder Message:\n";
    RUN_TEST(test_replace_order_init);
    RUN_TEST(test_replace_order_tokens);

    std::cout << "\nResponse Message Parsing:\n";
    RUN_TEST(test_accepted_parse);
    RUN_TEST(test_executed_parse);
    RUN_TEST(test_canceled_parse);

    std::cout << "\nSoupBinTCP:\n";
    RUN_TEST(test_login_request_size);
    RUN_TEST(test_login_accepted_size);
    RUN_TEST(test_login_request_init);
    RUN_TEST(test_login_request_credentials);

    std::cout << "\nSession Config:\n";
    RUN_TEST(test_session_config_defaults);

    std::cout << "\nOuchOrderSender:\n";
    RUN_TEST(test_ouch_sender_register_symbol);
    RUN_TEST(test_ouch_sender_not_connected);
    RUN_TEST(test_ouch_sender_unknown_symbol);
    RUN_TEST(test_ouch_sender_set_tif);

    std::cout << "\nConstants:\n";
    RUN_TEST(test_side_constants);
    RUN_TEST(test_tif_constants);
    RUN_TEST(test_display_constants);
    RUN_TEST(test_liquidity_constants);
    RUN_TEST(test_reject_reason_constants);

    std::cout << "\n=== All OUCH Tests Passed! ===\n";
    return 0;
}
