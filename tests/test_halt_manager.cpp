#include "../include/strategy/halt_manager.hpp"
#include "../include/types.hpp"

#include <cassert>
#include <iostream>
#include <thread>

using namespace hft;
using namespace hft::strategy;

// ============================================================================
// Basic State Tests
// ============================================================================

void test_initial_state() {
    HaltManager hm;
    assert(hm.state() == HaltState::Running);
    assert(hm.reason() == HaltReason::None);
    assert(!hm.is_halted());
    assert(hm.can_trade());
    std::cout << "[PASS] test_initial_state\n";
}

void test_halt_reason_to_string() {
    assert(std::string(halt_reason_to_string(HaltReason::None)) == "None");
    assert(std::string(halt_reason_to_string(HaltReason::PoolExhausted)) == "PoolExhausted");
    assert(std::string(halt_reason_to_string(HaltReason::PoolCritical)) == "PoolCritical");
    assert(std::string(halt_reason_to_string(HaltReason::MaxLossExceeded)) == "MaxLossExceeded");
    assert(std::string(halt_reason_to_string(HaltReason::ManualHalt)) == "ManualHalt");
    assert(std::string(halt_reason_to_string(HaltReason::SystemError)) == "SystemError");
    assert(std::string(halt_reason_to_string(HaltReason::ConnectionLost)) == "ConnectionLost");
    assert(std::string(halt_reason_to_string(HaltReason::ExchangeHalt)) == "ExchangeHalt");
    assert(std::string(halt_reason_to_string(HaltReason::CircuitBreaker)) == "CircuitBreaker");

    // Test unknown/invalid enum value
    assert(std::string(halt_reason_to_string(static_cast<HaltReason>(99))) == "Unknown");
    std::cout << "[PASS] test_halt_reason_to_string\n";
}

void test_halt_state_to_string() {
    assert(std::string(halt_state_to_string(HaltState::Running)) == "Running");
    assert(std::string(halt_state_to_string(HaltState::Halting)) == "Halting");
    assert(std::string(halt_state_to_string(HaltState::Halted)) == "Halted");
    assert(std::string(halt_state_to_string(HaltState::Error)) == "Error");

    // Test unknown/invalid enum value
    assert(std::string(halt_state_to_string(static_cast<HaltState>(99))) == "Unknown");
    std::cout << "[PASS] test_halt_state_to_string\n";
}

// ============================================================================
// Callback Tests
// ============================================================================

void test_set_callbacks() {
    HaltManager hm;

    bool get_positions_called = false;
    hm.set_get_positions_callback([&]() {
        get_positions_called = true;
        return std::vector<PositionInfo>{};
    });

    bool cancel_all_called = false;
    hm.set_cancel_all_callback([&]() { cancel_all_called = true; });

    bool send_order_called = false;
    hm.set_send_order_callback([&](Symbol, Side, Quantity, bool) {
        send_order_called = true;
        return true;
    });

    bool alert_called = false;
    hm.set_alert_callback([&](HaltReason, const std::string&) { alert_called = true; });

    bool log_called = false;
    hm.set_log_callback([&](const std::string&) { log_called = true; });

    // Trigger halt to invoke callbacks
    hm.halt(HaltReason::ManualHalt);

    assert(log_called);
    assert(alert_called);
    assert(cancel_all_called);

    std::cout << "[PASS] test_set_callbacks\n";
}

// ============================================================================
// Halt Trigger Tests
// ============================================================================

void test_halt_changes_state() {
    HaltManager hm;

    bool alert_fired = false;
    HaltReason alert_reason = HaltReason::None;
    hm.set_alert_callback([&](HaltReason reason, const std::string&) {
        alert_fired = true;
        alert_reason = reason;
    });

    hm.set_cancel_all_callback([]() {});
    hm.set_get_positions_callback([]() { return std::vector<PositionInfo>{}; });

    hm.halt(HaltReason::PoolExhausted);

    assert(hm.is_halted());
    assert(!hm.can_trade());
    assert(hm.reason() == HaltReason::PoolExhausted);
    assert(alert_fired);
    assert(alert_reason == HaltReason::PoolExhausted);

    std::cout << "[PASS] test_halt_changes_state\n";
}

void test_halt_cancels_all_orders() {
    HaltManager hm;

    bool cancel_all_called = false;
    hm.set_cancel_all_callback([&]() { cancel_all_called = true; });

    hm.set_get_positions_callback([]() { return std::vector<PositionInfo>{}; });

    hm.halt(HaltReason::SystemError);

    assert(cancel_all_called);
    std::cout << "[PASS] test_halt_cancels_all_orders\n";
}

void test_halt_flattens_positions() {
    HaltManager hm;

    std::vector<Symbol> flatten_symbols;
    hm.set_send_order_callback([&](Symbol sym, Side side, Quantity qty, bool is_market) {
        flatten_symbols.push_back(sym);
        return true;
    });

    hm.set_cancel_all_callback([]() {});

    // Return positions to flatten
    hm.set_get_positions_callback([]() {
        return std::vector<PositionInfo>{{0, "BTC", 100, 50000}, {1, "ETH", -50, 3000}};
    });

    hm.halt(HaltReason::MaxLossExceeded);

    // Should send flatten orders for both symbols
    assert(flatten_symbols.size() == 2);
    assert(hm.state() == HaltState::Halted);

    std::cout << "[PASS] test_halt_flattens_positions\n";
}

void test_halt_idempotent() {
    HaltManager hm;

    hm.set_cancel_all_callback([]() {});
    hm.set_get_positions_callback([]() { return std::vector<PositionInfo>{}; });

    hm.halt(HaltReason::PoolCritical);
    assert(hm.reason() == HaltReason::PoolCritical);

    // Second halt should not change reason
    hm.halt(HaltReason::SystemError);
    assert(hm.reason() == HaltReason::PoolCritical); // First reason wins

    std::cout << "[PASS] test_halt_idempotent\n";
}

// ============================================================================
// Flatten Tests
// ============================================================================

void test_flatten_no_positions() {
    HaltManager hm;

    hm.set_cancel_all_callback([]() {});
    hm.set_get_positions_callback([]() { return std::vector<PositionInfo>{}; });

    hm.halt(HaltReason::ConnectionLost);

    assert(hm.state() == HaltState::Halted); // Should transition directly to halted
    std::cout << "[PASS] test_flatten_no_positions\n";
}

void test_flatten_with_positions() {
    HaltManager hm;

    int flatten_count = 0;
    hm.set_send_order_callback([&](Symbol, Side, Quantity, bool) {
        flatten_count++;
        return true;
    });

    hm.set_cancel_all_callback([]() {});

    hm.set_get_positions_callback([]() {
        return std::vector<PositionInfo>{{0, "BTC", 100, 50000}, {1, "ETH", 200, 3000}, {2, "SOL", -50, 150}};
    });

    hm.halt(HaltReason::CircuitBreaker);

    assert(flatten_count == 3); // Should send 3 flatten orders
    assert(hm.state() == HaltState::Halted);

    std::cout << "[PASS] test_flatten_with_positions\n";
}

void test_flatten_order_failure() {
    HaltManager hm;

    // All flatten orders fail
    hm.set_send_order_callback([](Symbol, Side, Quantity, bool) {
        return false; // Always fail
    });

    hm.set_cancel_all_callback([]() {});

    hm.set_get_positions_callback([]() { return std::vector<PositionInfo>{{0, "BTC", 100, 50000}}; });

    hm.halt(HaltReason::ExchangeHalt);

    // Should be in Error state because flatten failed
    assert(hm.state() == HaltState::Error);

    std::cout << "[PASS] test_flatten_order_failure\n";
}

void test_flatten_with_zero_positions() {
    HaltManager hm;

    int flatten_count = 0;
    hm.set_send_order_callback([&](Symbol, Side, Quantity, bool) {
        flatten_count++;
        return true;
    });

    hm.set_cancel_all_callback([]() {});

    // Mix of zero and non-zero positions - covers line 296 (continue when position == 0)
    hm.set_get_positions_callback([]() {
        return std::vector<PositionInfo>{
            {0, "BTC", 100, 50000}, // Non-zero
            {1, "ETH", 0, 3000},    // Zero - should skip (line 296)
            {2, "SOL", -50, 150}    // Non-zero
        };
    });

    hm.halt(HaltReason::CircuitBreaker);

    assert(flatten_count == 2); // Should send only 2 flatten orders (skipping ETH with position 0)
    assert(hm.state() == HaltState::Halted);

    std::cout << "[PASS] test_flatten_with_zero_positions\n";
}

// ============================================================================
// Reset Tests
// ============================================================================

void test_reset_from_halted() {
    HaltManager hm;

    hm.set_cancel_all_callback([]() {});
    hm.set_get_positions_callback([]() { return std::vector<PositionInfo>{}; });

    hm.halt(HaltReason::ManualHalt);
    assert(hm.is_halted());

    hm.reset();

    assert(hm.state() == HaltState::Running);
    assert(hm.reason() == HaltReason::None);
    assert(!hm.is_halted());
    assert(hm.can_trade());

    std::cout << "[PASS] test_reset_from_halted\n";
}

// ============================================================================
// Error State Tests
// ============================================================================

void test_flatten_enters_error_state_on_failure() {
    HaltManager hm;

    // All flatten orders fail
    hm.set_send_order_callback([](Symbol, Side, Quantity, bool) {
        return false; // Always fail
    });

    hm.set_cancel_all_callback([]() {});

    hm.set_get_positions_callback([]() { return std::vector<PositionInfo>{{0, "BTC", 100, 50000}}; });

    hm.halt(HaltReason::PoolExhausted);

    assert(hm.state() == HaltState::Error); // Should be in error state
    assert(hm.reason() == HaltReason::PoolExhausted);

    std::cout << "[PASS] test_flatten_enters_error_state_on_failure\n";
}

void test_retry_flatten_from_error_state() {
    HaltManager hm;

    int attempt = 0;
    hm.set_send_order_callback([&](Symbol, Side, Quantity, bool) {
        attempt++;
        return attempt > 1; // Fail first, succeed second
    });

    hm.set_cancel_all_callback([]() {});

    int position_calls = 0;
    hm.set_get_positions_callback([&]() {
        position_calls++;
        if (position_calls == 1) {
            return std::vector<PositionInfo>{{0, "BTC", 100, 50000}};
        } else {
            return std::vector<PositionInfo>{}; // Position closed
        }
    });

    hm.halt(HaltReason::SystemError);
    assert(hm.state() == HaltState::Error);

    bool retry_success = hm.retry_flatten();
    assert(retry_success);
    assert(hm.state() == HaltState::Halted);

    std::cout << "[PASS] test_retry_flatten_from_error_state\n";
}

void test_retry_flatten_max_attempts() {
    HaltManager hm;
    hm.set_max_flatten_attempts(2);

    // Always fail
    hm.set_send_order_callback([](Symbol, Side, Quantity, bool) { return false; });

    hm.set_cancel_all_callback([]() {});

    hm.set_get_positions_callback([]() { return std::vector<PositionInfo>{{0, "BTC", 100, 50000}}; });

    hm.halt(HaltReason::MaxLossExceeded);
    assert(hm.state() == HaltState::Error);

    // First retry
    bool retry1 = hm.retry_flatten();
    assert(!retry1); // Should fail
    assert(hm.state() == HaltState::Error);

    // Second retry should be blocked (max attempts reached)
    bool retry2 = hm.retry_flatten();
    assert(!retry2);

    std::cout << "[PASS] test_retry_flatten_max_attempts\n";
}

void test_retry_flatten_from_non_error_state() {
    HaltManager hm;

    hm.set_cancel_all_callback([]() {});
    hm.set_get_positions_callback([]() { return std::vector<PositionInfo>{}; });

    hm.halt(HaltReason::CircuitBreaker);
    assert(hm.state() == HaltState::Halted); // Successful halt

    // Try to retry from Halted state (should fail)
    bool retry_result = hm.retry_flatten();
    assert(!retry_result);

    std::cout << "[PASS] test_retry_flatten_from_non_error_state\n";
}

// ============================================================================
// Configuration Tests
// ============================================================================

void test_set_max_flatten_attempts() {
    HaltManager hm;
    hm.set_max_flatten_attempts(5);

    // This is tested indirectly in test_retry_flatten_max_attempts
    std::cout << "[PASS] test_set_max_flatten_attempts\n";
}

// ============================================================================
// No Callbacks Tests
// ============================================================================

void test_halt_without_send_order_callback() {
    HaltManager hm;

    hm.set_cancel_all_callback([]() {});
    hm.set_get_positions_callback([]() { return std::vector<PositionInfo>{{0, "BTC", 100, 50000}}; });

    // Don't set send_order_callback
    hm.halt(HaltReason::ConnectionLost);

    // Should enter error state because it can't flatten
    assert(hm.state() == HaltState::Error);

    std::cout << "[PASS] test_halt_without_send_order_callback\n";
}

void test_halt_with_message() {
    HaltManager hm;

    std::string logged_message;
    hm.set_log_callback([&](const std::string& msg) {
        if (msg.find("Message:") != std::string::npos) {
            logged_message = msg;
        }
    });

    hm.set_cancel_all_callback([]() {});
    hm.set_get_positions_callback([]() { return std::vector<PositionInfo>{}; });

    hm.halt(HaltReason::ManualHalt, "Emergency stop requested by operator");

    assert(!logged_message.empty());
    assert(logged_message.find("Emergency stop") != std::string::npos);

    std::cout << "[PASS] test_halt_with_message\n";
}

// ============================================================================
// Multi-threaded Tests
// ============================================================================

void test_concurrent_halt_calls() {
    HaltManager hm;

    hm.set_cancel_all_callback([]() {});
    hm.set_get_positions_callback([]() { return std::vector<PositionInfo>{}; });

    // Trigger halts from multiple threads
    std::thread t1([&]() { hm.halt(HaltReason::PoolExhausted); });
    std::thread t2([&]() { hm.halt(HaltReason::SystemError); });
    std::thread t3([&]() { hm.halt(HaltReason::ConnectionLost); });

    t1.join();
    t2.join();
    t3.join();

    // Should be halted with one of the reasons
    assert(hm.is_halted());
    assert(hm.reason() != HaltReason::None);

    std::cout << "[PASS] test_concurrent_halt_calls\n";
}

void test_concurrent_state_queries() {
    HaltManager hm;

    std::atomic<int> query_count{0};

    auto query_thread = [&]() {
        for (int i = 0; i < 1000; i++) {
            hm.is_halted();
            hm.can_trade();
            hm.state();
            hm.reason();
            query_count++;
        }
    };

    std::thread t1(query_thread);
    std::thread t2(query_thread);
    std::thread t3(query_thread);

    t1.join();
    t2.join();
    t3.join();

    assert(query_count == 3000);

    std::cout << "[PASS] test_concurrent_state_queries\n";
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "Running HaltManager tests...\n\n";

    test_initial_state();
    test_halt_reason_to_string();
    test_halt_state_to_string();
    test_set_callbacks();
    test_halt_changes_state();
    test_halt_cancels_all_orders();
    test_halt_flattens_positions();
    test_halt_idempotent();
    test_flatten_no_positions();
    test_flatten_with_positions();
    test_flatten_order_failure();
    test_flatten_with_zero_positions();
    test_reset_from_halted();
    test_flatten_enters_error_state_on_failure();
    test_retry_flatten_from_error_state();
    test_retry_flatten_max_attempts();
    test_retry_flatten_from_non_error_state();
    test_set_max_flatten_attempts();
    test_halt_without_send_order_callback();
    test_halt_with_message();
    test_concurrent_halt_calls();
    test_concurrent_state_queries();

    std::cout << "\n✓ All 22 HaltManager tests passed!\n";
    return 0;
}
