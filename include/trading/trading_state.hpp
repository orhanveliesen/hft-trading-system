#pragma once

/**
 * TradingState - SoA (Struct of Arrays) Layout for Hot Path Trading
 *
 * This header defines the master trading state structure using SoA layout
 * for cache-friendly, SIMD-ready data access on the hot path.
 *
 * Design Principles:
 * - All arrays are cache-line aligned (64 bytes) to prevent false sharing
 * - SoA layout enables efficient iteration over single fields (cache-friendly)
 * - Atomic operations for cross-process IPC via shared memory
 * - Fixed-point arithmetic (x8 = 1e8 scaling) for atomic int64 operations
 * - No allocations on hot path
 *
 * Memory Layout:
 * - MAX_SYMBOLS = 64 symbols (4KB per array of doubles)
 * - Each array starts on cache line boundary
 * - Total TradingState size is designed to fit in shared memory efficiently
 *
 * Usage:
 *   Writer (trader):
 *     auto* state = TradingStateSHM::create("/hft_trading_state");
 *     state->positions.quantity[sym] = 0.5;
 *     state->positions.current_price[sym] = 95000.0;
 *
 *   Reader (dashboard/tuner):
 *     auto* state = TradingStateSHM::open("/hft_trading_state");
 *     double qty = state->positions.quantity[sym];
 */

#include "../util/string_utils.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace hft {
namespace trading {

// =============================================================================
// Constants
// =============================================================================

// Maximum number of symbols we can track
// Note: Also defined in portfolio.hpp - both must stay in sync
#ifndef HFT_MAX_SYMBOLS_DEFINED
#define HFT_MAX_SYMBOLS_DEFINED
constexpr size_t MAX_SYMBOLS = 64;
#endif

// Fixed-point scaling factor for atomic int64 <-> double conversions
// Using 1e8 provides 8 decimal places of precision (sufficient for crypto prices)
constexpr double FIXED_POINT_SCALE = 1e8;

// =============================================================================
// PositionData (pure data, no config)
// =============================================================================

/**
 * Per-symbol position data in SoA layout.
 * Pure data - no configuration or thresholds here.
 */
struct alignas(64) PositionData {
    alignas(64) std::array<double, MAX_SYMBOLS> quantity;       // Current quantity held
    alignas(64) std::array<double, MAX_SYMBOLS> avg_entry;      // Average entry price
    alignas(64) std::array<double, MAX_SYMBOLS> current_price;  // Latest market price
    alignas(64) std::array<uint64_t, MAX_SYMBOLS> open_time_ns; // Position open timestamp

    PositionData() {
        quantity.fill(0.0);
        avg_entry.fill(0.0);
        current_price.fill(0.0);
        open_time_ns.fill(0);
    }

    void clear(size_t sym) {
        quantity[sym] = 0.0;
        avg_entry[sym] = 0.0;
        current_price[sym] = 0.0;
        open_time_ns[sym] = 0;
    }

    void clear_all() {
        quantity.fill(0.0);
        avg_entry.fill(0.0);
        current_price.fill(0.0);
        open_time_ns.fill(0);
    }
};

// =============================================================================
// CommonConfig (all strategies use)
// =============================================================================

/**
 * Common trading configuration per symbol.
 * These are the parameters that all strategies share.
 */
struct alignas(64) CommonConfig {
    // Default values (configurable per-symbol)
    static constexpr double DEFAULT_STOP_PCT = 0.02;             // 2% stop loss
    static constexpr double DEFAULT_TARGET_PCT = 0.03;           // 3% take profit
    static constexpr double DEFAULT_POSITION_SIZE_PCT = 0.05;    // 5% of portfolio
    static constexpr double DEFAULT_MIN_PROFIT_FOR_EXIT = 0.005; // 0.5% min profit

    alignas(64) std::array<double, MAX_SYMBOLS> stop_pct;            // Stop loss %
    alignas(64) std::array<double, MAX_SYMBOLS> target_pct;          // Take profit %
    alignas(64) std::array<double, MAX_SYMBOLS> position_size_pct;   // Position size %
    alignas(64) std::array<double, MAX_SYMBOLS> min_profit_for_exit; // Min profit to allow exit

    CommonConfig() {
        stop_pct.fill(0.0);
        target_pct.fill(0.0);
        position_size_pct.fill(0.0);
        min_profit_for_exit.fill(0.0);
    }

    void init_defaults() {
        stop_pct.fill(DEFAULT_STOP_PCT);
        target_pct.fill(DEFAULT_TARGET_PCT);
        position_size_pct.fill(DEFAULT_POSITION_SIZE_PCT);
        min_profit_for_exit.fill(DEFAULT_MIN_PROFIT_FOR_EXIT);
    }
};

// =============================================================================
// SymbolFlags (commands from tuner)
// =============================================================================

/**
 * Per-symbol flags for control signals from tuner/operator.
 * Flags are bitwise-OR'd together.
 */
struct alignas(64) SymbolFlags {
    static constexpr uint8_t FLAG_HAS_POSITION = 1 << 0;   // Symbol has open position
    static constexpr uint8_t FLAG_TRADING_PAUSED = 1 << 1; // Trading paused for this symbol
    static constexpr uint8_t FLAG_EXIT_REQUESTED = 1 << 2; // Exit position ASAP
    static constexpr uint8_t FLAG_NEWS_EVENT = 1 << 3;     // News event affecting symbol

    alignas(64) std::array<uint8_t, MAX_SYMBOLS> flags;

    SymbolFlags() { flags.fill(0); }

    void clear_all() { flags.fill(0); }
};

// =============================================================================
// TunerSignals (injected buy/sell)
// =============================================================================

/**
 * Tuner-injected trading signals.
 * Signals have a TTL (time-to-live) to prevent stale signals from executing.
 */
struct alignas(64) TunerSignals {
    static constexpr int8_t SIGNAL_SELL = -1;
    static constexpr int8_t SIGNAL_NONE = 0;
    static constexpr int8_t SIGNAL_BUY = 1;

    // Signal TTL: 5 seconds
    static constexpr uint64_t SIGNAL_TTL_NS = 5'000'000'000ULL;

    alignas(64) std::array<int8_t, MAX_SYMBOLS> signal;         // -1=sell, 0=none, +1=buy
    alignas(64) std::array<double, MAX_SYMBOLS> quantity;       // Quantity to trade
    alignas(64) std::array<uint64_t, MAX_SYMBOLS> timestamp_ns; // When signal was injected

    TunerSignals() {
        signal.fill(SIGNAL_NONE);
        quantity.fill(0.0);
        timestamp_ns.fill(0);
    }

    void inject_buy(size_t sym, double qty, uint64_t ts) {
        signal[sym] = SIGNAL_BUY;
        quantity[sym] = qty;
        timestamp_ns[sym] = ts;
    }

    void inject_sell(size_t sym, double qty, uint64_t ts) {
        signal[sym] = SIGNAL_SELL;
        quantity[sym] = qty;
        timestamp_ns[sym] = ts;
    }

    void clear_signal(size_t sym) {
        signal[sym] = SIGNAL_NONE;
        quantity[sym] = 0.0;
        timestamp_ns[sym] = 0;
    }

    bool is_signal_valid(size_t sym, uint64_t current_time_ns) const {
        if (signal[sym] == SIGNAL_NONE)
            return false;
        if (timestamp_ns[sym] == 0)
            return false;
        return (current_time_ns - timestamp_ns[sym]) < SIGNAL_TTL_NS;
    }

    void clear_all() {
        signal.fill(SIGNAL_NONE);
        quantity.fill(0.0);
        timestamp_ns.fill(0);
    }
};

// =============================================================================
// Strategy-specific Configs (component model)
// =============================================================================

/**
 * RSI Strategy configuration per symbol.
 */
struct alignas(64) RSIConfig {
    static constexpr double DEFAULT_OVERSOLD = 30.0;
    static constexpr double DEFAULT_OVERBOUGHT = 70.0;

    alignas(64) std::array<double, MAX_SYMBOLS> oversold;   // Buy when RSI < oversold
    alignas(64) std::array<double, MAX_SYMBOLS> overbought; // Sell when RSI > overbought

    RSIConfig() {
        oversold.fill(DEFAULT_OVERSOLD);
        overbought.fill(DEFAULT_OVERBOUGHT);
    }
};

/**
 * MACD Strategy configuration per symbol.
 */
struct alignas(64) MACDConfig {
    static constexpr double DEFAULT_FAST_PERIOD = 12.0;
    static constexpr double DEFAULT_SLOW_PERIOD = 26.0;
    static constexpr double DEFAULT_SIGNAL_PERIOD = 9.0;

    alignas(64) std::array<double, MAX_SYMBOLS> fast_period;
    alignas(64) std::array<double, MAX_SYMBOLS> slow_period;
    alignas(64) std::array<double, MAX_SYMBOLS> signal_period;

    MACDConfig() {
        fast_period.fill(DEFAULT_FAST_PERIOD);
        slow_period.fill(DEFAULT_SLOW_PERIOD);
        signal_period.fill(DEFAULT_SIGNAL_PERIOD);
    }
};

/**
 * Momentum Strategy configuration per symbol.
 */
struct alignas(64) MomentumConfig {
    static constexpr double DEFAULT_LOOKBACK = 14.0;
    // Threshold for tick-to-tick momentum scoring
    // 0.00001 (0.001%) means a 0.0003% move gives score of 0.3 (buy threshold)
    // Crypto tick data at ~50ms intervals typically shows 0.0001-0.001% moves
    static constexpr double DEFAULT_THRESHOLD = 0.00001;

    alignas(64) std::array<double, MAX_SYMBOLS> lookback;
    alignas(64) std::array<double, MAX_SYMBOLS> threshold;

    MomentumConfig() {
        lookback.fill(DEFAULT_LOOKBACK);
        threshold.fill(DEFAULT_THRESHOLD);
    }
};

// =============================================================================
// Active Strategy Selection
// =============================================================================

/**
 * Strategy identifiers for dispatch (no vtable).
 */
enum class StrategyId : uint8_t {
    NONE = 0,
    RSI = 1,
    MACD = 2,
    MOMENTUM = 3,
    DEFENSIVE = 4,
    TEST = 5 // Always returns positive score for testing
};

/**
 * Per-symbol strategy selection.
 */
struct alignas(64) StrategySelection {
    alignas(64) std::array<StrategyId, MAX_SYMBOLS> active;

    StrategySelection() { active.fill(StrategyId::NONE); }
};

// =============================================================================
// Risk Limits (SoA)
// =============================================================================

/**
 * Per-symbol risk limits in SoA format.
 */
struct alignas(64) RiskLimits {
    alignas(64) std::array<int64_t, MAX_SYMBOLS> max_position;     // Max position qty
    alignas(64) std::array<int64_t, MAX_SYMBOLS> max_notional;     // Max notional value
    alignas(64) std::array<int64_t, MAX_SYMBOLS> current_notional; // Current notional

    RiskLimits() {
        max_position.fill(0); // 0 = no limit
        max_notional.fill(0); // 0 = no limit
        current_notional.fill(0);
    }
};

// =============================================================================
// Global Risk State
// =============================================================================

/**
 * Global risk state with atomic operations for cross-process updates.
 */
struct GlobalRiskState {
    std::atomic<int64_t> daily_pnl_x8;        // Daily P&L (fixed point)
    std::atomic<int64_t> peak_equity_x8;      // Peak equity for drawdown calc
    std::atomic<int64_t> total_notional_x8;   // Total exposure
    std::atomic<int64_t> daily_loss_limit_x8; // Daily loss limit
    std::atomic<double> max_drawdown_pct;     // Max drawdown percentage
    std::atomic<uint8_t> risk_halted;         // Risk limit breached

    GlobalRiskState()
        : daily_pnl_x8(0), peak_equity_x8(0), total_notional_x8(0), daily_loss_limit_x8(0), max_drawdown_pct(0.0),
          risk_halted(0) {}
};

// =============================================================================
// Halt State
// =============================================================================

/**
 * Halt status values.
 */
enum class HaltStatus : uint8_t {
    RUNNING = 0, // Normal trading
    HALTING = 1, // Flatten in progress
    HALTED = 2   // Safe state, all positions closed
};

/**
 * Halt reason values.
 */
enum class HaltReason : uint8_t {
    NONE = 0,
    RISK_LIMIT = 1,      // Daily loss or drawdown
    MANUAL = 2,          // Operator kill switch
    SYSTEM_ERROR = 3,    // Unexpected error
    CONNECTION_LOST = 4, // Exchange connection lost
    POOL_EXHAUSTED = 5   // Order pool ran out
};

/**
 * Unified halt state.
 */
struct HaltState {
    std::atomic<uint8_t> halted;        // HaltStatus enum
    std::atomic<uint8_t> reason;        // HaltReason enum
    std::atomic<uint64_t> halt_time_ns; // When halt was triggered

    HaltState()
        : halted(static_cast<uint8_t>(HaltStatus::RUNNING)), reason(static_cast<uint8_t>(HaltReason::NONE)),
          halt_time_ns(0) {}
};

// =============================================================================
// Pending Orders (SoA for execution tracking)
// =============================================================================

/**
 * Pending orders in SoA format for execution tracking.
 */
struct alignas(64) PendingOrders {
    static constexpr size_t MAX_PENDING = 64;

    alignas(64) std::array<uint64_t, MAX_PENDING> order_id;
    alignas(64) std::array<uint8_t, MAX_PENDING> symbol_id;
    alignas(64) std::array<uint8_t, MAX_PENDING> side; // 0=buy, 1=sell
    alignas(64) std::array<double, MAX_PENDING> quantity;
    alignas(64) std::array<int64_t, MAX_PENDING> limit_price_x8;
    alignas(64) std::array<uint64_t, MAX_PENDING> submit_time_ns;
    alignas(64) std::array<uint8_t, MAX_PENDING> active;

    std::atomic<uint32_t> count; // Number of active orders

    PendingOrders() : count(0) {
        order_id.fill(0);
        symbol_id.fill(0);
        side.fill(0);
        quantity.fill(0.0);
        limit_price_x8.fill(0);
        submit_time_ns.fill(0);
        active.fill(0);
    }
};

// =============================================================================
// Master TradingState (in shared memory)
// =============================================================================

/**
 * Master TradingState struct combining all SoA components.
 * This entire struct is designed to live in shared memory.
 */
struct alignas(64) TradingState {
    // Magic number for validation
    static constexpr uint64_t MAGIC = 0x4846545453544154ULL; // "HFTTSTAT"

    // Version from build hash
#ifdef HFT_BUILD_HASH
    static constexpr uint32_t VERSION = util::hex_to_u32(HFT_BUILD_HASH);
#else
    static constexpr uint32_t VERSION = 0;
#endif

    // === Header ===
    uint64_t magic;
    uint32_t version;
    uint32_t reserved;
    std::atomic<uint32_t> sequence; // Incremented on each update

    // === Core position data ===
    PositionData positions;

    // === Config (common + strategy-specific) ===
    CommonConfig common;
    RSIConfig rsi;
    MACDConfig macd;
    MomentumConfig momentum;

    // === Control ===
    SymbolFlags flags;
    TunerSignals signals;
    StrategySelection strategies;

    // === Risk management ===
    RiskLimits risk_limits;
    GlobalRiskState risk_state;

    // === Halt management ===
    HaltState halt;

    // === Execution tracking ===
    PendingOrders pending;

    // === Global state ===
    std::atomic<int64_t> cash_x8;
    std::atomic<int64_t> initial_cash_x8;
    std::atomic<int64_t> total_realized_pnl_x8;
    std::atomic<uint32_t> total_fills;
    std::atomic<uint32_t> total_targets;
    std::atomic<uint32_t> total_stops;
    std::atomic<uint64_t> start_time_ns;

    // === Initialization ===
    void init(double starting_cash) {
        magic = MAGIC;
        version = VERSION;
        reserved = 0;
        sequence.store(0);

        // Initialize positions
        positions.clear_all();

        // Initialize configs with defaults
        common.init_defaults();
        // rsi, macd, momentum already have defaults in constructors

        // Initialize control
        flags.clear_all();
        signals.clear_all();
        // strategies already initialized in constructor

        // Initialize risk
        // risk_limits already initialized in constructor
        risk_state.daily_pnl_x8.store(0);
        risk_state.peak_equity_x8.store(static_cast<int64_t>(starting_cash * FIXED_POINT_SCALE));
        risk_state.total_notional_x8.store(0);
        risk_state.daily_loss_limit_x8.store(0);
        risk_state.max_drawdown_pct.store(0.0);
        risk_state.risk_halted.store(0);

        // Initialize halt state
        halt.halted.store(static_cast<uint8_t>(HaltStatus::RUNNING));
        halt.reason.store(static_cast<uint8_t>(HaltReason::NONE));
        halt.halt_time_ns.store(0);

        // Initialize global state
        cash_x8.store(static_cast<int64_t>(starting_cash * FIXED_POINT_SCALE));
        initial_cash_x8.store(static_cast<int64_t>(starting_cash * FIXED_POINT_SCALE));
        total_realized_pnl_x8.store(0);
        total_fills.store(0);
        total_targets.store(0);
        total_stops.store(0);
        start_time_ns.store(0);
    }

    bool is_valid() const { return magic == MAGIC && version == VERSION; }

    // === Accessors ===
    double cash() const { return cash_x8.load(std::memory_order_relaxed) / FIXED_POINT_SCALE; }

    double initial_cash() const { return initial_cash_x8.load(std::memory_order_relaxed) / FIXED_POINT_SCALE; }

    double total_realized_pnl() const {
        return total_realized_pnl_x8.load(std::memory_order_relaxed) / FIXED_POINT_SCALE;
    }
};

// Static assertions for size and alignment
static_assert(alignof(PositionData) == 64, "PositionData must be cache-line aligned");
static_assert(alignof(CommonConfig) == 64, "CommonConfig must be cache-line aligned");
static_assert(alignof(SymbolFlags) == 64, "SymbolFlags must be cache-line aligned");
static_assert(alignof(TunerSignals) == 64, "TunerSignals must be cache-line aligned");
static_assert(alignof(RiskLimits) == 64, "RiskLimits must be cache-line aligned");
static_assert(alignof(TradingState) == 64, "TradingState must be cache-line aligned");

} // namespace trading
} // namespace hft
