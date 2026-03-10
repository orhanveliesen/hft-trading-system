#include "../include/types.hpp"

#include <cassert>
#include <cstring>
#include <iostream>

using namespace hft;

#define TEST(name) void name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "Running " << #name << "... ";                                                                    \
        name();                                                                                                        \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

// ============================================================================
// Order ID Validation
// ============================================================================

TEST(test_valid_order_id) {
    assert(is_valid_order_id(1));
    assert(is_valid_order_id(100));
    assert(is_valid_order_id(MAX_ORDERS - 1));
}

TEST(test_invalid_order_id_zero) {
    assert(!is_valid_order_id(INVALID_ORDER_ID));
    assert(!is_valid_order_id(0));
}

TEST(test_invalid_order_id_too_large) {
    assert(!is_valid_order_id(MAX_ORDERS));
}

// ============================================================================
// OrderResult String Conversion
// ============================================================================

TEST(test_order_result_to_string_success) {
    const char* str = order_result_to_string(OrderResult::Success);
    assert(std::strcmp(str, "Success") == 0);
}

TEST(test_order_result_to_string_pool_exhausted) {
    const char* str = order_result_to_string(OrderResult::PoolExhausted);
    assert(std::strcmp(str, "PoolExhausted") == 0);
}

TEST(test_order_result_to_string_invalid_order_id) {
    const char* str = order_result_to_string(OrderResult::InvalidOrderId);
    assert(std::strcmp(str, "InvalidOrderId") == 0);
}

TEST(test_order_result_to_string_invalid_price) {
    const char* str = order_result_to_string(OrderResult::InvalidPrice);
    assert(std::strcmp(str, "InvalidPrice") == 0);
}

TEST(test_order_result_to_string_invalid_quantity) {
    const char* str = order_result_to_string(OrderResult::InvalidQuantity);
    assert(std::strcmp(str, "InvalidQuantity") == 0);
}

TEST(test_order_result_to_string_order_not_found) {
    const char* str = order_result_to_string(OrderResult::OrderNotFound);
    assert(std::strcmp(str, "OrderNotFound") == 0);
}

TEST(test_order_result_to_string_system_halted) {
    const char* str = order_result_to_string(OrderResult::SystemHalted);
    assert(std::strcmp(str, "SystemHalted") == 0);
}

TEST(test_order_result_to_string_duplicate_order_id) {
    const char* str = order_result_to_string(OrderResult::DuplicateOrderId);
    assert(std::strcmp(str, "DuplicateOrderId") == 0);
}

TEST(test_order_result_to_string_rate_limit_exceeded) {
    const char* str = order_result_to_string(OrderResult::RateLimitExceeded);
    assert(std::strcmp(str, "RateLimitExceeded") == 0);
}

TEST(test_order_result_to_string_max_orders_exceeded) {
    const char* str = order_result_to_string(OrderResult::MaxOrdersExceeded);
    assert(std::strcmp(str, "MaxOrdersExceeded") == 0);
}

TEST(test_order_result_to_string_unknown) {
    // Cast an invalid value to OrderResult
    const char* str = order_result_to_string(static_cast<OrderResult>(255));
    assert(std::strcmp(str, "Unknown") == 0);
}

// ============================================================================
// Order Initialization and Operations
// ============================================================================

TEST(test_order_init) {
    Order order;
    order.init(12345, 100, 1000000, 1, 10000, 50, Side::Buy);

    assert(order.id == 12345);
    assert(order.trader_id == 100);
    assert(order.timestamp == 1000000);
    assert(order.symbol == 1);
    assert(order.price == 10000);
    assert(order.quantity == 50);
    assert(order.side == Side::Buy);
    assert(order.prev == nullptr);
    assert(order.next == nullptr);
}

TEST(test_order_reset) {
    Order order;
    order.init(12345, 100, 1000000, 1, 10000, 50, Side::Buy);

    // Simulate linking in a list
    Order other;
    order.prev = &other;
    order.next = &other;

    // Reset should clear pointers
    order.reset();
    assert(order.prev == nullptr);
    assert(order.next == nullptr);
}

TEST(test_order_reduce_quantity) {
    Order order;
    order.init(12345, 100, 1000000, 1, 10000, 50, Side::Buy);

    order.reduce_quantity(10);
    assert(order.quantity == 40);

    order.reduce_quantity(30);
    assert(order.quantity == 10);
}

TEST(test_order_is_fully_filled_false) {
    Order order;
    order.init(12345, 100, 1000000, 1, 10000, 50, Side::Buy);
    assert(!order.is_fully_filled());
}

TEST(test_order_is_fully_filled_true) {
    Order order;
    order.init(12345, 100, 1000000, 1, 10000, 50, Side::Buy);
    order.reduce_quantity(50);
    assert(order.is_fully_filled());
}

// ============================================================================
// PriceLevel Operations
// ============================================================================

TEST(test_price_level_reduce_quantity) {
    PriceLevel level{};
    level.price = 10000;
    level.total_quantity = 500;

    level.reduce_quantity(100);
    assert(level.total_quantity == 400);

    level.reduce_quantity(300);
    assert(level.total_quantity == 100);
}

TEST(test_price_level_add_quantity) {
    PriceLevel level{};
    level.price = 10000;
    level.total_quantity = 100;

    level.add_quantity(50);
    assert(level.total_quantity == 150);

    level.add_quantity(200);
    assert(level.total_quantity == 350);
}

TEST(test_price_level_is_empty_false) {
    PriceLevel level{};
    level.total_quantity = 100;
    assert(!level.is_empty());
}

TEST(test_price_level_is_empty_true) {
    PriceLevel level{};
    level.total_quantity = 0;
    assert(level.is_empty());
}

TEST(test_price_level_is_empty_after_reduce) {
    PriceLevel level{};
    level.total_quantity = 100;
    level.reduce_quantity(100);
    assert(level.is_empty());
}

int main() {
    RUN_TEST(test_valid_order_id);
    RUN_TEST(test_invalid_order_id_zero);
    RUN_TEST(test_invalid_order_id_too_large);

    RUN_TEST(test_order_result_to_string_success);
    RUN_TEST(test_order_result_to_string_pool_exhausted);
    RUN_TEST(test_order_result_to_string_invalid_order_id);
    RUN_TEST(test_order_result_to_string_invalid_price);
    RUN_TEST(test_order_result_to_string_invalid_quantity);
    RUN_TEST(test_order_result_to_string_order_not_found);
    RUN_TEST(test_order_result_to_string_system_halted);
    RUN_TEST(test_order_result_to_string_duplicate_order_id);
    RUN_TEST(test_order_result_to_string_rate_limit_exceeded);
    RUN_TEST(test_order_result_to_string_max_orders_exceeded);
    RUN_TEST(test_order_result_to_string_unknown);

    RUN_TEST(test_order_init);
    RUN_TEST(test_order_reset);
    RUN_TEST(test_order_reduce_quantity);
    RUN_TEST(test_order_is_fully_filled_false);
    RUN_TEST(test_order_is_fully_filled_true);

    RUN_TEST(test_price_level_reduce_quantity);
    RUN_TEST(test_price_level_add_quantity);
    RUN_TEST(test_price_level_is_empty_false);
    RUN_TEST(test_price_level_is_empty_true);
    RUN_TEST(test_price_level_is_empty_after_reduce);

    std::cout << "\nAll 24 types tests passed!\n";
    return 0;
}
