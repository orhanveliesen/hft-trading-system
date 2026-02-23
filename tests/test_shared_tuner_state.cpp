#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include "../include/ipc/shared_tuner_state.hpp"

using namespace hft::ipc;

#define TEST(name) void name()
#define RUN_TEST(name) do { \
    std::cout << "  " << #name << "... "; \
    name(); \
    std::cout << "PASSED\n"; \
} while(0)

#define ASSERT_EQ(a, b) assert((a) == (b))
#define ASSERT_TRUE(x) assert(x)
#define ASSERT_FALSE(x) assert(!(x))
#define ASSERT_NEAR(a, b, eps) assert(std::abs((a) - (b)) < (eps))

constexpr const char* TEST_SHM_NAME = "/tuner_state_test";

// ============================================================================
// TunerDecision Tests
// ============================================================================

TEST(decision_clear_and_init) {
    TunerDecision d;
    d.clear();

    ASSERT_EQ(d.timestamp_ns, 0u);
    ASSERT_EQ(d.sequence, 0u);
    ASSERT_EQ(d.confidence, 0);
    ASSERT_EQ(d.num_changes, 0);
    ASSERT_EQ(d.symbol[0], '\0');
    ASSERT_EQ(d.reason[0], '\0');
}

TEST(decision_set_symbol) {
    TunerDecision d;
    d.clear();

    d.set_symbol("BTCUSDT");
    ASSERT_TRUE(strcmp(d.symbol, "BTCUSDT") == 0);

    // Test truncation for long symbols
    d.set_symbol("VERYLONGSYMBOLNAME12345");
    ASSERT_EQ(strlen(d.symbol), TUNER_SYMBOL_LEN - 1);
}

TEST(decision_set_reason) {
    TunerDecision d;
    d.clear();

    d.set_reason("Win rate is too low at 25%");
    ASSERT_TRUE(strcmp(d.reason, "Win rate is too low at 25%") == 0);

    // Test long reason (should truncate)
    std::string long_reason(300, 'x');
    d.set_reason(long_reason.c_str());
    ASSERT_EQ(strlen(d.reason), MAX_REASON_LEN - 1);
}

TEST(decision_add_changes) {
    TunerDecision d;
    d.clear();

    // Add changes
    ASSERT_TRUE(d.add_change(TunerParam::Cooldown, 2000.0f, 5000.0f));
    ASSERT_TRUE(d.add_change(TunerParam::TargetPct, 1.5f, 2.5f));
    ASSERT_TRUE(d.add_change(TunerParam::EmaDevTrend, 0.8f, 1.2f));

    ASSERT_EQ(d.num_changes, 3);
    ASSERT_TRUE(d.has_changes());

    // Verify values
    ASSERT_EQ(d.changes[0].param, static_cast<uint8_t>(TunerParam::Cooldown));
    ASSERT_NEAR(d.changes[0].old_value, 2000.0f, 0.001f);
    ASSERT_NEAR(d.changes[0].new_value, 5000.0f, 0.001f);

    ASSERT_EQ(d.changes[1].param, static_cast<uint8_t>(TunerParam::TargetPct));
    ASSERT_NEAR(d.changes[1].old_value, 1.5f, 0.001f);
    ASSERT_NEAR(d.changes[1].new_value, 2.5f, 0.001f);
}

TEST(decision_max_changes) {
    TunerDecision d;
    d.clear();

    // Fill up to max
    for (int i = 0; i < MAX_PARAM_CHANGES; ++i) {
        ASSERT_TRUE(d.add_change(TunerParam::Cooldown, i * 1.0f, (i + 1) * 1.0f));
    }
    ASSERT_EQ(d.num_changes, MAX_PARAM_CHANGES);

    // Try to add one more - should fail
    ASSERT_FALSE(d.add_change(TunerParam::Cooldown, 100.0f, 200.0f));
    ASSERT_EQ(d.num_changes, MAX_PARAM_CHANGES);
}

TEST(decision_is_valid) {
    TunerDecision d;
    d.clear();

    ASSERT_FALSE(d.is_valid());  // sequence == 0

    d.sequence = 1;
    ASSERT_TRUE(d.is_valid());
}

// ============================================================================
// SharedTunerState Tests
// ============================================================================

TEST(state_create_and_init) {
    SharedTunerState::destroy(TEST_SHM_NAME);  // Clean up any previous

    SharedTunerState* state = SharedTunerState::create(TEST_SHM_NAME);
    ASSERT_TRUE(state != nullptr);

    // Check magic and version
    ASSERT_TRUE(state->is_valid());
    ASSERT_EQ(state->magic, SharedTunerState::MAGIC);
    ASSERT_EQ(state->version, SharedTunerState::VERSION);

    // Initial state
    ASSERT_EQ(state->write_index.load(), 0u);
    ASSERT_EQ(state->total_decisions.load(), 0u);
    ASSERT_EQ(state->available_count(), 0u);

    SharedTunerState::close(state);
    SharedTunerState::destroy(TEST_SHM_NAME);
}

TEST(state_open_existing) {
    SharedTunerState::destroy(TEST_SHM_NAME);

    // Create
    SharedTunerState* writer = SharedTunerState::create(TEST_SHM_NAME);

    // Write a decision
    auto* d = writer->write_next();
    d->set_symbol("BTCUSDT");
    d->confidence = 75;
    writer->commit_write();

    // Open from "another process" (read-only)
    SharedTunerState* reader = SharedTunerState::open(TEST_SHM_NAME);
    ASSERT_TRUE(reader != nullptr);
    ASSERT_TRUE(reader->is_valid());

    // Read the decision
    const auto* latest = reader->get_latest();
    ASSERT_TRUE(latest != nullptr);
    ASSERT_TRUE(strcmp(latest->symbol, "BTCUSDT") == 0);
    ASSERT_EQ(latest->confidence, 75);

    SharedTunerState::close(reader);
    SharedTunerState::close(writer);
    SharedTunerState::destroy(TEST_SHM_NAME);
}

TEST(state_write_and_read) {
    SharedTunerState::destroy(TEST_SHM_NAME);
    SharedTunerState* state = SharedTunerState::create(TEST_SHM_NAME);

    // Write first decision
    auto* d1 = state->write_next();
    d1->set_symbol("BTCUSDT");
    d1->set_reason("Win rate 25% is unsustainable");
    d1->confidence = 80;
    d1->action = 1;  // UpdateSymbolConfig
    d1->add_change(TunerParam::Cooldown, 2000, 5000);
    d1->add_change(TunerParam::TargetPct, 2.0, 3.0);
    state->commit_write();

    ASSERT_EQ(state->total_decisions.load(), 1u);
    ASSERT_EQ(state->available_count(), 1u);

    // Verify
    const auto* latest = state->get_latest();
    ASSERT_TRUE(latest != nullptr);
    ASSERT_TRUE(strcmp(latest->symbol, "BTCUSDT") == 0);
    ASSERT_TRUE(strstr(latest->reason, "Win rate") != nullptr);
    ASSERT_EQ(latest->confidence, 80);
    ASSERT_EQ(latest->num_changes, 2);
    ASSERT_EQ(latest->sequence, 1u);

    SharedTunerState::close(state);
    SharedTunerState::destroy(TEST_SHM_NAME);
}

TEST(state_ring_buffer_wrap) {
    SharedTunerState::destroy(TEST_SHM_NAME);
    SharedTunerState* state = SharedTunerState::create(TEST_SHM_NAME);

    // Fill the ring buffer completely
    for (uint32_t i = 0; i < MAX_TUNER_HISTORY + 5; ++i) {
        auto* d = state->write_next();
        char sym[16];
        snprintf(sym, sizeof(sym), "SYM%u", i);
        d->set_symbol(sym);
        d->confidence = i % 100;
        state->commit_write();
    }

    // Total should track all writes
    ASSERT_EQ(state->total_decisions.load(), MAX_TUNER_HISTORY + 5);

    // Available should be capped at ring buffer size
    ASSERT_EQ(state->available_count(), MAX_TUNER_HISTORY);

    // Latest should be the most recent
    const auto* latest = state->get_latest();
    ASSERT_TRUE(latest != nullptr);
    char expected_sym[16];
    snprintf(expected_sym, sizeof(expected_sym), "SYM%lu", MAX_TUNER_HISTORY + 4);
    ASSERT_TRUE(strcmp(latest->symbol, expected_sym) == 0);

    SharedTunerState::close(state);
    SharedTunerState::destroy(TEST_SHM_NAME);
}

TEST(state_get_by_offset) {
    SharedTunerState::destroy(TEST_SHM_NAME);
    SharedTunerState* state = SharedTunerState::create(TEST_SHM_NAME);

    // Write 3 decisions
    for (int i = 0; i < 3; ++i) {
        auto* d = state->write_next();
        d->confidence = (i + 1) * 10;  // 10, 20, 30
        state->commit_write();
    }

    // offset=0 is latest (confidence 30)
    const auto* d0 = state->get_by_offset(0);
    ASSERT_TRUE(d0 != nullptr);
    ASSERT_EQ(d0->confidence, 30);

    // offset=1 is second most recent (confidence 20)
    const auto* d1 = state->get_by_offset(1);
    ASSERT_TRUE(d1 != nullptr);
    ASSERT_EQ(d1->confidence, 20);

    // offset=2 is oldest (confidence 10)
    const auto* d2 = state->get_by_offset(2);
    ASSERT_TRUE(d2 != nullptr);
    ASSERT_EQ(d2->confidence, 10);

    // offset=3 should be nullptr (doesn't exist)
    const auto* d3 = state->get_by_offset(3);
    ASSERT_TRUE(d3 == nullptr);

    SharedTunerState::close(state);
    SharedTunerState::destroy(TEST_SHM_NAME);
}

TEST(state_for_recent_decisions) {
    SharedTunerState::destroy(TEST_SHM_NAME);
    SharedTunerState* state = SharedTunerState::create(TEST_SHM_NAME);

    // Write 5 decisions
    for (int i = 0; i < 5; ++i) {
        auto* d = state->write_next();
        d->confidence = (i + 1) * 10;  // 10, 20, 30, 40, 50
        state->commit_write();
    }

    // Iterate over last 3 (should be 50, 40, 30 in order)
    std::vector<uint8_t> observed;
    state->for_recent_decisions(3, [&](const TunerDecision& d) {
        observed.push_back(d.confidence);
    });

    ASSERT_EQ(observed.size(), 3u);
    ASSERT_EQ(observed[0], 50);  // newest first
    ASSERT_EQ(observed[1], 40);
    ASSERT_EQ(observed[2], 30);

    SharedTunerState::close(state);
    SharedTunerState::destroy(TEST_SHM_NAME);
}

TEST(state_has_new_since) {
    SharedTunerState::destroy(TEST_SHM_NAME);
    SharedTunerState* state = SharedTunerState::create(TEST_SHM_NAME);

    ASSERT_FALSE(state->has_new_since(0));

    // Write one
    auto* d = state->write_next();
    d->confidence = 50;
    state->commit_write();

    ASSERT_TRUE(state->has_new_since(0));
    ASSERT_FALSE(state->has_new_since(1));

    // Write another
    d = state->write_next();
    d->confidence = 60;
    state->commit_write();

    ASSERT_TRUE(state->has_new_since(0));
    ASSERT_TRUE(state->has_new_since(1));
    ASSERT_FALSE(state->has_new_since(2));

    SharedTunerState::close(state);
    SharedTunerState::destroy(TEST_SHM_NAME);
}

TEST(state_cross_thread_visibility) {
    SharedTunerState::destroy(TEST_SHM_NAME);
    SharedTunerState* writer = SharedTunerState::create(TEST_SHM_NAME);

    std::atomic<bool> ready{false};
    std::atomic<bool> done{false};
    uint8_t observed_confidence = 0;

    // Reader thread
    std::thread reader([&]() {
        SharedTunerState* reader_state = SharedTunerState::open(TEST_SHM_NAME);
        ready.store(true);

        // Wait for writer
        while (!done.load()) {
            const auto* latest = reader_state->get_latest();
            if (latest != nullptr) {
                observed_confidence = latest->confidence;
            }
            std::this_thread::yield();
        }

        SharedTunerState::close(reader_state);
    });

    // Wait for reader ready
    while (!ready.load()) {
        std::this_thread::yield();
    }

    // Write a decision
    auto* d = writer->write_next();
    d->confidence = 99;
    writer->commit_write();

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    done.store(true);

    reader.join();

    // Reader should have seen the value
    ASSERT_EQ(observed_confidence, 99);

    SharedTunerState::close(writer);
    SharedTunerState::destroy(TEST_SHM_NAME);
}

TEST(state_size_check) {
    // TunerDecision should be 376 bytes
    ASSERT_EQ(sizeof(TunerDecision), 376u);

    // ParamChange should be 12 bytes
    ASSERT_EQ(sizeof(ParamChange), 12u);

    // SharedTunerState header should be 64 bytes (cache line)
    // Total should be 64 + (376 * 16) = 64 + 6016 = 6080
    ASSERT_EQ(sizeof(SharedTunerState), 64u + (376u * MAX_TUNER_HISTORY));
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "\n=== SharedTunerState Tests ===\n\n";

    // TunerDecision tests
    std::cout << "TunerDecision:\n";
    RUN_TEST(decision_clear_and_init);
    RUN_TEST(decision_set_symbol);
    RUN_TEST(decision_set_reason);
    RUN_TEST(decision_add_changes);
    RUN_TEST(decision_max_changes);
    RUN_TEST(decision_is_valid);

    // SharedTunerState tests
    std::cout << "\nSharedTunerState:\n";
    RUN_TEST(state_create_and_init);
    RUN_TEST(state_open_existing);
    RUN_TEST(state_write_and_read);
    RUN_TEST(state_ring_buffer_wrap);
    RUN_TEST(state_get_by_offset);
    RUN_TEST(state_for_recent_decisions);
    RUN_TEST(state_has_new_since);
    RUN_TEST(state_cross_thread_visibility);
    RUN_TEST(state_size_check);

    std::cout << "\n=== All SharedTunerState Tests Passed! ===\n";
    return 0;
}
