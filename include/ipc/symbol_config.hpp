#pragma once

/**
 * Per-Symbol Trading Configuration
 *
 * Each symbol can have its own tuning parameters.
 * AI tuner updates these based on performance + market conditions.
 *
 * Binary-compatible struct for fast IPC and AI responses.
 */

#include "../config/defaults.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace hft {
namespace ipc {

// Maximum symbols we support
constexpr size_t MAX_TUNED_SYMBOLS = 32;
constexpr size_t SYMBOL_NAME_LEN = 16;

/**
 * Per-symbol tuning configuration
 *
 * ALL trading parameters are now per-symbol (no more use_global_flags).
 * Tuner can tune each symbol independently based on its own performance.
 *
 * Packed struct for binary serialization (AI responses)
 */
#pragma pack(push, 1)
struct SymbolTuningConfig {
    // =========================================================================
    // Identity (16 bytes)
    // =========================================================================
    char symbol[SYMBOL_NAME_LEN]; // "BTCUSDT\0"

    // =========================================================================
    // Trading Control (2 bytes)
    // =========================================================================
    uint8_t enabled;         // 0=skip, 1=trade this symbol
    uint8_t regime_override; // 0=auto, 1-5=force specific regime

    // =========================================================================
    // EMA Deviation Thresholds (6 bytes)
    // x100, e.g., 100 = 1%
    // =========================================================================
    int16_t ema_dev_trending_x100; // Max % above EMA in uptrend
    int16_t ema_dev_ranging_x100;  // Max % above EMA in ranging
    int16_t ema_dev_highvol_x100;  // Max % above EMA in high vol

    // =========================================================================
    // Position Sizing (6 bytes)
    // x100, e.g., 200 = 2%
    // =========================================================================
    int16_t base_position_x100; // Base position % of capital
    int16_t max_position_x100;  // Max position % of capital
    int16_t min_position_x100;  // Min position % of capital (NEW)

    // =========================================================================
    // Trade Filtering (4 bytes)
    // =========================================================================
    int16_t cooldown_ms;    // Cooldown between trades
    int8_t signal_strength; // 1=Medium, 2=Strong
    int8_t reserved1;       // Padding

    // =========================================================================
    // Profit Targets (6 bytes)
    // x100, e.g., 150 = 1.5%
    // =========================================================================
    int16_t target_pct_x100;   // Profit target %
    int16_t stop_pct_x100;     // Stop loss %
    int16_t pullback_pct_x100; // Trend exit pullback %

    // =========================================================================
    // Trading Costs (4 bytes)
    // =========================================================================
    int16_t slippage_bps_x100; // Expected slippage
    int16_t commission_x10000; // Commission rate

    // =========================================================================
    // Order Execution (6 bytes)
    // =========================================================================
    uint8_t order_type_preference; // 0=Auto, 1=MarketOnly, 2=LimitOnly, 3=Adaptive
    uint8_t reserved2;             // Padding (was use_global_flags - REMOVED)
    int16_t limit_offset_bps_x100; // Limit price offset (bps * 100)
    int16_t limit_timeout_ms;      // Adaptive mode: limit→market timeout

    // =========================================================================
    // Mode Thresholds - Streak Based (7 bytes)
    // Per-symbol mode transitions based on consecutive wins/losses
    // =========================================================================
    int8_t losses_to_cautious;  // Consecutive losses → CAUTIOUS (default: 2)
    int8_t reserved_mode1;      // Was: losses_to_tighten_signal (unused, use SharedConfig)
    int8_t losses_to_defensive; // Consecutive losses → DEFENSIVE (default: 4)
    int8_t losses_to_pause;     // Consecutive losses → PAUSE trading (default: 5)
    int8_t losses_to_exit_only; // Consecutive losses → EXIT_ONLY (default: 6)
    int8_t wins_to_aggressive;  // Consecutive wins → can be AGGRESSIVE (default: 3)
    int8_t reserved_mode2;      // Was: wins_max_aggressive (unused, use SharedConfig)

    // =========================================================================
    // Drawdown Thresholds (4 bytes)
    // x100, e.g., 300 = 3%
    // =========================================================================
    int16_t drawdown_defensive_x100; // Drawdown % → DEFENSIVE (default: 300 = 3%)
    int16_t drawdown_exit_x100;      // Drawdown % → EXIT_ONLY (default: 500 = 5%)

    // =========================================================================
    // Sharpe Ratio Thresholds (6 bytes)
    // x100, e.g., 100 = 1.0
    // =========================================================================
    int16_t sharpe_aggressive_x100; // Sharpe > this → AGGRESSIVE (default: 100 = 1.0)
    int16_t sharpe_cautious_x100;   // Sharpe < this → CAUTIOUS (default: 30 = 0.3)
    int16_t sharpe_defensive_x100;  // Sharpe < this → DEFENSIVE (default: 0 = 0.0)

    // =========================================================================
    // Win Rate Thresholds (2 bytes)
    // x100, e.g., 60 = 60%
    // =========================================================================
    int8_t win_rate_aggressive_x100; // Win rate > this → AGGRESSIVE (default: 60)
    int8_t win_rate_cautious_x100;   // Win rate < this → CAUTIOUS (default: 40)

    // =========================================================================
    // Signal Thresholds by Mode (4 bytes)
    // x100, e.g., 50 = 0.5
    // =========================================================================
    int8_t signal_aggressive_x100; // Signal threshold when AGGRESSIVE (default: 30)
    int8_t signal_normal_x100;     // Signal threshold when NORMAL (default: 50)
    int8_t signal_cautious_x100;   // Signal threshold when CAUTIOUS (default: 70)
    int8_t min_confidence_x100;    // Below this, no signal (default: 30)

    // =========================================================================
    // Risk/Reward (1 byte)
    // =========================================================================
    int8_t min_risk_reward_x100; // Min risk/reward ratio (default: 60 = 0.6)

    // =========================================================================
    // Current State - Trader Updates (3 bytes)
    // These are updated by trader, NOT tuner
    // =========================================================================
    int8_t consecutive_losses; // Current loss streak
    int8_t consecutive_wins;   // Current win streak
    int8_t current_mode;       // 0=AGGRESSIVE, 1=NORMAL, 2=CAUTIOUS, 3=DEFENSIVE, 4=EXIT_ONLY

    // =========================================================================
    // Performance Tracking (24 bytes)
    // Trader updates these, tuner reads them
    // =========================================================================
    int32_t total_trades;   // Total trades
    int32_t winning_trades; // Winning trades
    int64_t total_pnl_x100; // Total P&L in cents
    int64_t last_update_ns; // Last tuner update timestamp

    // =========================================================================
    // Accumulation Control (8 bytes) - Tuner controls these
    // How aggressively to add to existing positions
    // =========================================================================
    int8_t accum_floor_trending_x100;   // Floor when trending (default: 50)
    int8_t accum_floor_ranging_x100;    // Floor when ranging (default: 30)
    int8_t accum_floor_highvol_x100;    // Floor when high volatility (default: 20)
    int8_t accum_boost_per_win_x100;    // Boost per consecutive win (default: 10)
    int8_t accum_penalty_per_loss_x100; // Penalty per consecutive loss (default: 10)
    int8_t accum_signal_boost_x100;     // Boost for strong signals (default: 10)
    int8_t accum_max_x100;              // Maximum accumulation factor (default: 80)
    int8_t accum_reserved;              // Padding

    // =========================================================================
    // Reserved for Future Use (pad to 128 bytes)
    // =========================================================================
    uint8_t reserved[19];

    // =========================================================================
    // Helper Methods
    // =========================================================================

    void init(const char* sym) {
        using namespace config;

        std::memset(this, 0, sizeof(*this));
        std::strncpy(symbol, sym, SYMBOL_NAME_LEN - 1);
        symbol[SYMBOL_NAME_LEN - 1] = '\0';

        // Trading costs
        slippage_bps_x100 = costs::SLIPPAGE_BPS_X100;
        commission_x10000 = costs::COMMISSION_X10000;

        // Target/stop
        target_pct_x100 = targets::TARGET_X100;
        stop_pct_x100 = targets::STOP_X100;
        pullback_pct_x100 = targets::PULLBACK_X100;

        // Position sizing
        base_position_x100 = position::BASE_X100;
        max_position_x100 = position::MAX_X100;
        min_position_x100 = smart_strategy::MIN_POSITION_X100;

        // EMA deviation thresholds
        ema_dev_trending_x100 = ema::DEV_TRENDING_X100;
        ema_dev_ranging_x100 = ema::DEV_RANGING_X100;
        ema_dev_highvol_x100 = ema::DEV_HIGHVOL_X100;

        // Trade filtering
        cooldown_ms = execution::COOLDOWN_MS;
        signal_strength = execution::SIGNAL_STRENGTH;

        // Order execution
        order_type_preference = execution::ORDER_TYPE_AUTO;
        limit_offset_bps_x100 = execution::LIMIT_OFFSET_BPS_X100;
        limit_timeout_ms = execution::LIMIT_TIMEOUT_MS;

        // Mode thresholds (streak-based)
        losses_to_cautious = smart_strategy::LOSSES_TO_CAUTIOUS;
        reserved_mode1 = 0; // Was: losses_to_tighten_signal (use SharedConfig)
        losses_to_defensive = smart_strategy::LOSSES_TO_DEFENSIVE;
        losses_to_pause = smart_strategy::LOSSES_TO_PAUSE;
        losses_to_exit_only = smart_strategy::LOSSES_TO_EXIT_ONLY;
        wins_to_aggressive = smart_strategy::WINS_TO_AGGRESSIVE;
        reserved_mode2 = 0; // Was: wins_max_aggressive (use SharedConfig)

        // Drawdown thresholds
        drawdown_defensive_x100 = smart_strategy::DRAWDOWN_DEFENSIVE_X100;
        drawdown_exit_x100 = smart_strategy::DRAWDOWN_EXIT_X100;

        // Sharpe thresholds
        sharpe_aggressive_x100 = smart_strategy::SHARPE_AGGRESSIVE_X100;
        sharpe_cautious_x100 = smart_strategy::SHARPE_CAUTIOUS_X100;
        sharpe_defensive_x100 = smart_strategy::SHARPE_DEFENSIVE_X100;

        // Win rate thresholds
        win_rate_aggressive_x100 = smart_strategy::WIN_RATE_AGGRESSIVE_X100;
        win_rate_cautious_x100 = smart_strategy::WIN_RATE_CAUTIOUS_X100;

        // Signal thresholds
        signal_aggressive_x100 = smart_strategy::SIGNAL_AGGRESSIVE_X100;
        signal_normal_x100 = smart_strategy::SIGNAL_NORMAL_X100;
        signal_cautious_x100 = smart_strategy::SIGNAL_CAUTIOUS_X100;
        min_confidence_x100 = smart_strategy::MIN_CONFIDENCE_X100;

        // Risk/reward
        min_risk_reward_x100 = smart_strategy::MIN_RISK_REWARD_X100;

        // Accumulation control
        accum_floor_trending_x100 = smart_strategy::ACCUM_FLOOR_TRENDING_X100;
        accum_floor_ranging_x100 = smart_strategy::ACCUM_FLOOR_RANGING_X100;
        accum_floor_highvol_x100 = smart_strategy::ACCUM_FLOOR_HIGHVOL_X100;
        accum_boost_per_win_x100 = smart_strategy::ACCUM_BOOST_PER_WIN_X100;
        accum_penalty_per_loss_x100 = smart_strategy::ACCUM_PENALTY_PER_LOSS_X100;
        accum_signal_boost_x100 = smart_strategy::ACCUM_SIGNAL_BOOST_X100;
        accum_max_x100 = smart_strategy::ACCUM_MAX_X100;

        // State (zeroed by memset)
        // consecutive_losses = 0;
        // consecutive_wins = 0;
        // current_mode = 0;  // AGGRESSIVE

        // Feature flags
        enabled = 1;
        regime_override = 0; // auto
    }

    // =========================================================================
    // Accessors (Convert from Fixed-Point)
    // =========================================================================

    // EMA deviation (returns as decimal, e.g., 0.01 for 1%)
    double ema_dev_trending() const { return ema_dev_trending_x100 / 100.0 / 100.0; }
    double ema_dev_ranging() const { return ema_dev_ranging_x100 / 100.0 / 100.0; }
    double ema_dev_highvol() const { return ema_dev_highvol_x100 / 100.0 / 100.0; }

    // Position sizing (returns as percentage, e.g., 2.0 for 2%)
    double base_position_pct() const { return base_position_x100 / 100.0; }
    double max_position_pct() const { return max_position_x100 / 100.0; }
    double min_position_pct() const { return min_position_x100 / 100.0; }

    // Target/stop (returns as percentage)
    double target_pct() const { return target_pct_x100 / 100.0; }
    double stop_pct() const { return stop_pct_x100 / 100.0; }
    double pullback_pct() const { return pullback_pct_x100 / 100.0; }

    // Performance
    double win_rate() const { return total_trades > 0 ? (100.0 * winning_trades / total_trades) : 0; }
    double avg_pnl() const { return total_trades > 0 ? (total_pnl_x100 / 100.0 / total_trades) : 0; }

    // Cooldown setter with bounds checking (prevents int16_t overflow)
    // Clamps to [100, 32767] ms range
    static constexpr int16_t COOLDOWN_MIN_MS = 100;
    static constexpr int16_t COOLDOWN_MAX_MS = 32767;
    void set_cooldown_ms(int32_t value) {
        if (value <= 0) {
            cooldown_ms = COOLDOWN_MIN_MS;
        } else if (value > COOLDOWN_MAX_MS) {
            cooldown_ms = COOLDOWN_MAX_MS;
        } else {
            cooldown_ms = static_cast<int16_t>(value);
        }
    }

    // Drawdown thresholds (returns as decimal, e.g., 0.03 for 3%)
    double drawdown_to_defensive() const { return drawdown_defensive_x100 / 10000.0; }
    double drawdown_to_exit() const { return drawdown_exit_x100 / 10000.0; }

    // Sharpe thresholds
    double sharpe_aggressive() const { return sharpe_aggressive_x100 / 100.0; }
    double sharpe_cautious() const { return sharpe_cautious_x100 / 100.0; }
    double sharpe_defensive() const { return sharpe_defensive_x100 / 100.0; }

    // Win rate thresholds (returns 0-100 scale)
    double win_rate_aggressive_threshold() const { return static_cast<double>(win_rate_aggressive_x100); }
    double win_rate_cautious_threshold() const { return static_cast<double>(win_rate_cautious_x100); }

    // Signal thresholds (returns 0-1 scale)
    double signal_threshold_aggressive() const { return signal_aggressive_x100 / 100.0; }
    double signal_threshold_normal() const { return signal_normal_x100 / 100.0; }
    double signal_threshold_cautious() const { return signal_cautious_x100 / 100.0; }
    double min_confidence() const { return min_confidence_x100 / 100.0; }

    // Risk/reward
    double min_risk_reward() const { return min_risk_reward_x100 / 100.0; }

    // Accumulation control accessors (returns 0-1 scale)
    double accum_floor_trending() const { return accum_floor_trending_x100 / 100.0; }
    double accum_floor_ranging() const { return accum_floor_ranging_x100 / 100.0; }
    double accum_floor_highvol() const { return accum_floor_highvol_x100 / 100.0; }
    double accum_boost_per_win() const { return accum_boost_per_win_x100 / 100.0; }
    double accum_penalty_per_loss() const { return accum_penalty_per_loss_x100 / 100.0; }
    double accum_signal_boost() const { return accum_signal_boost_x100 / 100.0; }
    double accum_max() const { return accum_max_x100 / 100.0; }

    // Order execution accessors
    uint8_t get_order_type_preference() const { return order_type_preference; }
    double limit_offset_bps() const { return limit_offset_bps_x100 / 100.0; }
    int32_t get_limit_timeout_ms() const { return limit_timeout_ms; }

    // Order type preference helpers
    bool is_market_only() const { return order_type_preference == 1; }
    bool is_limit_only() const { return order_type_preference == 2; }
    bool is_adaptive() const { return order_type_preference == 3; }
    bool is_order_type_auto() const { return order_type_preference == 0; }

    bool is_enabled() const { return enabled != 0; }
    bool matches(const char* sym) const { return std::strcmp(symbol, sym) == 0; }

    // =========================================================================
    // Backward Compatibility Stubs (use_global_flags REMOVED)
    // Everything is now per-symbol, these always return false
    // =========================================================================
    bool use_global_ema() const { return false; }
    bool use_global_position() const { return false; }
    bool use_global_target() const { return false; }
    bool use_global_filtering() const { return false; }
    void set_use_global_ema(bool) {}
    void set_use_global_position(bool) {}
    void set_use_global_target(bool) {}
    void set_use_global_filtering(bool) {}

    // =========================================================================
    // State Management
    // =========================================================================

    /**
     * Record a trade result and update streak/stats
     * @param won True if trade was profitable
     * @param pnl_pct P&L as percentage (e.g., 1.5 for 1.5% profit)
     */
    void record_trade(bool won, double pnl_pct) {
        // Update streaks
        if (won) {
            consecutive_wins++;
            consecutive_losses = 0;
            winning_trades++;
        } else {
            consecutive_losses++;
            consecutive_wins = 0;
        }

        // Update stats
        total_trades++;
        total_pnl_x100 += static_cast<int64_t>(pnl_pct * 100);
    }

    /**
     * Reset state (used when restarting or clearing history)
     */
    void reset_state() {
        consecutive_losses = 0;
        consecutive_wins = 0;
        current_mode = 1; // NORMAL
    }
};
#pragma pack(pop)

static_assert(sizeof(SymbolTuningConfig) == 128, "SymbolTuningConfig must be 128 bytes for binary protocol");

/**
 * Tuner command from AI
 * Binary struct that Claude returns (base64/hex encoded)
 */
#pragma pack(push, 1)
struct TunerCommand {
    static constexpr uint32_t MAGIC = 0x54554E45; // "TUNE" in little-endian
    static constexpr uint8_t VERSION = 1;

    enum class Action : uint8_t {
        NoChange = 0,
        UpdateSymbolConfig = 1,
        PauseSymbol = 2,
        ResumeSymbol = 3,
        PauseAllTrading = 4,
        ResumeAllTrading = 5,
        EmergencyExitSymbol = 6, // Close position for symbol
        EmergencyExitAll = 7     // Close all positions
    };

    // Header
    uint32_t magic;
    uint8_t version;
    Action action;
    uint16_t reserved_header;

    // Target
    char symbol[SYMBOL_NAME_LEN]; // Target symbol or "*" for all

    // Config update (when action == UpdateSymbolConfig)
    SymbolTuningConfig config;

    // Confidence and reasoning
    uint8_t confidence; // 0-100 confidence score
    uint8_t urgency;    // 0=low, 1=medium, 2=high
    uint16_t reserved_meta;
    char reason[64]; // Human-readable reason

    // Checksum
    uint32_t checksum;

    // === Helper methods ===

    bool is_valid() const { return magic == MAGIC && version == VERSION && verify_checksum(); }

    void finalize() {
        magic = MAGIC;
        version = VERSION;
        checksum = calculate_checksum();
    }

    uint32_t calculate_checksum() const {
        // Simple XOR checksum over the struct (excluding checksum field)
        const uint8_t* data = reinterpret_cast<const uint8_t*>(this);
        size_t len = offsetof(TunerCommand, checksum);
        uint32_t sum = 0;
        for (size_t i = 0; i < len; i += 4) {
            uint32_t word = 0;
            std::memcpy(&word, data + i, std::min(size_t(4), len - i));
            sum ^= word;
        }
        return sum;
    }

    bool verify_checksum() const { return checksum == calculate_checksum(); }
};
#pragma pack(pop)

static_assert(sizeof(TunerCommand) == 224, "TunerCommand must be 224 bytes");

/**
 * Shared memory structure for per-symbol configs
 * HFT reads, Tuner writes
 */
struct SharedSymbolConfigs {
    static constexpr uint64_t MAGIC = 0x53594D43464700ULL; // "SYMCFG\0"
    static constexpr uint32_t VERSION = 1;

    // Header
    uint64_t magic;
    uint32_t version;
    std::atomic<uint32_t> sequence; // Incremented on each change

    // Symbol configs
    std::atomic<uint32_t> symbol_count;
    SymbolTuningConfig symbols[MAX_TUNED_SYMBOLS];

    // Global tuner state
    std::atomic<int64_t> last_tune_ns;    // Last AI tuning timestamp
    std::atomic<uint32_t> tune_count;     // Total tuning operations
    std::atomic<uint8_t> tuner_connected; // Is tuner process alive?

    // === Methods ===

    void init() {
        magic = MAGIC;
        version = VERSION;
        sequence.store(0);
        symbol_count.store(0);
        last_tune_ns.store(0);
        tune_count.store(0);
        tuner_connected.store(0);
        std::memset(symbols, 0, sizeof(symbols));
    }

    bool is_valid() const { return magic == MAGIC && version == VERSION; }

    // Find or create symbol config
    SymbolTuningConfig* get_or_create(const char* sym) {
        uint32_t count = symbol_count.load();

        // Find existing
        for (uint32_t i = 0; i < count; ++i) {
            if (symbols[i].matches(sym)) {
                return &symbols[i];
            }
        }

        // Create new if room
        if (count < MAX_TUNED_SYMBOLS) {
            symbols[count].init(sym);
            symbol_count.store(count + 1);
            sequence.fetch_add(1);
            return &symbols[count];
        }

        return nullptr;
    }

    // Find symbol config (read-only)
    const SymbolTuningConfig* find(const char* sym) const {
        uint32_t count = symbol_count.load();
        for (uint32_t i = 0; i < count; ++i) {
            if (symbols[i].matches(sym)) {
                return &symbols[i];
            }
        }
        return nullptr;
    }

    // Update symbol config (from tuner)
    bool update(const char* sym, const SymbolTuningConfig& cfg) {
        auto* existing = get_or_create(sym);
        if (!existing)
            return false;

        // Copy config but preserve performance stats
        int32_t trades = existing->total_trades;
        int32_t wins = existing->winning_trades;
        int64_t pnl = existing->total_pnl_x100;

        *existing = cfg;
        existing->total_trades = trades;
        existing->winning_trades = wins;
        existing->total_pnl_x100 = pnl;
        existing->last_update_ns = std::chrono::steady_clock::now().time_since_epoch().count();

        sequence.fetch_add(1);
        return true;
    }

    // Record trade result (called by HFT)
    void record_trade(const char* sym, bool win, double pnl) {
        auto* cfg = get_or_create(sym);
        if (cfg) {
            cfg->total_trades++;
            if (win)
                cfg->winning_trades++;
            cfg->total_pnl_x100 += static_cast<int64_t>(pnl * 100);
        }
    }

    // === Shared Memory Factory ===

    static SharedSymbolConfigs* create(const char* name) {
        int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
        if (fd < 0)
            return nullptr;

        if (ftruncate(fd, sizeof(SharedSymbolConfigs)) < 0) {
            close(fd);
            return nullptr;
        }

        void* ptr = mmap(nullptr, sizeof(SharedSymbolConfigs), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);

        if (ptr == MAP_FAILED)
            return nullptr;

        auto* cfg = static_cast<SharedSymbolConfigs*>(ptr);
        cfg->init();
        return cfg;
    }

    static SharedSymbolConfigs* open(const char* name) {
        int fd = shm_open(name, O_RDONLY, 0666);
        if (fd < 0)
            return nullptr;

        void* ptr = mmap(nullptr, sizeof(SharedSymbolConfigs), PROT_READ, MAP_SHARED, fd, 0);
        close(fd);

        if (ptr == MAP_FAILED)
            return nullptr;

        auto* cfg = static_cast<SharedSymbolConfigs*>(ptr);
        if (!cfg->is_valid()) {
            munmap(ptr, sizeof(SharedSymbolConfigs));
            return nullptr;
        }
        return cfg;
    }

    static SharedSymbolConfigs* open_rw(const char* name) {
        int fd = shm_open(name, O_RDWR, 0666);
        if (fd < 0)
            return nullptr;

        void* ptr = mmap(nullptr, sizeof(SharedSymbolConfigs), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);

        if (ptr == MAP_FAILED)
            return nullptr;

        auto* cfg = static_cast<SharedSymbolConfigs*>(ptr);
        if (!cfg->is_valid()) {
            munmap(ptr, sizeof(SharedSymbolConfigs));
            return nullptr;
        }
        return cfg;
    }
};

} // namespace ipc
} // namespace hft
