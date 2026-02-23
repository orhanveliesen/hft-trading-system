#include "../include/ipc/shared_event_log.hpp"
#include "../include/ipc/tuner_event.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

using namespace hft::ipc;

#define TEST(name) void name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "  " << #name << "... ";                                                                          \
        name();                                                                                                        \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

#define ASSERT_EQ(a, b) assert((a) == (b))
#define ASSERT_TRUE(x) assert(x)
#define ASSERT_FALSE(x) assert(!(x))
#define ASSERT_STREQ(a, b) assert(std::strcmp((a), (b)) == 0)

// Error codes (matching plan)
static constexpr int32_t ERROR_JSON_PARSE_FAILED = 1001;
static constexpr int32_t ERROR_API_FAILED = 1002;
static constexpr int32_t ERROR_API_TIMEOUT = 1003;
static constexpr int32_t ERROR_INVALID_COMMAND = 1004;
static constexpr int32_t ERROR_WS_DISCONNECT = 2001;
static constexpr int32_t ERROR_HEARTBEAT_TIMEOUT = 2002;
static constexpr int32_t ERROR_POSITION_LIMIT = 2003;

constexpr const char* TEST_SHM_NAME = "/error_event_test";

// ============================================================================
// TunerEvent::make_error Tests
// ============================================================================

TEST(error_event_creation) {
    auto event = TunerEvent::make_error("claude_client", ERROR_JSON_PARSE_FAILED,
                                        true, // recoverable
                                        "No valid JSON in response");

    // Verify event type
    ASSERT_EQ(event.type, TunerEventType::Error);

    // Verify severity is Critical for errors
    ASSERT_EQ(event.severity, Severity::Critical);

    // Verify payload
    ASSERT_EQ(event.payload.error.error_code, ERROR_JSON_PARSE_FAILED);
    ASSERT_EQ(event.payload.error.is_recoverable, 1);
    ASSERT_STREQ(event.payload.error.component, "claude_client");

    // Verify reason
    ASSERT_TRUE(std::strstr(event.reason, "No valid JSON") != nullptr);

    // Verify symbol is "*" (global event)
    ASSERT_STREQ(event.symbol, "*");
}

TEST(error_event_non_recoverable) {
    auto event = TunerEvent::make_error("binance_ws", ERROR_WS_DISCONNECT,
                                        false, // not recoverable
                                        "WebSocket connection lost permanently");

    ASSERT_EQ(event.type, TunerEventType::Error);
    ASSERT_EQ(event.payload.error.error_code, ERROR_WS_DISCONNECT);
    ASSERT_EQ(event.payload.error.is_recoverable, 0);
    ASSERT_STREQ(event.payload.error.component, "binance_ws");
}

TEST(error_event_component_truncation) {
    // Component field is 24 bytes, test truncation
    auto event =
        TunerEvent::make_error("very_long_component_name_that_exceeds_24_bytes", ERROR_API_TIMEOUT, true, "Timeout");

    // Should be truncated to 23 chars + null
    ASSERT_TRUE(strlen(event.payload.error.component) <= 23);
}

TEST(error_event_is_system_event) {
    auto event = TunerEvent::make_error("test", 1001, true, "test error");

    // Error type (51) is >= 48, so it's a system event
    ASSERT_TRUE(event.is_system_event());
    ASSERT_FALSE(event.is_trade_event());
    ASSERT_FALSE(event.is_tuner_event());
    ASSERT_FALSE(event.is_market_event());
}

// ============================================================================
// SharedEventLog Error Event Tests
// ============================================================================

TEST(log_error_event) {
    SharedEventLog::destroy(TEST_SHM_NAME);
    SharedEventLog* log = SharedEventLog::create(TEST_SHM_NAME);
    ASSERT_TRUE(log != nullptr);

    // Create and log an error event
    auto error_event =
        TunerEvent::make_error("claude_client", ERROR_JSON_PARSE_FAILED, true, "No valid JSON in response");
    log->log(error_event);

    // Verify event was logged
    ASSERT_EQ(log->total_events.load(), 1u);

    // Retrieve the event
    const TunerEvent* retrieved = log->get_event(0);
    ASSERT_TRUE(retrieved != nullptr);
    ASSERT_EQ(retrieved->type, TunerEventType::Error);
    ASSERT_EQ(retrieved->payload.error.error_code, ERROR_JSON_PARSE_FAILED);

    SharedEventLog::destroy(TEST_SHM_NAME);
}

TEST(filter_error_events_from_mixed_log) {
    SharedEventLog::destroy(TEST_SHM_NAME);
    SharedEventLog* log = SharedEventLog::create(TEST_SHM_NAME);
    ASSERT_TRUE(log != nullptr);

    // Log mixed events: signal, error, fill, error, config change
    auto signal = TunerEvent::make_signal("BTCUSDT", TradeSide::Buy, 50000.0, 0.1, "Test signal");
    log->log(signal);

    auto error1 = TunerEvent::make_error("claude_client", ERROR_JSON_PARSE_FAILED, true, "JSON error 1");
    log->log(error1);

    auto fill = TunerEvent::make_fill("BTCUSDT", TradeSide::Buy, 50000.0, 0.1, 100, "Test fill");
    log->log(fill);

    auto error2 = TunerEvent::make_error("binance_ws", ERROR_WS_DISCONNECT, false, "WS disconnected");
    log->log(error2);

    auto config = TunerEvent::make_config_change("BTCUSDT", "cooldown_ms", 2000, 5000, 80, "Test config");
    log->log(config);

    // Verify total events
    ASSERT_EQ(log->total_events.load(), 5u);

    // Filter for error events
    TunerEvent all_events[10];
    size_t count = log->get_events_since(0, all_events, 10);
    ASSERT_EQ(count, 5u);

    // Count error events manually
    int error_count = 0;
    for (size_t i = 0; i < count; ++i) {
        if (all_events[i].type == TunerEventType::Error) {
            ++error_count;
        }
    }
    ASSERT_EQ(error_count, 2);

    // Verify error events have correct data
    std::vector<const TunerEvent*> errors;
    for (size_t i = 0; i < count; ++i) {
        if (all_events[i].type == TunerEventType::Error) {
            errors.push_back(&all_events[i]);
        }
    }

    ASSERT_EQ(errors.size(), 2u);
    ASSERT_EQ(errors[0]->payload.error.error_code, ERROR_JSON_PARSE_FAILED);
    ASSERT_EQ(errors[1]->payload.error.error_code, ERROR_WS_DISCONNECT);

    SharedEventLog::destroy(TEST_SHM_NAME);
}

TEST(multiple_error_events_ring_buffer) {
    SharedEventLog::destroy(TEST_SHM_NAME);
    SharedEventLog* log = SharedEventLog::create(TEST_SHM_NAME);
    ASSERT_TRUE(log != nullptr);

    // Log multiple error events
    for (int i = 0; i < 10; ++i) {
        char reason[128];
        snprintf(reason, sizeof(reason), "Error %d", i);
        auto event = TunerEvent::make_error("tuner", 1000 + i, true, reason);
        log->log(event);
    }

    ASSERT_EQ(log->total_events.load(), 10u);

    // Get all events and verify they're all errors
    TunerEvent events[15];
    size_t count = log->get_events_since(0, events, 15);
    ASSERT_EQ(count, 10u);

    for (size_t i = 0; i < count; ++i) {
        ASSERT_EQ(events[i].type, TunerEventType::Error);
        ASSERT_EQ(events[i].payload.error.error_code, 1000 + static_cast<int32_t>(i));
    }

    SharedEventLog::destroy(TEST_SHM_NAME);
}

TEST(error_event_type_name) {
    auto event = TunerEvent::make_error("test", 1001, true, "test");

    ASSERT_STREQ(event.type_name(), "ERROR");
}

TEST(error_event_codes_match_plan) {
    // Verify error codes match the plan
    ASSERT_EQ(ERROR_JSON_PARSE_FAILED, 1001);
    ASSERT_EQ(ERROR_API_FAILED, 1002);
    ASSERT_EQ(ERROR_API_TIMEOUT, 1003);
    ASSERT_EQ(ERROR_INVALID_COMMAND, 1004);
    ASSERT_EQ(ERROR_WS_DISCONNECT, 2001);
    ASSERT_EQ(ERROR_HEARTBEAT_TIMEOUT, 2002);
    ASSERT_EQ(ERROR_POSITION_LIMIT, 2003);
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "\n=== Error Event Tests (TunerEvent::Error) ===\n\n";

    std::cout << "TunerEvent::make_error:\n";
    RUN_TEST(error_event_creation);
    RUN_TEST(error_event_non_recoverable);
    RUN_TEST(error_event_component_truncation);
    RUN_TEST(error_event_is_system_event);
    RUN_TEST(error_event_type_name);
    RUN_TEST(error_event_codes_match_plan);

    std::cout << "\nSharedEventLog with Error Events:\n";
    RUN_TEST(log_error_event);
    RUN_TEST(filter_error_events_from_mixed_log);
    RUN_TEST(multiple_error_events_ring_buffer);

    std::cout << "\n=== All Error Event Tests Passed! ===\n";
    return 0;
}
