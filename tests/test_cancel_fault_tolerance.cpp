/**
 * Cancel Fault Tolerance Test Suite
 *
 * Tests the 3-layer fault-tolerant order cancellation system:
 * - Layer 1: Rich error responses (CancelResult enum)
 * - Layer 2: Order state machine (OrderState enum)
 * - Layer 3: Periodic recovery (recover_stuck_orders)
 *
 * Run with: ./test_cancel_fault_tolerance
 */

#include "../include/exchange/paper_exchange_adapter.hpp"
#include "../include/execution/execution_engine.hpp"
#include "../include/execution/spot_limit_stage.hpp"
#include "../include/util/time_utils.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

using namespace hft;
using namespace hft::execution;
using namespace hft::exchange;

// =============================================================================
// Mock Exchange with Configurable Cancel Behavior
// =============================================================================

class MockExchangeAdapter : public IExchangeAdapter {
public:
    CancelResult cancel_result = CancelResult::Success; // Configurable
    int cancel_calls = 0;
    uint64_t next_order_id = 1000;
    bool order_pending = false; // For is_order_pending()

    uint64_t send_market_order(Symbol symbol, Side side, double qty, Price expected_price) override {
        return next_order_id++;
    }

    uint64_t send_limit_order(Symbol symbol, Side side, double qty, Price limit_price) override {
        return next_order_id++;
    }

    CancelResult cancel_order(uint64_t order_id) override {
        cancel_calls++;
        return cancel_result;
    }

    bool is_order_pending(uint64_t order_id) const override { return order_pending; }

    bool is_paper() const override { return true; }
};

// =============================================================================
// Test 1-4: CancelResult Basic Behavior
// =============================================================================

void test_cancel_success_clears_pending() {
    MockExchangeAdapter exchange;
    exchange.cancel_result = CancelResult::Success;

    ExecutionEngine engine;
    engine.set_exchange(&exchange);

    // Track pending order
    engine.track_pending_order_for_test(1001, 1, Side::Buy, 10.0, 50000, util::now_ns());
    assert(engine.pending_order_count() == 1);

    // Cancel should succeed and clear
    int resolved = engine.cancel_pending_for_symbol(1);
    assert(resolved == 1);
    assert(engine.pending_order_count() == 0);

    std::cout << "[PASS] test_cancel_success_clears_pending\n";
}

void test_cancel_not_found_clears_pending() {
    MockExchangeAdapter exchange;
    exchange.cancel_result = CancelResult::NotFound;

    ExecutionEngine engine;
    engine.set_exchange(&exchange);

    // Track pending order
    engine.track_pending_order_for_test(1002, 1, Side::Buy, 10.0, 50000, util::now_ns());
    assert(engine.pending_order_count() == 1);

    // Cancel returns NotFound → should still clear local state
    int resolved = engine.cancel_pending_for_symbol(1);
    assert(resolved == 1); // NotFound counts as "resolved"
    assert(engine.pending_order_count() == 0);

    std::cout << "[PASS] test_cancel_not_found_clears_pending\n";
}

void test_cancel_network_error_keeps_pending() {
    MockExchangeAdapter exchange;
    exchange.cancel_result = CancelResult::NetworkError;

    ExecutionEngine engine;
    engine.set_exchange(&exchange);

    // Track pending order
    engine.track_pending_order_for_test(1003, 1, Side::Buy, 10.0, 50000, util::now_ns());
    assert(engine.pending_order_count() == 1);

    // Cancel fails → should keep pending
    int resolved = engine.cancel_pending_for_symbol(1);
    assert(resolved == 0);                     // Not resolved
    assert(engine.pending_order_count() == 1); // Still pending

    std::cout << "[PASS] test_cancel_network_error_keeps_pending\n";
}

void test_cancel_rate_limited_keeps_pending() {
    MockExchangeAdapter exchange;
    exchange.cancel_result = CancelResult::RateLimited;

    ExecutionEngine engine;
    engine.set_exchange(&exchange);

    // Track pending order
    engine.track_pending_order_for_test(1004, 1, Side::Buy, 10.0, 50000, util::now_ns());
    assert(engine.pending_order_count() == 1);

    // Cancel rate-limited → should keep pending
    int resolved = engine.cancel_pending_for_symbol(1);
    assert(resolved == 0);                     // Not resolved
    assert(engine.pending_order_count() == 1); // Still pending

    std::cout << "[PASS] test_cancel_rate_limited_keeps_pending\n";
}

// =============================================================================
// Test 5-8: Order State Transitions
// =============================================================================

void test_state_active_to_cancel_sent() {
    MockExchangeAdapter exchange;
    exchange.cancel_result = CancelResult::NetworkError; // Keep order in CancelFailed

    ExecutionEngine engine;
    engine.set_exchange(&exchange);

    // Track pending order
    engine.track_pending_order_for_test(1005, 1, Side::Buy, 10.0, 50000, util::now_ns());

    // Attempt cancel → state should transition to CancelFailed (via CancelSent)
    engine.cancel_pending_for_symbol(1);
    assert(engine.pending_order_count() == 1); // Still active

    std::cout << "[PASS] test_state_active_to_cancel_sent\n";
}

void test_state_cancel_sent_to_cancel_failed() {
    MockExchangeAdapter exchange;
    exchange.cancel_result = CancelResult::NetworkError;

    ExecutionEngine engine;
    engine.set_exchange(&exchange);

    // Track and cancel (transitions to CancelFailed)
    engine.track_pending_order_for_test(1006, 1, Side::Buy, 10.0, 50000, util::now_ns());
    engine.cancel_pending_for_symbol(1);

    // Order should remain pending for retry
    assert(engine.pending_order_count() == 1);

    std::cout << "[PASS] test_state_cancel_sent_to_cancel_failed\n";
}

void test_fill_during_cancel_sent_clears() {
    MockExchangeAdapter exchange;
    exchange.cancel_result = CancelResult::NetworkError;

    ExecutionEngine engine;
    engine.set_exchange(&exchange);

    // Track pending order
    engine.track_pending_order_for_test(1007, 1, Side::Buy, 10.0, 50000, util::now_ns());
    engine.cancel_pending_for_symbol(1); // Attempt cancel (fails)

    // Simulate fill during cancel (fill wins the race)
    engine.on_fill(1007, 1, Side::Buy, 10.0, 50100);

    // Fill should clear the pending order regardless of cancel state
    assert(engine.pending_order_count() == 0);

    std::cout << "[PASS] test_fill_during_cancel_sent_clears\n";
}

void test_fill_during_cancel_failed_clears() {
    MockExchangeAdapter exchange;
    exchange.cancel_result = CancelResult::NetworkError;

    ExecutionEngine engine;
    engine.set_exchange(&exchange);

    // Track, cancel (fail), then fill
    engine.track_pending_order_for_test(1008, 1, Side::Buy, 10.0, 50000, util::now_ns());
    engine.cancel_pending_for_symbol(1); // Fails → CancelFailed state
    assert(engine.pending_order_count() == 1);

    // Fill arrives
    engine.on_fill(1008, 1, Side::Buy, 10.0, 50100);
    assert(engine.pending_order_count() == 0); // Fill clears

    std::cout << "[PASS] test_fill_during_cancel_failed_clears\n";
}

// =============================================================================
// Test 9-13: Retry Logic
// =============================================================================

void test_retry_after_interval() {
    MockExchangeAdapter exchange;
    exchange.cancel_result = CancelResult::NetworkError;

    ExecutionEngine engine;
    engine.set_exchange(&exchange);

    // Track and fail cancel 2 seconds in the past
    uint64_t old_time = util::now_ns() - 2'000'000'000; // 2 seconds ago
    engine.track_pending_order_for_test(1009, 1, Side::Buy, 10.0, 50000, old_time);
    engine.cancel_pending_for_symbol(1); // Attempt 1 (fails)
    assert(exchange.cancel_calls == 1);

    // Sleep 1+ second to allow retry interval to pass
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    // Recover → should retry now
    engine.recover_stuck_orders();
    assert(exchange.cancel_calls == 2); // Retry happened

    std::cout << "[PASS] test_retry_after_interval\n";
}

void test_retry_before_interval_skipped() {
    MockExchangeAdapter exchange;
    exchange.cancel_result = CancelResult::NetworkError;

    ExecutionEngine engine;
    engine.set_exchange(&exchange);

    // Track and fail cancel very recently
    engine.track_pending_order_for_test(1010, 1, Side::Buy, 10.0, 50000, util::now_ns());
    engine.cancel_pending_for_symbol(1); // Attempt 1 (fails)
    assert(exchange.cancel_calls == 1);

    // Immediate recover → should NOT retry (< 1s interval)
    engine.recover_stuck_orders();
    assert(exchange.cancel_calls == 1); // No retry yet

    std::cout << "[PASS] test_retry_before_interval_skipped\n";
}

void test_max_retries_triggers_query() {
    MockExchangeAdapter exchange;
    exchange.cancel_result = CancelResult::NetworkError;
    exchange.order_pending = true; // Order still on exchange

    ExecutionEngine engine;
    engine.set_exchange(&exchange);

    // Track and fail cancel 3 times
    uint64_t old_time = util::now_ns() - 10'000'000'000; // 10 seconds ago
    engine.track_pending_order_for_test(1011, 1, Side::Buy, 10.0, 50000, old_time);

    // Attempt 1
    engine.cancel_pending_for_symbol(1);
    assert(exchange.cancel_calls == 1);

    // Attempt 2 (after 1s)
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    engine.recover_stuck_orders();
    assert(exchange.cancel_calls == 2);

    // Attempt 3 (after 2s)
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    engine.recover_stuck_orders();
    assert(exchange.cancel_calls == 3);

    // After 3 attempts, should query exchange and retry
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    engine.recover_stuck_orders();
    assert(exchange.cancel_calls == 4); // Final retry triggered

    std::cout << "[PASS] test_max_retries_triggers_query\n";
}

void test_query_finds_order_retries_cancel() {
    MockExchangeAdapter exchange;
    exchange.cancel_result = CancelResult::NetworkError;
    exchange.order_pending = true; // Order still there

    ExecutionEngine engine;
    engine.set_exchange(&exchange);

    // Simulate 3 failed attempts
    uint64_t old_time = util::now_ns() - 10'000'000'000;
    engine.track_pending_order_for_test(1012, 1, Side::Buy, 10.0, 50000, old_time);
    engine.cancel_pending_for_symbol(1); // 1
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    engine.recover_stuck_orders(); // 2
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    engine.recover_stuck_orders(); // 3

    int calls_before = exchange.cancel_calls;
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    engine.recover_stuck_orders(); // Should query, find order, retry

    assert(exchange.cancel_calls == calls_before + 1); // Retry happened

    std::cout << "[PASS] test_query_finds_order_retries_cancel\n";
}

void test_query_order_gone_force_clears() {
    MockExchangeAdapter exchange;
    exchange.cancel_result = CancelResult::NetworkError;
    exchange.order_pending = false; // Order gone

    ExecutionEngine engine;
    engine.set_exchange(&exchange);

    // Simulate 3 failed attempts
    uint64_t old_time = util::now_ns() - 10'000'000'000;
    engine.track_pending_order_for_test(1013, 1, Side::Buy, 10.0, 50000, old_time);
    engine.cancel_pending_for_symbol(1); // 1
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    engine.recover_stuck_orders(); // 2
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    engine.recover_stuck_orders(); // 3

    assert(engine.pending_order_count() == 1); // Still there before recovery

    // Recovery after 3 attempts → query finds nothing → force clear
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    engine.recover_stuck_orders();
    assert(engine.pending_order_count() == 0); // Cleared

    std::cout << "[PASS] test_query_order_gone_force_clears\n";
}

// =============================================================================
// Test 14-15: SpotLimitStage Integration
// =============================================================================

void test_cancel_failed_blocks_new_order() {
    MockExchangeAdapter exchange;
    exchange.cancel_result = CancelResult::NetworkError;

    ExecutionEngine engine;
    engine.set_exchange(&exchange);

    SpotLimitStage stage(&engine);

    // Place limit order
    strategy::Signal signal = strategy::Signal::buy(strategy::SignalStrength::Weak, 10.0, "test");
    ExecutionContext ctx;
    ctx.symbol = 1;
    ctx.market.bid = 100000;
    ctx.market.ask = 100100;
    ctx.metrics = nullptr;

    auto req1 = stage.process(signal, ctx);
    assert(req1.size() == 1);

    // Track it
    stage.track_pending(1, 1014, Side::Buy, 100050, 10.0, util::now_ns());

    // Try to place opposite direction → should cancel first
    strategy::Signal sell_signal = strategy::Signal::sell(strategy::SignalStrength::Weak, 5.0, "test");
    ctx.position.quantity = 10.0; // Have position

    auto req2 = stage.process(sell_signal, ctx);

    // Cancel failed → should return empty (can't place new order)
    assert(req2.empty());

    std::cout << "[PASS] test_cancel_failed_blocks_new_order\n";
}

void test_cancel_all_partial_failure() {
    MockExchangeAdapter exchange;
    ExecutionEngine engine;
    engine.set_exchange(&exchange);

    // Track 2 pending orders
    engine.track_pending_order_for_test(1015, 1, Side::Buy, 10.0, 100050, util::now_ns());
    engine.track_pending_order_for_test(1016, 2, Side::Sell, 5.0, 100150, util::now_ns());
    assert(engine.pending_order_count() == 2);

    // Cancel symbol 1 succeeds
    exchange.cancel_result = CancelResult::Success;
    int resolved1 = engine.cancel_pending_for_symbol(1);
    assert(resolved1 == 1);                    // Symbol 1 canceled
    assert(engine.pending_order_count() == 1); // One order left

    // Cancel symbol 2 fails
    exchange.cancel_result = CancelResult::NetworkError;
    int resolved2 = engine.cancel_pending_for_symbol(2);
    assert(resolved2 == 0);                    // Symbol 2 failed
    assert(engine.pending_order_count() == 1); // Still one order (stuck)

    std::cout << "[PASS] test_cancel_all_partial_failure\n";
}

// =============================================================================
// Test 16-17: PaperExchangeAdapter
// =============================================================================

void test_paper_returns_success_for_valid_order() {
    PaperExchangeAdapter adapter;
    Symbol sym_id = adapter.register_symbol("BTCUSDT");

    // Send limit order
    uint64_t order_id = adapter.send_limit_order(sym_id, Side::Buy, 1.0, 50000);
    assert(order_id > 0);

    // Cancel existing order → Success
    CancelResult result = adapter.cancel_order(order_id);
    assert(result == CancelResult::Success);

    std::cout << "[PASS] test_paper_returns_success_for_valid_order\n";
}

void test_paper_returns_not_found_for_invalid() {
    PaperExchangeAdapter adapter;

    // Cancel non-existent order → NotFound
    CancelResult result = adapter.cancel_order(99999);
    assert(result == CancelResult::NotFound);

    std::cout << "[PASS] test_paper_returns_not_found_for_invalid\n";
}

// =============================================================================
// Main Test Runner
// =============================================================================

int main() {
    std::cout << "\n=== Cancel Fault Tolerance Tests ===\n\n";

    // Test 1-4: CancelResult basic behavior
    test_cancel_success_clears_pending();
    test_cancel_not_found_clears_pending();
    test_cancel_network_error_keeps_pending();
    test_cancel_rate_limited_keeps_pending();

    // Test 5-8: Order state transitions
    test_state_active_to_cancel_sent();
    test_state_cancel_sent_to_cancel_failed();
    test_fill_during_cancel_sent_clears();
    test_fill_during_cancel_failed_clears();

    // Test 9-13: Retry logic
    test_retry_after_interval();
    test_retry_before_interval_skipped();
    test_max_retries_triggers_query();
    test_query_finds_order_retries_cancel();
    test_query_order_gone_force_clears();

    // Test 14-15: SpotLimitStage integration
    test_cancel_failed_blocks_new_order();
    test_cancel_all_partial_failure();

    // Test 16-17: PaperExchangeAdapter
    test_paper_returns_success_for_valid_order();
    test_paper_returns_not_found_for_invalid();

    std::cout << "\n=== All 17 Tests Passed ===\n\n";
    return 0;
}
