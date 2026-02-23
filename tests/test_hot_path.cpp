#include "../include/trading/hot_path.hpp"
#include "../include/trading/trading_state.hpp"

#include <cassert>
#include <cmath>
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
#define ASSERT_NEAR(a, b, eps) assert(std::abs((a) - (b)) < (eps))
#define ASSERT_GT(a, b) assert((a) > (b))
#define ASSERT_LT(a, b) assert((a) < (b))

// =============================================================================
// calculate_position_size Tests
// =============================================================================

TEST(calculate_position_size_uses_config) {
    TradingState state{};
    state.init(100000.0);

    // 1% position size
    state.common.position_size_pct[0] = 0.01;

    double size = calculate_position_size(0, 50000.0, state);

    // 1% of 100000 = 1000, at price 50000 = 0.02 units
    ASSERT_NEAR(size, 0.02, 0.001);
}

TEST(calculate_position_size_respects_max_position) {
    TradingState state{};
    state.init(100000.0);

    state.common.position_size_pct[0] = 0.10; // 10% = 10000
    state.risk_limits.max_position[0] = 5000; // But max 5000 notional

    double size = calculate_position_size(0, 100.0, state);

    // Should be limited by max_position: 5000/100 = 50 units
    ASSERT_NEAR(size, 50.0, 0.1);
}

TEST(calculate_position_size_zero_on_no_cash) {
    TradingState state{};
    state.init(0.0);

    state.common.position_size_pct[0] = 0.01;

    double size = calculate_position_size(0, 100.0, state);

    ASSERT_NEAR(size, 0.0, 0.001);
}

// =============================================================================
// check_stop_target Tests
// =============================================================================

TEST(check_stop_target_stop_hit) {
    TradingState state{};
    state.init(100000.0);

    // Position with entry at 100
    state.positions.quantity[0] = 1.0;
    state.positions.avg_entry[0] = 100.0;
    state.common.stop_pct[0] = 0.05;   // 5% stop
    state.common.target_pct[0] = 0.10; // 10% target

    // Price at 94 = -6% loss (below 5% stop)
    ExitResult result = check_stop_target(0, 94.0, state);

    ASSERT_TRUE(result.should_exit);
    ASSERT_EQ(result.reason, HotPathExitReason::STOP);
}

TEST(check_stop_target_target_hit) {
    TradingState state{};
    state.init(100000.0);

    state.positions.quantity[0] = 1.0;
    state.positions.avg_entry[0] = 100.0;
    state.common.stop_pct[0] = 0.05;
    state.common.target_pct[0] = 0.10;

    // Price at 112 = +12% gain (above 10% target)
    ExitResult result = check_stop_target(0, 112.0, state);

    ASSERT_TRUE(result.should_exit);
    ASSERT_EQ(result.reason, HotPathExitReason::TARGET);
}

TEST(check_stop_target_no_exit_in_range) {
    TradingState state{};
    state.init(100000.0);

    state.positions.quantity[0] = 1.0;
    state.positions.avg_entry[0] = 100.0;
    state.common.stop_pct[0] = 0.05;
    state.common.target_pct[0] = 0.10;

    // Price at 102 = +2% gain (between stop and target)
    ExitResult result = check_stop_target(0, 102.0, state);

    ASSERT_FALSE(result.should_exit);
    ASSERT_EQ(result.reason, HotPathExitReason::NONE);
}

TEST(check_stop_target_no_position_no_exit) {
    TradingState state{};
    state.init(100000.0);

    state.positions.quantity[0] = 0.0;

    ExitResult result = check_stop_target(0, 100.0, state);

    ASSERT_FALSE(result.should_exit);
}

// =============================================================================
// check_tuner_signal Tests
// =============================================================================

TEST(check_tuner_signal_buy_signal) {
    TradingState state{};
    state.init(100000.0);

    // Fresh buy signal
    state.signals.signal[0] = 1;
    state.signals.quantity[0] = 0.5;
    state.signals.timestamp_ns[0] = now_ns();

    TunerAction action = check_tuner_signal(0, state);

    ASSERT_EQ(action.action, TunerActionType::BUY);
    ASSERT_NEAR(action.quantity, 0.5, 0.001);
}

TEST(check_tuner_signal_sell_signal) {
    TradingState state{};
    state.init(100000.0);

    state.signals.signal[0] = -1;
    state.signals.quantity[0] = 0.3;
    state.signals.timestamp_ns[0] = now_ns();

    TunerAction action = check_tuner_signal(0, state);

    ASSERT_EQ(action.action, TunerActionType::SELL);
    ASSERT_NEAR(action.quantity, 0.3, 0.001);
}

TEST(check_tuner_signal_expired_signal_ignored) {
    TradingState state{};
    state.init(100000.0);

    // Signal from 10 seconds ago (past TTL)
    state.signals.signal[0] = 1;
    state.signals.quantity[0] = 0.5;
    state.signals.timestamp_ns[0] = now_ns() - 10'000'000'000ULL;

    TunerAction action = check_tuner_signal(0, state);

    ASSERT_EQ(action.action, TunerActionType::NONE);
}

TEST(check_tuner_signal_no_signal) {
    TradingState state{};
    state.init(100000.0);

    state.signals.signal[0] = 0;

    TunerAction action = check_tuner_signal(0, state);

    ASSERT_EQ(action.action, TunerActionType::NONE);
}

// =============================================================================
// check_flags Tests
// =============================================================================

TEST(check_flags_exit_requested) {
    TradingState state{};
    state.init(100000.0);

    state.flags.flags[0] = SymbolFlags::FLAG_EXIT_REQUESTED;

    FlagAction action = check_flags(0, state);

    ASSERT_EQ(action.action, FlagActionType::EXIT);
}

TEST(check_flags_trading_paused) {
    TradingState state{};
    state.init(100000.0);

    state.flags.flags[0] = SymbolFlags::FLAG_TRADING_PAUSED;

    FlagAction action = check_flags(0, state);

    ASSERT_EQ(action.action, FlagActionType::SKIP);
}

TEST(check_flags_no_flags) {
    TradingState state{};
    state.init(100000.0);

    state.flags.flags[0] = 0;

    FlagAction action = check_flags(0, state);

    ASSERT_EQ(action.action, FlagActionType::CONTINUE);
}

// =============================================================================
// generate_signal Tests
// =============================================================================

TEST(generate_signal_buy_on_positive_score) {
    TradingState state{};
    state.init(100000.0);

    state.strategies.active[0] = StrategyId::RSI;
    state.positions.quantity[0] = 0.0; // No position

    // Score > threshold should signal buy
    TradeSignal signal = generate_signal(0, 0.6, 100.0, state);

    ASSERT_EQ(signal.action, SignalAction::BUY);
    ASSERT_GT(signal.quantity, 0.0);
}

TEST(generate_signal_no_buy_with_position) {
    TradingState state{};
    state.init(100000.0);

    state.positions.quantity[0] = 1.0; // Has position

    // Positive score but already have position
    TradeSignal signal = generate_signal(0, 0.6, 100.0, state);

    ASSERT_EQ(signal.action, SignalAction::HOLD);
}

TEST(generate_signal_hold_on_weak_score) {
    TradingState state{};
    state.init(100000.0);

    state.positions.quantity[0] = 0.0;

    // Weak score below threshold
    TradeSignal signal = generate_signal(0, 0.1, 100.0, state);

    ASSERT_EQ(signal.action, SignalAction::HOLD);
}

TEST(generate_signal_sell_on_negative_score_with_profit) {
    TradingState state{};
    state.init(100000.0);

    state.positions.quantity[0] = 1.0;
    state.positions.avg_entry[0] = 90.0;
    state.common.min_profit_for_exit[0] = 0.05; // 5% min profit

    // Price at 100 = 11% profit, negative score
    TradeSignal signal = generate_signal(0, -0.6, 100.0, state);

    ASSERT_EQ(signal.action, SignalAction::SELL);
}

TEST(generate_signal_hold_on_negative_score_no_profit) {
    TradingState state{};
    state.init(100000.0);

    state.positions.quantity[0] = 1.0;
    state.positions.avg_entry[0] = 100.0;
    state.common.min_profit_for_exit[0] = 0.05;

    // Price at 102 = 2% profit (below 5% min), negative score
    TradeSignal signal = generate_signal(0, -0.6, 102.0, state);

    ASSERT_EQ(signal.action, SignalAction::HOLD);
}

// =============================================================================
// process_price_update Tests (Integration)
// =============================================================================

TEST(process_price_update_updates_current_price) {
    TradingState state{};
    state.init(100000.0);

    process_price_update(0, 12345.67, state);

    ASSERT_NEAR(state.positions.current_price[0], 12345.67, 0.01);
}

TEST(process_price_update_skips_when_halted) {
    TradingState state{};
    state.init(100000.0);

    state.halt.halted.store(static_cast<uint8_t>(HaltStatus::HALTED));
    state.positions.quantity[0] = 1.0;
    state.positions.avg_entry[0] = 100.0;
    state.common.stop_pct[0] = 0.01; // 1% stop

    // Price drops 5% but system is halted
    process_price_update(0, 95.0, state);

    // Position should NOT be closed
    ASSERT_NEAR(state.positions.quantity[0], 1.0, 0.001);
}

TEST(process_price_update_handles_tuner_signal) {
    TradingState state{};
    state.init(100000.0);

    state.common.position_size_pct[0] = 0.01;
    state.signals.signal[0] = 1; // Buy signal
    state.signals.quantity[0] = 0.5;
    state.signals.timestamp_ns[0] = now_ns();

    // Should execute tuner signal
    process_price_update(0, 100.0, state);

    // Signal should be consumed
    ASSERT_EQ(state.signals.signal[0], 0);
}

TEST(process_price_update_triggers_stop) {
    TradingState state{};
    state.init(100000.0);

    state.positions.quantity[0] = 1.0;
    state.positions.avg_entry[0] = 100.0;
    state.flags.flags[0] = SymbolFlags::FLAG_HAS_POSITION;
    state.common.stop_pct[0] = 0.05;
    state.common.target_pct[0] = 0.10;

    // Price drops 6% - should trigger stop
    process_price_update(0, 94.0, state);

    // Position should be exited
    ASSERT_NEAR(state.positions.quantity[0], 0.0, 0.001);
}

TEST(process_price_update_triggers_target) {
    TradingState state{};
    state.init(100000.0);

    state.positions.quantity[0] = 1.0;
    state.positions.avg_entry[0] = 100.0;
    state.flags.flags[0] = SymbolFlags::FLAG_HAS_POSITION;
    state.common.stop_pct[0] = 0.05;
    state.common.target_pct[0] = 0.10;

    // Price rises 12% - should trigger target
    process_price_update(0, 112.0, state);

    // Position should be exited
    ASSERT_NEAR(state.positions.quantity[0], 0.0, 0.001);
}

TEST(process_price_update_momentum_buy_signal) {
    TradingState state{};
    state.init(100000.0);

    // Set strategy to MOMENTUM
    state.strategies.active[0] = StrategyId::MOMENTUM;
    state.common.position_size_pct[0] = 0.05; // 5%

    // First tick: sets initial price (no momentum calculated)
    process_price_update(0, 100.0, state);
    ASSERT_NEAR(state.positions.current_price[0], 100.0, 0.01);
    ASSERT_NEAR(state.positions.quantity[0], 0.0, 0.001); // No position yet

    // Second tick: price up 0.1% - should generate momentum signal
    // momentum = (100.1 - 100) / 100 = 0.001
    // score = 0.001 / 0.00001 = 100 -> clamped to 1.0
    // 1.0 > 0.3 threshold, should BUY
    process_price_update(0, 100.1, state);

    // Should have a position now
    ASSERT_GT(state.positions.quantity[0], 0.0);
    ASSERT_NEAR(state.positions.avg_entry[0], 100.1, 0.01);
}

TEST(process_price_update_momentum_no_signal_on_first_tick) {
    TradingState state{};
    state.init(100000.0);

    state.strategies.active[0] = StrategyId::MOMENTUM;
    state.common.position_size_pct[0] = 0.05;

    // First tick: prev_price = 0, momentum = 0, no signal
    process_price_update(0, 100.0, state);

    // No position should be opened
    ASSERT_NEAR(state.positions.quantity[0], 0.0, 0.001);
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "\n=== Hot Path Tests ===\n\n";

    std::cout << "calculate_position_size Tests:\n";
    RUN_TEST(calculate_position_size_uses_config);
    RUN_TEST(calculate_position_size_respects_max_position);
    RUN_TEST(calculate_position_size_zero_on_no_cash);

    std::cout << "\ncheck_stop_target Tests:\n";
    RUN_TEST(check_stop_target_stop_hit);
    RUN_TEST(check_stop_target_target_hit);
    RUN_TEST(check_stop_target_no_exit_in_range);
    RUN_TEST(check_stop_target_no_position_no_exit);

    std::cout << "\ncheck_tuner_signal Tests:\n";
    RUN_TEST(check_tuner_signal_buy_signal);
    RUN_TEST(check_tuner_signal_sell_signal);
    RUN_TEST(check_tuner_signal_expired_signal_ignored);
    RUN_TEST(check_tuner_signal_no_signal);

    std::cout << "\ncheck_flags Tests:\n";
    RUN_TEST(check_flags_exit_requested);
    RUN_TEST(check_flags_trading_paused);
    RUN_TEST(check_flags_no_flags);

    std::cout << "\ngenerate_signal Tests:\n";
    RUN_TEST(generate_signal_buy_on_positive_score);
    RUN_TEST(generate_signal_no_buy_with_position);
    RUN_TEST(generate_signal_hold_on_weak_score);
    RUN_TEST(generate_signal_sell_on_negative_score_with_profit);
    RUN_TEST(generate_signal_hold_on_negative_score_no_profit);

    std::cout << "\nprocess_price_update Tests:\n";
    RUN_TEST(process_price_update_updates_current_price);
    RUN_TEST(process_price_update_skips_when_halted);
    RUN_TEST(process_price_update_handles_tuner_signal);
    RUN_TEST(process_price_update_triggers_stop);
    RUN_TEST(process_price_update_triggers_target);
    RUN_TEST(process_price_update_momentum_buy_signal);
    RUN_TEST(process_price_update_momentum_no_signal_on_first_tick);

    std::cout << "\n=== All Hot Path Tests Passed! ===\n";
    return 0;
}
