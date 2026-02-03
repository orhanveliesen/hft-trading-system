#pragma once

#include "../config/defaults.hpp"

/**
 * SharedConfig - Bidirectional config exchange between Trader and Dashboard
 *
 * Dashboard can modify config values, Trader reads them on next check.
 * Lock-free using atomic operations.
 *
 * Usage:
 *   Trader (reader):
 *     auto cfg = SharedConfig::open("/trader_config");
 *     if (cfg->sequence() != last_seq) {
 *         apply_config(*cfg);
 *         last_seq = cfg->sequence();
 *     }
 *
 *   Dashboard (writer):
 *     auto cfg = SharedConfig::open_rw("/trader_config");
 *     cfg->set_spread_multiplier(20);  // 2.0x
 */

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

namespace hft {
namespace ipc {

/**
 * Strategy types for regime mapping
 * Used by SharedConfig::get/set_strategy_for_regime()
 */
enum class StrategyType : uint8_t {
    NONE = 0,       // No trading
    MOMENTUM = 1,   // Momentum/trend following
    MEAN_REV = 2,   // Mean reversion
    MKT_MAKER = 3,  // Market making
    DEFENSIVE = 4,  // Defensive (reduced risk)
    CAUTIOUS = 5,   // Extra cautious (high vol)
    SMART = 6       // Smart/adaptive strategy
};

constexpr size_t STRATEGY_TYPE_COUNT = 7;

// Strategy name lookup table: {long_name, short_name}
// Index matches StrategyType enum value - no branching needed
constexpr std::array<std::pair<const char*, const char*>, STRATEGY_TYPE_COUNT> STRATEGY_NAMES = {{
    {"NONE", "OFF"},
    {"MOMENTUM", "MOM"},
    {"MEAN_REV", "MRV"},
    {"MKT_MAKER", "MMK"},
    {"DEFENSIVE", "DEF"},
    {"CAUTIOUS", "CAU"},
    {"SMART", "SMT"}
}};

inline const char* strategy_type_to_string(StrategyType type) {
    const auto idx = static_cast<size_t>(type);
    return idx < STRATEGY_TYPE_COUNT ? STRATEGY_NAMES[idx].first : "UNKNOWN";
}

inline const char* strategy_type_to_short(StrategyType type) {
    const auto idx = static_cast<size_t>(type);
    return idx < STRATEGY_TYPE_COUNT ? STRATEGY_NAMES[idx].second : "UNK";
}

// Convert 8-char hex string to uint32_t at compile time
constexpr uint32_t hex_to_u32(const char* s) {
    uint32_t result = 0;
    for (int i = 0; i < 8 && s[i]; ++i) {
        result <<= 4;
        char c = s[i];
        if (c >= '0' && c <= '9') result |= (c - '0');
        else if (c >= 'a' && c <= 'f') result |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') result |= (c - 'A' + 10);
    }
    return result;
}

struct SharedConfig {
    static constexpr uint64_t MAGIC = 0x4846544346494700ULL;  // "HFTCFG\0"
#ifdef TRADER_BUILD_HASH
    static constexpr uint32_t VERSION = hex_to_u32(TRADER_BUILD_HASH);
#else
    static constexpr uint32_t VERSION = 0;  // Fallback
#endif

    // Header
    uint64_t magic;
    uint32_t version;
    std::atomic<uint32_t> sequence;  // Incremented on each change

    // ReviewGate config
    std::atomic<int32_t> spread_multiplier_x10;  // Default: 15 (1.5x)
    std::atomic<int32_t> drawdown_threshold_x100; // Default: 200 (2%)
    std::atomic<int32_t> loss_streak_threshold;   // Default: 2

    // SmartStrategy config
    std::atomic<int32_t> base_position_pct_x100;  // Default: 200 (2%)
    std::atomic<int32_t> max_position_pct_x100;   // Default: 500 (5%)
    std::atomic<int32_t> target_pct_x100;         // Default: 300 (3%)
    std::atomic<int32_t> stop_pct_x100;           // Default: 500 (5%)
    std::atomic<int32_t> pullback_pct_x100;       // Default: 50 (0.5%) - trend exit threshold

    // Trading costs (for paper trading simulation)
    std::atomic<int32_t> commission_rate_x10000;  // Default: 10 (0.1% = 0.001)
    std::atomic<int32_t> slippage_bps_x100;       // Default: 0 (slippage in basis points)

    // Trade filtering (anti-overtrading)
    std::atomic<int32_t> min_trade_value_x100;    // Default: 10000 ($100 minimum trade)
    std::atomic<int32_t> cooldown_ms;             // Default: 2000 (2 second cooldown)
    std::atomic<int32_t> signal_strength;         // Default: 2 (1=Medium, 2=Strong)
    std::atomic<uint8_t> auto_tune_enabled;       // Default: 1 (auto-tune on)

    // EMA deviation thresholds (max % above EMA to allow buy)
    std::atomic<int32_t> ema_dev_trending_x1000;  // Default: 10 (1% = 0.01)
    std::atomic<int32_t> ema_dev_ranging_x1000;   // Default: 5 (0.5% = 0.005)
    std::atomic<int32_t> ema_dev_highvol_x1000;   // Default: 2 (0.2% = 0.002)

    // Spike detection thresholds (regime detector)
    // Based on statistical significance: spike = move > N standard deviations
    std::atomic<int32_t> spike_threshold_x100;    // Default: 300 (3.0σ - 99.7% significance)
    std::atomic<int32_t> spike_lookback;          // Default: 10 (bars for avg calculation)
    std::atomic<int32_t> spike_min_move_x10000;   // Default: 50 (0.5% minimum move filter)
    std::atomic<int32_t> spike_cooldown;          // Default: 5 (bars between detections)

    // Mode overrides
    std::atomic<uint8_t> force_mode;    // 0 = auto, 1-5 = force specific mode
    std::atomic<uint8_t> trading_enabled;  // 0 = paused, 1 = active
    std::atomic<uint8_t> paper_trading;    // 1 = paper trading mode (simulation)

    // Tuner integration
    std::atomic<uint8_t> tuner_mode;       // 0 = OFF (traditional strategies), 1 = ON (AI-controlled)
    std::atomic<uint8_t> manual_override;  // 0 = normal, 1 = manual control (tuner ignored)
    std::atomic<uint8_t> tuner_paused;     // 0 = active, 1 = paused (tuner skips scheduled runs)
    std::atomic<uint8_t> reserved_tuner;   // Padding for alignment

    // Manual tune trigger (dashboard can request immediate tuning)
    std::atomic<int64_t> manual_tune_request_ns;  // Non-zero = tune immediately, then clear

    // Order execution defaults (global, symbols can override)
    std::atomic<uint8_t> order_type_default;      // 0=Auto, 1=MarketOnly, 2=LimitOnly, 3=Adaptive
    std::atomic<int16_t> limit_offset_bps_x100;   // Default limit offset (bps * 100)
    std::atomic<int32_t> limit_timeout_ms;        // Default adaptive timeout (ms)

    // Trader writes these (dashboard reads)
    std::atomic<uint8_t> active_mode;   // Current active mode (Trader sets this)
    std::atomic<uint8_t> active_signals; // Number of active signals
    std::atomic<int32_t> consecutive_losses; // Current loss streak
    std::atomic<int32_t> consecutive_wins;   // Current win streak

    // Trader lifecycle (heartbeat)
    std::atomic<int64_t> heartbeat_ns;  // Last update timestamp (epoch ns)
    std::atomic<int32_t> trader_pid;       // Trader process ID
    std::atomic<uint8_t> trader_status;    // 0=stopped, 1=starting, 2=running, 3=shutting_down

    // Trader start time (for dashboard restart detection)
    std::atomic<int64_t> trader_start_time_ns;  // When Trader process started

    // WebSocket connection status (Trader writes, dashboard reads)
    std::atomic<uint8_t> ws_market_status;     // 0=disconnected, 1=degraded, 2=healthy
    std::atomic<uint8_t> ws_user_status;       // 0=disconnected, 1=degraded, 2=healthy
    std::atomic<uint8_t> ws_reserved1;         // Padding for alignment
    std::atomic<uint8_t> ws_reserved2;         // Padding for alignment
    std::atomic<uint32_t> ws_reconnect_count;  // Total reconnection attempts
    std::atomic<int64_t> ws_last_message_ns;   // Last received message timestamp

    // Build info
    char build_hash[12];  // Git commit hash (8 chars + null + padding)

    // Display settings (dashboard uses these)
    std::atomic<int32_t> price_decimals;   // Decimal places for prices (default 4)
    std::atomic<int32_t> money_decimals;   // Decimal places for money/P&L (default 2)
    std::atomic<int32_t> qty_decimals;     // Decimal places for quantities (default 4)

    // Regime → Strategy mapping (configurable)
    // Index: 0=Unknown, 1=TrendingUp, 2=TrendingDown, 3=Ranging, 4=HighVol, 5=LowVol, 6=Spike
    // Value: StrategyType enum (0=NONE, 1=MOMENTUM, 2=MEAN_REV, 3=MKT_MAKER, 4=DEFENSIVE, 5=CAUTIOUS, 6=SMART)
    std::atomic<uint8_t> regime_strategy[8];  // 7 regimes + 1 padding

    // Position sizing mode
    // 0 = Percentage-based (use portfolio_value * position_pct, recommended)
    // 1 = Unit-based (use max_units directly)
    std::atomic<uint8_t> position_sizing_mode;
    std::atomic<int32_t> max_position_units;  // Max units when unit-based mode (default: 10)

    // === Accessors ===
    double spread_multiplier() const { return spread_multiplier_x10.load() / 10.0; }
    double drawdown_threshold() const { return drawdown_threshold_x100.load() / 100.0; }
    int loss_streak() const { return loss_streak_threshold.load(); }
    double base_position_pct() const { return base_position_pct_x100.load() / 100.0; }
    double max_position_pct() const { return max_position_pct_x100.load() / 100.0; }
    double target_pct() const { return target_pct_x100.load() / 100.0; }
    double stop_pct() const { return stop_pct_x100.load() / 100.0; }
    double pullback_pct() const { return pullback_pct_x100.load() / 100.0; }
    double commission_rate() const { return commission_rate_x10000.load() / 10000.0; }
    double slippage_bps() const { return slippage_bps_x100.load() / 100.0; }
    double min_trade_value() const { return min_trade_value_x100.load() / 100.0; }
    int32_t get_cooldown_ms() const { return cooldown_ms.load(); }
    int32_t get_signal_strength() const { return signal_strength.load(); }
    bool is_auto_tune_enabled() const { return auto_tune_enabled.load() != 0; }
    bool is_paper_trading() const { return paper_trading.load() != 0; }
    bool is_tuner_mode() const { return tuner_mode.load() != 0; }
    bool is_manual_override() const { return manual_override.load() != 0; }
    bool is_tuner_paused() const { return tuner_paused.load() != 0; }

    // Position sizing accessors
    bool is_percentage_based_sizing() const { return position_sizing_mode.load() == 0; }
    bool is_unit_based_sizing() const { return position_sizing_mode.load() == 1; }
    uint8_t get_position_sizing_mode() const { return position_sizing_mode.load(); }
    int32_t get_max_position_units() const { return max_position_units.load(); }

    // Check if manual tune was requested
    bool should_tune_now() const {
        return manual_tune_request_ns.load() > 0;
    }

    // Trigger manual tune (dashboard calls this)
    void request_manual_tune() {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        manual_tune_request_ns.store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    }

    // Clear manual tune request (tuner calls this after processing)
    void clear_manual_tune_request() {
        manual_tune_request_ns.store(0);
    }

    // Get manual tune request timestamp
    int64_t get_manual_tune_request_ns() const {
        return manual_tune_request_ns.load();
    }

    // Order execution accessors
    uint8_t get_order_type_default() const { return order_type_default.load(); }
    double get_limit_offset_bps() const { return limit_offset_bps_x100.load() / 100.0; }
    int32_t get_limit_timeout_ms() const { return limit_timeout_ms.load(); }
    bool is_order_type_market_only() const { return order_type_default.load() == 1; }
    bool is_order_type_limit_only() const { return order_type_default.load() == 2; }
    bool is_order_type_adaptive() const { return order_type_default.load() == 3; }

    // EMA deviation accessors (returns as decimal, e.g., 0.01 for 1%)
    double ema_dev_trending() const { return ema_dev_trending_x1000.load() / 1000.0; }
    double ema_dev_ranging() const { return ema_dev_ranging_x1000.load() / 1000.0; }
    double ema_dev_highvol() const { return ema_dev_highvol_x1000.load() / 1000.0; }

    // Spike detection accessors
    double spike_threshold() const { return spike_threshold_x100.load() / 100.0; }
    int32_t get_spike_lookback() const { return spike_lookback.load(); }
    double spike_min_move() const { return spike_min_move_x10000.load() / 10000.0; }
    int32_t get_spike_cooldown() const { return spike_cooldown.load(); }

    // Regime → Strategy mapping accessors
    // regime_idx: 0=Unknown, 1=TrendingUp, 2=TrendingDown, 3=Ranging, 4=HighVol, 5=LowVol, 6=Spike
    uint8_t get_strategy_for_regime(int regime_idx) const {
        if (regime_idx < 0 || regime_idx > 6) return 0;
        return regime_strategy[regime_idx].load();
    }

    void set_strategy_for_regime(int regime_idx, uint8_t strategy_type) {
        if (regime_idx < 0 || regime_idx > 6) return;
        regime_strategy[regime_idx].store(strategy_type);
        sequence.fetch_add(1);
    }

    // === Mutators (for dashboard) ===
    void set_spread_multiplier(double val) {
        spread_multiplier_x10.store(static_cast<int32_t>(val * 10));
        sequence.fetch_add(1);
    }
    void set_drawdown_threshold(double val) {
        drawdown_threshold_x100.store(static_cast<int32_t>(val * 100));
        sequence.fetch_add(1);
    }
    void set_loss_streak(int val) {
        loss_streak_threshold.store(val);
        sequence.fetch_add(1);
    }
    void set_base_position_pct(double val) {
        base_position_pct_x100.store(static_cast<int32_t>(val * 100));
        sequence.fetch_add(1);
    }
    void set_max_position_pct(double val) {
        max_position_pct_x100.store(static_cast<int32_t>(val * 100));
        sequence.fetch_add(1);
    }
    void set_target_pct(double val) {
        target_pct_x100.store(static_cast<int32_t>(val * 100));
        sequence.fetch_add(1);
    }
    void set_stop_pct(double val) {
        stop_pct_x100.store(static_cast<int32_t>(val * 100));
        sequence.fetch_add(1);
    }
    void set_pullback_pct(double val) {
        pullback_pct_x100.store(static_cast<int32_t>(val * 100));
        sequence.fetch_add(1);
    }
    void set_commission_rate(double val) {
        commission_rate_x10000.store(static_cast<int32_t>(val * 10000));
        sequence.fetch_add(1);
    }
    void set_slippage_bps(double val) {
        slippage_bps_x100.store(static_cast<int32_t>(val * 100));
        sequence.fetch_add(1);
    }
    void set_min_trade_value(double val) {
        min_trade_value_x100.store(static_cast<int32_t>(val * 100));
        sequence.fetch_add(1);
    }
    void set_cooldown_ms(int32_t val) {
        cooldown_ms.store(val);
        sequence.fetch_add(1);
    }
    void set_signal_strength(int32_t val) {
        signal_strength.store(val);
        sequence.fetch_add(1);
    }
    void set_auto_tune_enabled(bool enabled) {
        auto_tune_enabled.store(enabled ? 1 : 0);
        sequence.fetch_add(1);
    }
    // EMA deviation setters (val as percentage, e.g., 1.0 for 1%)
    void set_ema_dev_trending(double val) {
        ema_dev_trending_x1000.store(static_cast<int32_t>(val * 10));  // 1.0% -> 10
        sequence.fetch_add(1);
    }
    void set_ema_dev_ranging(double val) {
        ema_dev_ranging_x1000.store(static_cast<int32_t>(val * 10));   // 0.5% -> 5
        sequence.fetch_add(1);
    }
    void set_ema_dev_highvol(double val) {
        ema_dev_highvol_x1000.store(static_cast<int32_t>(val * 10));   // 0.2% -> 2
        sequence.fetch_add(1);
    }
    // Spike detection setters
    void set_spike_threshold(double val) {
        spike_threshold_x100.store(static_cast<int32_t>(val * 100));
        sequence.fetch_add(1);
    }
    void set_spike_lookback(int32_t val) {
        spike_lookback.store(val);
        sequence.fetch_add(1);
    }
    void set_spike_min_move(double val) {
        spike_min_move_x10000.store(static_cast<int32_t>(val * 10000));
        sequence.fetch_add(1);
    }
    void set_spike_cooldown(int32_t val) {
        spike_cooldown.store(val);
        sequence.fetch_add(1);
    }
    void set_trading_enabled(bool enabled) {
        trading_enabled.store(enabled ? 1 : 0);
        sequence.fetch_add(1);
    }
    void set_paper_trading(bool enabled) {
        paper_trading.store(enabled ? 1 : 0);
        sequence.fetch_add(1);
    }
    void set_force_mode(uint8_t mode) {
        force_mode.store(mode);
        sequence.fetch_add(1);
    }
    uint8_t get_force_mode() const { return force_mode.load(); }

    void set_tuner_mode(bool enabled) {
        tuner_mode.store(enabled ? 1 : 0);
        sequence.fetch_add(1);
    }
    void set_manual_override(bool enabled) {
        manual_override.store(enabled ? 1 : 0);
        sequence.fetch_add(1);
    }
    void set_tuner_paused(bool paused) {
        tuner_paused.store(paused ? 1 : 0);
        sequence.fetch_add(1);
    }

    // Position sizing mutators
    void set_position_sizing_mode(uint8_t mode) {
        position_sizing_mode.store(mode);
        sequence.fetch_add(1);
    }
    void set_max_position_units(int32_t units) {
        max_position_units.store(units);
        sequence.fetch_add(1);
    }

    // Order execution mutators
    void set_order_type_default(uint8_t type) {
        order_type_default.store(type);
        sequence.fetch_add(1);
    }
    void set_limit_offset_bps(double bps) {
        limit_offset_bps_x100.store(static_cast<int16_t>(bps * 100));
        sequence.fetch_add(1);
    }
    void set_limit_timeout_ms(int32_t ms) {
        limit_timeout_ms.store(ms);
        sequence.fetch_add(1);
    }

    // Trader updates these (no sequence bump - read-only for dashboard)
    void set_active_mode(uint8_t mode) { active_mode.store(mode); }
    void set_active_signals(uint8_t count) { active_signals.store(count); }
    void set_consecutive_losses(int32_t count) { consecutive_losses.store(count); }
    void set_consecutive_wins(int32_t count) { consecutive_wins.store(count); }

    uint8_t get_active_mode() const { return active_mode.load(); }
    uint8_t get_active_signals() const { return active_signals.load(); }
    int32_t get_consecutive_losses() const { return consecutive_losses.load(); }
    int32_t get_consecutive_wins() const { return consecutive_wins.load(); }

    // Display settings
    int get_price_decimals() const { return price_decimals.load(); }
    int get_money_decimals() const { return money_decimals.load(); }
    int get_qty_decimals() const { return qty_decimals.load(); }
    void set_price_decimals(int val) { price_decimals.store(val); sequence.fetch_add(1); }
    void set_money_decimals(int val) { money_decimals.store(val); sequence.fetch_add(1); }
    void set_qty_decimals(int val) { qty_decimals.store(val); sequence.fetch_add(1); }

    // Trader lifecycle
    void set_trader_status(uint8_t status) { trader_status.store(status); }
    void set_trader_pid(int32_t pid) { trader_pid.store(pid); }
    void update_heartbeat() {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        heartbeat_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    }

    uint8_t get_trader_status() const { return trader_status.load(); }
    int32_t get_trader_pid() const { return trader_pid.load(); }
    int64_t get_heartbeat_ns() const { return heartbeat_ns.load(); }
    int64_t get_trader_start_time_ns() const { return trader_start_time_ns.load(); }

    // WebSocket status accessors
    uint8_t get_ws_market_status() const { return ws_market_status.load(); }
    uint8_t get_ws_user_status() const { return ws_user_status.load(); }
    uint32_t get_ws_reconnect_count() const { return ws_reconnect_count.load(); }
    int64_t get_ws_last_message_ns() const { return ws_last_message_ns.load(); }

    // WebSocket status mutators (Trader calls these)
    void set_ws_market_status(uint8_t status) { ws_market_status.store(status); }
    void set_ws_user_status(uint8_t status) { ws_user_status.store(status); }
    void set_ws_reconnect_count(uint32_t count) { ws_reconnect_count.store(count); }
    void increment_ws_reconnect_count() { ws_reconnect_count.fetch_add(1); }
    void set_ws_last_message_ns(int64_t ns) { ws_last_message_ns.store(ns); }
    void update_ws_last_message() {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        ws_last_message_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    }

    // Check if WebSocket is healthy (receiving data within timeout)
    bool is_ws_healthy(int timeout_seconds = 10) const {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        int64_t last = ws_last_message_ns.load();
        if (last == 0) return false;  // Never received a message
        int64_t diff_ns = now_ns - last;
        return diff_ns < (timeout_seconds * 1'000'000'000LL);
    }

    // WebSocket status names for display
    static const char* ws_status_name(uint8_t status) {
        switch (status) {
            case 0: return "Disconnected";
            case 1: return "Degraded";
            case 2: return "Healthy";
            default: return "Unknown";
        }
    }

    // Trader start time setter
    void set_trader_start_time() {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        trader_start_time_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    }

    // Check if Trader is alive (heartbeat within last N seconds)
    bool is_trader_alive(int timeout_seconds = 5) const {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        int64_t last = heartbeat_ns.load();
        int64_t diff_ns = now_ns - last;
        return diff_ns < (timeout_seconds * 1'000'000'000LL);
    }

    // === Initialization ===
    void init() {
        magic = MAGIC;
        version = VERSION;
        sequence.store(0);

        // Risk management (from centralized defaults)
        spread_multiplier_x10.store(config::risk::SPREAD_MULTIPLIER_X10);
        drawdown_threshold_x100.store(config::risk::DRAWDOWN_THRESHOLD_X100);
        loss_streak_threshold.store(config::risk::LOSS_STREAK_THRESHOLD);

        // Position sizing
        base_position_pct_x100.store(config::position::BASE_X100);
        max_position_pct_x100.store(config::position::MAX_X100);
        min_trade_value_x100.store(config::position::MIN_TRADE_VALUE_X100);

        // Target/stop (derived from round-trip costs)
        target_pct_x100.store(config::targets::TARGET_X100);
        stop_pct_x100.store(config::targets::STOP_X100);
        pullback_pct_x100.store(config::targets::PULLBACK_X100);

        // Trading costs
        commission_rate_x10000.store(config::costs::COMMISSION_X10000);
        slippage_bps_x100.store(config::costs::SLIPPAGE_BPS_X100);

        // Order execution
        cooldown_ms.store(config::execution::COOLDOWN_MS);
        signal_strength.store(config::execution::SIGNAL_STRENGTH);
        auto_tune_enabled.store(config::flags::AUTO_TUNE_ENABLED ? 1 : 0);

        // EMA deviation thresholds
        ema_dev_trending_x1000.store(config::ema::DEV_TRENDING_X1000);
        ema_dev_ranging_x1000.store(config::ema::DEV_RANGING_X1000);
        ema_dev_highvol_x1000.store(config::ema::DEV_HIGHVOL_X1000);

        // Spike detection
        spike_threshold_x100.store(config::spike::THRESHOLD_X100);
        spike_lookback.store(config::spike::LOOKBACK_BARS);
        spike_min_move_x10000.store(config::spike::MIN_MOVE_X10000);
        spike_cooldown.store(config::spike::COOLDOWN_BARS);

        // Feature flags
        force_mode.store(0);                  // auto
        trading_enabled.store(config::flags::TRADING_ENABLED ? 1 : 0);
        paper_trading.store(config::flags::PAPER_TRADING ? 1 : 0);
        tuner_mode.store(0);                  // tuner OFF by default
        manual_override.store(0);             // normal mode
        tuner_paused.store(0);
        reserved_tuner.store(0);
        manual_tune_request_ns.store(0);

        // Order execution settings
        order_type_default.store(config::execution::ORDER_TYPE_AUTO);
        limit_offset_bps_x100.store(config::execution::LIMIT_OFFSET_BPS_X100);
        limit_timeout_ms.store(config::execution::LIMIT_TIMEOUT_MS);

        // Display defaults (not in centralized config - UI specific)
        price_decimals.store(4);
        money_decimals.store(2);
        qty_decimals.store(4);

        // Regime → Strategy mapping
        // StrategyType: 0=NONE, 1=MOMENTUM, 2=MEAN_REV, 3=MKT_MAKER, 4=DEFENSIVE, 5=CAUTIOUS, 6=SMART
        regime_strategy[0].store(0);  // Unknown → NONE
        regime_strategy[1].store(1);  // TrendingUp → MOMENTUM
        regime_strategy[2].store(4);  // TrendingDown → DEFENSIVE
        regime_strategy[3].store(3);  // Ranging → MKT_MAKER
        regime_strategy[4].store(5);  // HighVolatility → CAUTIOUS
        regime_strategy[5].store(3);  // LowVolatility → MKT_MAKER
        regime_strategy[6].store(0);  // Spike → NONE
        regime_strategy[7].store(0);  // padding

        // Position sizing mode
        position_sizing_mode.store(0);        // percentage-based
        max_position_units.store(config::position::MAX_UNITS);

        // Trader status
        active_mode.store(2);                 // NORMAL
        active_signals.store(0);
        consecutive_losses.store(0);
        consecutive_wins.store(0);

        // Trader lifecycle
        heartbeat_ns.store(0);
        trader_pid.store(0);
        trader_status.store(0);               // stopped
        trader_start_time_ns.store(0);           // not started yet

        // WebSocket connection status
        ws_market_status.store(0);            // disconnected
        ws_user_status.store(0);              // disconnected
        ws_reserved1.store(0);
        ws_reserved2.store(0);
        ws_reconnect_count.store(0);          // no reconnects yet
        ws_last_message_ns.store(0);          // no messages yet

        // Build info
#ifdef TRADER_BUILD_HASH
        std::strncpy(build_hash, TRADER_BUILD_HASH, 11);
        build_hash[11] = '\0';
#else
        std::strncpy(build_hash, "unknown", 11);
#endif
    }

    const char* get_build_hash() const { return build_hash; }

    bool is_valid() const {
        return magic == MAGIC && version == VERSION;
    }

    // === Shared Memory Factory ===
    static SharedConfig* create(const char* name) {
        int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
        if (fd < 0) return nullptr;

        if (ftruncate(fd, sizeof(SharedConfig)) < 0) {
            close(fd);
            return nullptr;
        }

        void* ptr = mmap(nullptr, sizeof(SharedConfig),
                         PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);

        if (ptr == MAP_FAILED) return nullptr;

        auto* cfg = static_cast<SharedConfig*>(ptr);
        cfg->init();
        return cfg;
    }

    static SharedConfig* open(const char* name) {
        int fd = shm_open(name, O_RDONLY, 0666);
        if (fd < 0) return nullptr;

        void* ptr = mmap(nullptr, sizeof(SharedConfig),
                         PROT_READ, MAP_SHARED, fd, 0);
        close(fd);

        if (ptr == MAP_FAILED) return nullptr;

        auto* cfg = static_cast<SharedConfig*>(ptr);
        if (!cfg->is_valid()) {
            munmap(ptr, sizeof(SharedConfig));
            return nullptr;
        }
        return cfg;
    }

    static SharedConfig* open_rw(const char* name) {
        int fd = shm_open(name, O_RDWR, 0666);
        if (fd < 0) return nullptr;

        void* ptr = mmap(nullptr, sizeof(SharedConfig),
                         PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);

        if (ptr == MAP_FAILED) return nullptr;

        auto* cfg = static_cast<SharedConfig*>(ptr);
        if (!cfg->is_valid()) {
            munmap(ptr, sizeof(SharedConfig));
            return nullptr;
        }
        return cfg;
    }

    static void destroy(const char* name) {
        shm_unlink(name);
    }
};

}  // namespace ipc
}  // namespace hft
