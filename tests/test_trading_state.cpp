#include "../include/trading/trading_state.hpp"
#include "../include/trading/trading_state_shm.hpp"

#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>

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

// =============================================================================
// Alignment Tests
// =============================================================================

TEST(position_data_cache_aligned) {
    ASSERT_EQ(alignof(PositionData), 64u);
}

TEST(common_config_cache_aligned) {
    ASSERT_EQ(alignof(CommonConfig), 64u);
}

TEST(symbol_flags_cache_aligned) {
    ASSERT_EQ(alignof(SymbolFlags), 64u);
}

TEST(tuner_signals_cache_aligned) {
    ASSERT_EQ(alignof(TunerSignals), 64u);
}

TEST(risk_limits_cache_aligned) {
    ASSERT_EQ(alignof(RiskLimits), 64u);
}

TEST(trading_state_cache_aligned) {
    ASSERT_EQ(alignof(TradingState), 64u);
}

// =============================================================================
// Size Tests - Ensure arrays have correct count
// =============================================================================

TEST(max_symbols_constant) {
    ASSERT_EQ(MAX_SYMBOLS, 64u);
}

TEST(position_data_array_sizes) {
    PositionData pd{};
    ASSERT_EQ(pd.quantity.size(), MAX_SYMBOLS);
    ASSERT_EQ(pd.avg_entry.size(), MAX_SYMBOLS);
    ASSERT_EQ(pd.current_price.size(), MAX_SYMBOLS);
    ASSERT_EQ(pd.open_time_ns.size(), MAX_SYMBOLS);
}

TEST(common_config_array_sizes) {
    CommonConfig cc{};
    ASSERT_EQ(cc.stop_pct.size(), MAX_SYMBOLS);
    ASSERT_EQ(cc.target_pct.size(), MAX_SYMBOLS);
    ASSERT_EQ(cc.position_size_pct.size(), MAX_SYMBOLS);
}

TEST(risk_limits_array_sizes) {
    RiskLimits rl{};
    ASSERT_EQ(rl.max_position.size(), MAX_SYMBOLS);
    ASSERT_EQ(rl.max_notional.size(), MAX_SYMBOLS);
    ASSERT_EQ(rl.current_notional.size(), MAX_SYMBOLS);
}

// =============================================================================
// PositionData Tests
// =============================================================================

TEST(position_data_initialization) {
    PositionData pd{};

    // All arrays should be zero-initialized
    for (size_t i = 0; i < MAX_SYMBOLS; ++i) {
        ASSERT_EQ(pd.quantity[i], 0.0);
        ASSERT_EQ(pd.avg_entry[i], 0.0);
        ASSERT_EQ(pd.current_price[i], 0.0);
        ASSERT_EQ(pd.open_time_ns[i], 0u);
    }
}

TEST(position_data_read_write) {
    PositionData pd{};

    constexpr size_t SYM_BTC = 0;
    constexpr size_t SYM_ETH = 1;

    pd.quantity[SYM_BTC] = 0.5;
    pd.avg_entry[SYM_BTC] = 95000.0;
    pd.current_price[SYM_BTC] = 96000.0;
    pd.open_time_ns[SYM_BTC] = 1234567890ULL;

    pd.quantity[SYM_ETH] = 2.0;
    pd.avg_entry[SYM_ETH] = 3200.0;
    pd.current_price[SYM_ETH] = 3250.0;

    ASSERT_NEAR(pd.quantity[SYM_BTC], 0.5, 1e-9);
    ASSERT_NEAR(pd.avg_entry[SYM_BTC], 95000.0, 1e-9);
    ASSERT_NEAR(pd.current_price[SYM_BTC], 96000.0, 1e-9);
    ASSERT_EQ(pd.open_time_ns[SYM_BTC], 1234567890ULL);

    ASSERT_NEAR(pd.quantity[SYM_ETH], 2.0, 1e-9);
}

// =============================================================================
// CommonConfig Tests
// =============================================================================

TEST(common_config_defaults) {
    CommonConfig cc{};
    cc.init_defaults();

    // Check default values
    for (size_t i = 0; i < MAX_SYMBOLS; ++i) {
        ASSERT_NEAR(cc.stop_pct[i], CommonConfig::DEFAULT_STOP_PCT, 1e-9);
        ASSERT_NEAR(cc.target_pct[i], CommonConfig::DEFAULT_TARGET_PCT, 1e-9);
        ASSERT_NEAR(cc.position_size_pct[i], CommonConfig::DEFAULT_POSITION_SIZE_PCT, 1e-9);
    }
}

TEST(common_config_per_symbol_override) {
    CommonConfig cc{};
    cc.init_defaults();

    constexpr size_t SYM = 5;
    cc.stop_pct[SYM] = 0.01;   // 1% stop
    cc.target_pct[SYM] = 0.05; // 5% target

    ASSERT_NEAR(cc.stop_pct[SYM], 0.01, 1e-9);
    ASSERT_NEAR(cc.target_pct[SYM], 0.05, 1e-9);

    // Other symbols unchanged
    ASSERT_NEAR(cc.stop_pct[0], CommonConfig::DEFAULT_STOP_PCT, 1e-9);
}

// =============================================================================
// SymbolFlags Tests
// =============================================================================

TEST(symbol_flags_initialization) {
    SymbolFlags sf{};

    for (size_t i = 0; i < MAX_SYMBOLS; ++i) {
        ASSERT_EQ(sf.flags[i], 0u);
    }
}

TEST(symbol_flags_set_and_check) {
    SymbolFlags sf{};

    constexpr size_t SYM = 3;

    // Set has_position flag
    sf.flags[SYM] |= SymbolFlags::FLAG_HAS_POSITION;
    ASSERT_TRUE(sf.flags[SYM] & SymbolFlags::FLAG_HAS_POSITION);
    ASSERT_FALSE(sf.flags[SYM] & SymbolFlags::FLAG_TRADING_PAUSED);

    // Set trading paused
    sf.flags[SYM] |= SymbolFlags::FLAG_TRADING_PAUSED;
    ASSERT_TRUE(sf.flags[SYM] & SymbolFlags::FLAG_HAS_POSITION);
    ASSERT_TRUE(sf.flags[SYM] & SymbolFlags::FLAG_TRADING_PAUSED);

    // Clear has_position
    sf.flags[SYM] &= ~SymbolFlags::FLAG_HAS_POSITION;
    ASSERT_FALSE(sf.flags[SYM] & SymbolFlags::FLAG_HAS_POSITION);
    ASSERT_TRUE(sf.flags[SYM] & SymbolFlags::FLAG_TRADING_PAUSED);
}

TEST(symbol_flags_exit_requested) {
    SymbolFlags sf{};

    constexpr size_t SYM = 10;
    sf.flags[SYM] |= SymbolFlags::FLAG_EXIT_REQUESTED;

    ASSERT_TRUE(sf.flags[SYM] & SymbolFlags::FLAG_EXIT_REQUESTED);

    // Clear after processing
    sf.flags[SYM] &= ~SymbolFlags::FLAG_EXIT_REQUESTED;
    ASSERT_FALSE(sf.flags[SYM] & SymbolFlags::FLAG_EXIT_REQUESTED);
}

// =============================================================================
// TunerSignals Tests
// =============================================================================

TEST(tuner_signals_initialization) {
    TunerSignals ts{};

    for (size_t i = 0; i < MAX_SYMBOLS; ++i) {
        ASSERT_EQ(ts.signal[i], 0);
        ASSERT_EQ(ts.quantity[i], 0.0);
        ASSERT_EQ(ts.timestamp_ns[i], 0u);
    }
}

TEST(tuner_signals_inject_buy) {
    TunerSignals ts{};

    constexpr size_t SYM = 0;
    ts.inject_buy(SYM, 0.1, 1000000000ULL);

    ASSERT_EQ(ts.signal[SYM], TunerSignals::SIGNAL_BUY);
    ASSERT_NEAR(ts.quantity[SYM], 0.1, 1e-9);
    ASSERT_EQ(ts.timestamp_ns[SYM], 1000000000ULL);
}

TEST(tuner_signals_inject_sell) {
    TunerSignals ts{};

    constexpr size_t SYM = 5;
    ts.inject_sell(SYM, 0.5, 2000000000ULL);

    ASSERT_EQ(ts.signal[SYM], TunerSignals::SIGNAL_SELL);
    ASSERT_NEAR(ts.quantity[SYM], 0.5, 1e-9);
    ASSERT_EQ(ts.timestamp_ns[SYM], 2000000000ULL);
}

TEST(tuner_signals_clear) {
    TunerSignals ts{};

    constexpr size_t SYM = 0;
    ts.inject_buy(SYM, 0.1, 1000000000ULL);

    ts.clear_signal(SYM);
    ASSERT_EQ(ts.signal[SYM], TunerSignals::SIGNAL_NONE);
}

TEST(tuner_signals_is_valid) {
    TunerSignals ts{};

    constexpr size_t SYM = 0;
    constexpr uint64_t NOW = 10'000'000'000ULL; // 10 seconds

    // Signal at 8 seconds, TTL is 5 seconds
    ts.inject_buy(SYM, 0.1, 8'000'000'000ULL);

    // At 10 seconds: signal is 2 seconds old - should be valid
    ASSERT_TRUE(ts.is_signal_valid(SYM, NOW));

    // At 20 seconds: signal is 12 seconds old - should be invalid
    constexpr uint64_t LATER = 20'000'000'000ULL;
    ASSERT_FALSE(ts.is_signal_valid(SYM, LATER));
}

// =============================================================================
// RiskLimits Tests
// =============================================================================

TEST(risk_limits_initialization) {
    RiskLimits rl{};

    for (size_t i = 0; i < MAX_SYMBOLS; ++i) {
        ASSERT_EQ(rl.max_position[i], 0);
        ASSERT_EQ(rl.max_notional[i], 0);
        ASSERT_EQ(rl.current_notional[i], 0);
    }
}

TEST(risk_limits_per_symbol) {
    RiskLimits rl{};

    constexpr size_t SYM = 0;
    rl.max_position[SYM] = 100;
    rl.max_notional[SYM] = 1000000; // 1M

    ASSERT_EQ(rl.max_position[SYM], 100);
    ASSERT_EQ(rl.max_notional[SYM], 1000000);
}

// =============================================================================
// GlobalRiskState Tests
// =============================================================================

TEST(global_risk_state_atomic_operations) {
    GlobalRiskState grs{};

    grs.daily_pnl_x8.store(0);
    grs.peak_equity_x8.store(100000 * FIXED_POINT_SCALE);
    grs.total_notional_x8.store(0);
    grs.risk_halted.store(0);

    // Simulate adding P&L
    grs.daily_pnl_x8.fetch_add(static_cast<int64_t>(1000.0 * FIXED_POINT_SCALE));
    ASSERT_EQ(grs.daily_pnl_x8.load(), static_cast<int64_t>(1000.0 * FIXED_POINT_SCALE));

    // Check halt flag
    grs.risk_halted.store(1);
    ASSERT_EQ(grs.risk_halted.load(), 1u);
}

// =============================================================================
// HaltState Tests
// =============================================================================

TEST(halt_state_transitions) {
    HaltState hs{};

    hs.halted.store(static_cast<uint8_t>(HaltStatus::RUNNING));
    hs.reason.store(static_cast<uint8_t>(HaltReason::NONE));

    ASSERT_EQ(hs.halted.load(), static_cast<uint8_t>(HaltStatus::RUNNING));

    // Transition to halting
    hs.halted.store(static_cast<uint8_t>(HaltStatus::HALTING));
    hs.reason.store(static_cast<uint8_t>(HaltReason::RISK_LIMIT));

    ASSERT_EQ(hs.halted.load(), static_cast<uint8_t>(HaltStatus::HALTING));
    ASSERT_EQ(hs.reason.load(), static_cast<uint8_t>(HaltReason::RISK_LIMIT));

    // Transition to halted
    hs.halted.store(static_cast<uint8_t>(HaltStatus::HALTED));
    ASSERT_EQ(hs.halted.load(), static_cast<uint8_t>(HaltStatus::HALTED));
}

// =============================================================================
// StrategySelection Tests
// =============================================================================

TEST(strategy_selection_per_symbol) {
    StrategySelection ss{};

    ss.active[0] = StrategyId::RSI;
    ss.active[1] = StrategyId::MACD;
    ss.active[2] = StrategyId::MOMENTUM;

    ASSERT_EQ(ss.active[0], StrategyId::RSI);
    ASSERT_EQ(ss.active[1], StrategyId::MACD);
    ASSERT_EQ(ss.active[2], StrategyId::MOMENTUM);
}

// =============================================================================
// TradingState Master Struct Tests
// =============================================================================

TEST(trading_state_initialization) {
    TradingState ts{};
    ts.init(100000.0); // 100k initial cash

    ASSERT_EQ(ts.magic, TradingState::MAGIC);
    ASSERT_EQ(ts.version, TradingState::VERSION);
    ASSERT_EQ(ts.cash_x8.load(), static_cast<int64_t>(100000.0 * FIXED_POINT_SCALE));
    ASSERT_EQ(ts.initial_cash_x8.load(), static_cast<int64_t>(100000.0 * FIXED_POINT_SCALE));
}

TEST(trading_state_is_valid) {
    TradingState ts{};
    ts.init(100000.0);

    ASSERT_TRUE(ts.is_valid());

    // Corrupt magic
    ts.magic = 0;
    ASSERT_FALSE(ts.is_valid());
}

TEST(trading_state_cash_operations) {
    TradingState ts{};
    ts.init(100000.0);

    // Simulate buying: cash decreases
    int64_t spent = static_cast<int64_t>(5000.0 * FIXED_POINT_SCALE);
    ts.cash_x8.fetch_sub(spent);

    double cash = ts.cash_x8.load() / FIXED_POINT_SCALE;
    ASSERT_NEAR(cash, 95000.0, 1e-6);
}

TEST(trading_state_position_update) {
    TradingState ts{};
    ts.init(100000.0);

    constexpr size_t SYM = 0;

    // Open position
    ts.positions.quantity[SYM] = 0.5;
    ts.positions.avg_entry[SYM] = 95000.0;
    ts.positions.current_price[SYM] = 95000.0;
    ts.positions.open_time_ns[SYM] = 1234567890;
    ts.flags.flags[SYM] |= SymbolFlags::FLAG_HAS_POSITION;

    ASSERT_TRUE(ts.flags.flags[SYM] & SymbolFlags::FLAG_HAS_POSITION);
    ASSERT_NEAR(ts.positions.quantity[SYM], 0.5, 1e-9);

    // Price update
    ts.positions.current_price[SYM] = 96000.0;

    // Calculate unrealized P&L
    double qty = ts.positions.quantity[SYM];
    double entry = ts.positions.avg_entry[SYM];
    double current = ts.positions.current_price[SYM];
    double unrealized_pnl = qty * (current - entry);

    ASSERT_NEAR(unrealized_pnl, 500.0, 1e-9);
}

TEST(trading_state_halt_integration) {
    TradingState ts{};
    ts.init(100000.0);

    // Initially running
    ASSERT_EQ(ts.halt.halted.load(), static_cast<uint8_t>(HaltStatus::RUNNING));

    // Trigger halt
    ts.halt.halted.store(static_cast<uint8_t>(HaltStatus::HALTING));
    ts.halt.reason.store(static_cast<uint8_t>(HaltReason::RISK_LIMIT));
    ts.halt.halt_time_ns.store(1234567890);

    ASSERT_EQ(ts.halt.halted.load(), static_cast<uint8_t>(HaltStatus::HALTING));
    ASSERT_EQ(ts.halt.reason.load(), static_cast<uint8_t>(HaltReason::RISK_LIMIT));
}

TEST(trading_state_sequence_increment) {
    TradingState ts{};
    ts.init(100000.0);

    uint32_t seq1 = ts.sequence.load();
    ts.sequence.fetch_add(1);
    uint32_t seq2 = ts.sequence.load();

    ASSERT_EQ(seq2, seq1 + 1);
}

// =============================================================================
// Shared Memory Tests
// =============================================================================

constexpr const char* TEST_SHM_NAME = "/hft_trading_state_test";

TEST(shm_create_and_init) {
    // Cleanup any existing
    TradingStateSHM::destroy(TEST_SHM_NAME);

    TradingState* state = TradingStateSHM::create(TEST_SHM_NAME, 100000.0);
    ASSERT_TRUE(state != nullptr);
    ASSERT_TRUE(state->is_valid());
    ASSERT_EQ(state->magic, TradingState::MAGIC);

    double cash = state->cash_x8.load() / FIXED_POINT_SCALE;
    ASSERT_NEAR(cash, 100000.0, 1e-6);

    TradingStateSHM::close(state);
    TradingStateSHM::destroy(TEST_SHM_NAME);
}

TEST(shm_open_existing) {
    TradingStateSHM::destroy(TEST_SHM_NAME);

    // Create
    TradingState* owner = TradingStateSHM::create(TEST_SHM_NAME, 50000.0);
    ASSERT_TRUE(owner != nullptr);

    // Modify
    owner->positions.quantity[0] = 1.5;
    owner->positions.current_price[0] = 90000.0;

    // Open from "another process"
    TradingState* client = TradingStateSHM::open(TEST_SHM_NAME);
    ASSERT_TRUE(client != nullptr);

    // Should see the same values
    ASSERT_NEAR(client->positions.quantity[0], 1.5, 1e-9);
    ASSERT_NEAR(client->positions.current_price[0], 90000.0, 1e-9);

    TradingStateSHM::close(client);
    TradingStateSHM::close(owner);
    TradingStateSHM::destroy(TEST_SHM_NAME);
}

TEST(shm_open_readonly) {
    TradingStateSHM::destroy(TEST_SHM_NAME);

    TradingState* owner = TradingStateSHM::create(TEST_SHM_NAME, 100000.0);
    owner->flags.flags[5] = SymbolFlags::FLAG_HAS_POSITION;

    const TradingState* reader = TradingStateSHM::open_readonly(TEST_SHM_NAME);
    ASSERT_TRUE(reader != nullptr);
    ASSERT_TRUE(reader->flags.flags[5] & SymbolFlags::FLAG_HAS_POSITION);

    // Reader can still read atomics
    owner->sequence.fetch_add(1);
    ASSERT_EQ(reader->sequence.load(), 1u);

    TradingStateSHM::close(reader);
    TradingStateSHM::close(owner);
    TradingStateSHM::destroy(TEST_SHM_NAME);
}

TEST(shm_cross_thread_visibility) {
    TradingStateSHM::destroy(TEST_SHM_NAME);

    TradingState* state = TradingStateSHM::create(TEST_SHM_NAME, 100000.0);

    std::atomic<bool> ready{false};
    std::atomic<bool> done{false};
    double observed_price = 0;

    // Reader thread
    std::thread reader([&]() {
        TradingState* s = TradingStateSHM::open(TEST_SHM_NAME);
        ready.store(true);

        while (!done.load()) {
            observed_price = s->positions.current_price[0];
            std::this_thread::yield();
        }

        TradingStateSHM::close(s);
    });

    // Wait for reader
    while (!ready.load()) {
        std::this_thread::yield();
    }

    // Write new price
    state->positions.current_price[0] = 95000.0;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    done.store(true);

    reader.join();

    ASSERT_NEAR(observed_price, 95000.0, 1e-9);

    TradingStateSHM::close(state);
    TradingStateSHM::destroy(TEST_SHM_NAME);
}

TEST(shm_open_nonexistent_fails) {
    TradingStateSHM::destroy("/nonexistent_shm_test");

    TradingState* state = TradingStateSHM::open("/nonexistent_shm_test");
    ASSERT_TRUE(state == nullptr);
}

TEST(scoped_trading_state_raii) {
    TradingStateSHM::destroy(TEST_SHM_NAME);

    {
        ScopedTradingState owner(true, TEST_SHM_NAME, 100000.0);
        ASSERT_TRUE(owner);
        owner->positions.quantity[0] = 0.5;

        ScopedTradingState client(false, TEST_SHM_NAME);
        ASSERT_TRUE(client);
        ASSERT_NEAR(client->positions.quantity[0], 0.5, 1e-9);
    }
    // owner destroyed, should have cleaned up

    // Try to open again - should fail
    TradingState* should_be_null = TradingStateSHM::open(TEST_SHM_NAME);
    ASSERT_TRUE(should_be_null == nullptr);
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "\n=== TradingState SoA Tests ===\n\n";

    std::cout << "Alignment Tests:\n";
    RUN_TEST(position_data_cache_aligned);
    RUN_TEST(common_config_cache_aligned);
    RUN_TEST(symbol_flags_cache_aligned);
    RUN_TEST(tuner_signals_cache_aligned);
    RUN_TEST(risk_limits_cache_aligned);
    RUN_TEST(trading_state_cache_aligned);

    std::cout << "\nSize Tests:\n";
    RUN_TEST(max_symbols_constant);
    RUN_TEST(position_data_array_sizes);
    RUN_TEST(common_config_array_sizes);
    RUN_TEST(risk_limits_array_sizes);

    std::cout << "\nPositionData Tests:\n";
    RUN_TEST(position_data_initialization);
    RUN_TEST(position_data_read_write);

    std::cout << "\nCommonConfig Tests:\n";
    RUN_TEST(common_config_defaults);
    RUN_TEST(common_config_per_symbol_override);

    std::cout << "\nSymbolFlags Tests:\n";
    RUN_TEST(symbol_flags_initialization);
    RUN_TEST(symbol_flags_set_and_check);
    RUN_TEST(symbol_flags_exit_requested);

    std::cout << "\nTunerSignals Tests:\n";
    RUN_TEST(tuner_signals_initialization);
    RUN_TEST(tuner_signals_inject_buy);
    RUN_TEST(tuner_signals_inject_sell);
    RUN_TEST(tuner_signals_clear);
    RUN_TEST(tuner_signals_is_valid);

    std::cout << "\nRiskLimits Tests:\n";
    RUN_TEST(risk_limits_initialization);
    RUN_TEST(risk_limits_per_symbol);

    std::cout << "\nGlobalRiskState Tests:\n";
    RUN_TEST(global_risk_state_atomic_operations);

    std::cout << "\nHaltState Tests:\n";
    RUN_TEST(halt_state_transitions);

    std::cout << "\nStrategySelection Tests:\n";
    RUN_TEST(strategy_selection_per_symbol);

    std::cout << "\nTradingState Master Struct Tests:\n";
    RUN_TEST(trading_state_initialization);
    RUN_TEST(trading_state_is_valid);
    RUN_TEST(trading_state_cash_operations);
    RUN_TEST(trading_state_position_update);
    RUN_TEST(trading_state_halt_integration);
    RUN_TEST(trading_state_sequence_increment);

    std::cout << "\nShared Memory Tests:\n";
    RUN_TEST(shm_create_and_init);
    RUN_TEST(shm_open_existing);
    RUN_TEST(shm_open_readonly);
    RUN_TEST(shm_cross_thread_visibility);
    RUN_TEST(shm_open_nonexistent_fails);
    RUN_TEST(scoped_trading_state_raii);

    std::cout << "\n=== All TradingState Tests Passed! ===\n";
    return 0;
}
