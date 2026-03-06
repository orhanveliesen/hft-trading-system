#include "../include/execution/execution_engine.hpp"

#include <cassert>
#include <cstring>
#include <iostream>

using namespace hft;
using namespace hft::execution;

// Test 1: Market order type
void test_market_order_type() {
    const char* result = order_type_str(OrderType::Market);
    assert(std::strcmp(result, "MARKET") == 0);
    std::cout << "[PASS] test_market_order_type\n";
}

// Test 2: Limit order type
void test_limit_order_type() {
    const char* result = order_type_str(OrderType::Limit);
    assert(std::strcmp(result, "LIMIT") == 0);
    std::cout << "[PASS] test_limit_order_type\n";
}

// Test 3: Invalid OrderType enum value → returns "UNKNOWN"
void test_invalid_order_type() {
    // Cast invalid value to OrderType enum
    OrderType invalid_type = static_cast<OrderType>(99);

    const char* result = order_type_str(invalid_type);
    assert(std::strcmp(result, "UNKNOWN") == 0);
    std::cout << "[PASS] test_invalid_order_type\n";
}

int main() {
    std::cout << "Running order_type_str tests...\n\n";

    test_market_order_type();
    test_limit_order_type();
    test_invalid_order_type();

    std::cout << "\n3 order_type_str tests passed!\n";
    std::cout << "✓ Coverage: All OrderType enum values tested\n";
    std::cout << "✓ Invalid enum default case covered\n";
    return 0;
}
