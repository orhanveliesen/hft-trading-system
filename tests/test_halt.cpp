#include "../include/trading/halt.hpp"
#include "../include/trading/trading_state.hpp"

#include <cassert>
#include <chrono>
#include <iostream>

using namespace hft::trading;

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

// =============================================================================
// trigger_halt Tests
// =============================================================================

TEST(trigger_halt_from_running) {
    TradingState state{};
    state.init(100000.0);

    ASSERT_EQ(state.halt.halted.load(), static_cast<uint8_t>(HaltStatus::RUNNING));

    trigger_halt(state, HaltReason::MANUAL);

    ASSERT_EQ(state.halt.halted.load(), static_cast<uint8_t>(HaltStatus::HALTING));
    ASSERT_EQ(state.halt.reason.load(), static_cast<uint8_t>(HaltReason::MANUAL));
    ASSERT_TRUE(state.halt.halt_time_ns.load() > 0);
}

TEST(trigger_halt_idempotent) {
    TradingState state{};
    state.init(100000.0);

    trigger_halt(state, HaltReason::RISK_LIMIT);
    uint64_t first_time = state.halt.halt_time_ns.load();

    // Second halt attempt should not change anything
    trigger_halt(state, HaltReason::SYSTEM_ERROR);

    // Reason should still be RISK_LIMIT (first one wins)
    ASSERT_EQ(state.halt.reason.load(), static_cast<uint8_t>(HaltReason::RISK_LIMIT));
    ASSERT_EQ(state.halt.halt_time_ns.load(), first_time);
}

TEST(trigger_halt_sets_timestamp) {
    TradingState state{};
    state.init(100000.0);

    auto before = std::chrono::steady_clock::now().time_since_epoch().count();
    trigger_halt(state, HaltReason::CONNECTION_LOST);
    auto after = std::chrono::steady_clock::now().time_since_epoch().count();

    uint64_t halt_time = state.halt.halt_time_ns.load();
    ASSERT_TRUE(halt_time >= static_cast<uint64_t>(before));
    ASSERT_TRUE(halt_time <= static_cast<uint64_t>(after));
}

// =============================================================================
// flatten_all_positions Tests
// =============================================================================

TEST(flatten_all_positions_sets_exit_flags) {
    TradingState state{};
    state.init(100000.0);

    // Set up some positions
    state.positions.quantity[0] = 0.5; // Position in BTC
    state.positions.quantity[1] = 2.0; // Position in ETH
    state.positions.quantity[2] = 0.0; // No position
    state.positions.quantity[3] = 1.5; // Position in SOL

    state.flags.flags[0] |= SymbolFlags::FLAG_HAS_POSITION;
    state.flags.flags[1] |= SymbolFlags::FLAG_HAS_POSITION;
    state.flags.flags[3] |= SymbolFlags::FLAG_HAS_POSITION;

    flatten_all_positions(state);

    // Symbols with positions should have EXIT_REQUESTED
    ASSERT_TRUE(state.flags.flags[0] & SymbolFlags::FLAG_EXIT_REQUESTED);
    ASSERT_TRUE(state.flags.flags[1] & SymbolFlags::FLAG_EXIT_REQUESTED);
    ASSERT_FALSE(state.flags.flags[2] & SymbolFlags::FLAG_EXIT_REQUESTED);
    ASSERT_TRUE(state.flags.flags[3] & SymbolFlags::FLAG_EXIT_REQUESTED);
}

TEST(flatten_all_positions_sets_halted_status) {
    TradingState state{};
    state.init(100000.0);

    state.halt.halted.store(static_cast<uint8_t>(HaltStatus::HALTING));

    flatten_all_positions(state);

    ASSERT_EQ(state.halt.halted.load(), static_cast<uint8_t>(HaltStatus::HALTED));
}

// =============================================================================
// reset_halt Tests
// =============================================================================

TEST(reset_halt_clears_state) {
    TradingState state{};
    state.init(100000.0);

    // Set up halted state
    state.halt.halted.store(static_cast<uint8_t>(HaltStatus::HALTED));
    state.halt.reason.store(static_cast<uint8_t>(HaltReason::RISK_LIMIT));
    state.halt.halt_time_ns.store(12345);
    state.risk_state.risk_halted.store(1);

    reset_halt(state);

    ASSERT_EQ(state.halt.halted.load(), static_cast<uint8_t>(HaltStatus::RUNNING));
    ASSERT_EQ(state.halt.reason.load(), static_cast<uint8_t>(HaltReason::NONE));
    ASSERT_EQ(state.risk_state.risk_halted.load(), 0u);
}

// =============================================================================
// is_halted helper Tests
// =============================================================================

TEST(is_halted_returns_true_when_halting) {
    TradingState state{};
    state.init(100000.0);

    state.halt.halted.store(static_cast<uint8_t>(HaltStatus::HALTING));
    ASSERT_TRUE(is_halted(state));
}

TEST(is_halted_returns_true_when_halted) {
    TradingState state{};
    state.init(100000.0);

    state.halt.halted.store(static_cast<uint8_t>(HaltStatus::HALTED));
    ASSERT_TRUE(is_halted(state));
}

TEST(is_halted_returns_false_when_running) {
    TradingState state{};
    state.init(100000.0);

    ASSERT_FALSE(is_halted(state));
}

// =============================================================================
// can_trade helper Tests
// =============================================================================

TEST(can_trade_returns_true_when_running) {
    TradingState state{};
    state.init(100000.0);

    ASSERT_TRUE(can_trade(state));
}

TEST(can_trade_returns_false_when_halted) {
    TradingState state{};
    state.init(100000.0);

    state.halt.halted.store(static_cast<uint8_t>(HaltStatus::HALTED));
    ASSERT_FALSE(can_trade(state));
}

TEST(can_trade_returns_false_when_risk_halted) {
    TradingState state{};
    state.init(100000.0);

    state.risk_state.risk_halted.store(1);
    ASSERT_FALSE(can_trade(state));
}

// =============================================================================
// Full halt sequence Tests
// =============================================================================

TEST(full_halt_sequence) {
    TradingState state{};
    state.init(100000.0);

    // Set up positions
    state.positions.quantity[0] = 0.5;
    state.positions.quantity[1] = 1.0;
    state.flags.flags[0] |= SymbolFlags::FLAG_HAS_POSITION;
    state.flags.flags[1] |= SymbolFlags::FLAG_HAS_POSITION;

    // 1. System is running
    ASSERT_TRUE(can_trade(state));

    // 2. Trigger halt
    trigger_halt(state, HaltReason::RISK_LIMIT);
    ASSERT_FALSE(can_trade(state));
    ASSERT_EQ(state.halt.halted.load(), static_cast<uint8_t>(HaltStatus::HALTING));

    // 3. Flatten positions
    flatten_all_positions(state);
    ASSERT_TRUE(state.flags.flags[0] & SymbolFlags::FLAG_EXIT_REQUESTED);
    ASSERT_TRUE(state.flags.flags[1] & SymbolFlags::FLAG_EXIT_REQUESTED);
    ASSERT_EQ(state.halt.halted.load(), static_cast<uint8_t>(HaltStatus::HALTED));

    // 4. Reset halt
    reset_halt(state);
    ASSERT_TRUE(can_trade(state));
    ASSERT_EQ(state.halt.halted.load(), static_cast<uint8_t>(HaltStatus::RUNNING));
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "\n=== Halt Management Tests ===\n\n";

    std::cout << "trigger_halt Tests:\n";
    RUN_TEST(trigger_halt_from_running);
    RUN_TEST(trigger_halt_idempotent);
    RUN_TEST(trigger_halt_sets_timestamp);

    std::cout << "\nflatten_all_positions Tests:\n";
    RUN_TEST(flatten_all_positions_sets_exit_flags);
    RUN_TEST(flatten_all_positions_sets_halted_status);

    std::cout << "\nreset_halt Tests:\n";
    RUN_TEST(reset_halt_clears_state);

    std::cout << "\nis_halted Tests:\n";
    RUN_TEST(is_halted_returns_true_when_halting);
    RUN_TEST(is_halted_returns_true_when_halted);
    RUN_TEST(is_halted_returns_false_when_running);

    std::cout << "\ncan_trade Tests:\n";
    RUN_TEST(can_trade_returns_true_when_running);
    RUN_TEST(can_trade_returns_false_when_halted);
    RUN_TEST(can_trade_returns_false_when_risk_halted);

    std::cout << "\nFull Halt Sequence Tests:\n";
    RUN_TEST(full_halt_sequence);

    std::cout << "\n=== All Halt Management Tests Passed! ===\n";
    return 0;
}
