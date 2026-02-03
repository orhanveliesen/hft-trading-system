#pragma once

/**
 * Per-Symbol Trading Configuration
 *
 * Each symbol can have its own tuning parameters.
 * AI tuner updates these based on performance + market conditions.
 *
 * Binary-compatible struct for fast IPC and AI responses.
 */

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include "../config/defaults.hpp"

namespace hft {
namespace ipc {

// Maximum symbols we support
constexpr size_t MAX_TUNED_SYMBOLS = 32;
constexpr size_t SYMBOL_NAME_LEN = 16;

/**
 * Per-symbol tuning configuration
 * Packed struct for binary serialization (AI responses)
 */
#pragma pack(push, 1)
struct SymbolTuningConfig {
    // Identity
    char symbol[SYMBOL_NAME_LEN];  // "BTCUSDT\0"

    // Trading control
    uint8_t enabled;               // 0=skip, 1=trade this symbol
    uint8_t regime_override;       // 0=auto, 1-5=force specific regime

    // EMA deviation thresholds (x100, e.g., 100 = 1%)
    int16_t ema_dev_trending_x100; // Max % above EMA in uptrend
    int16_t ema_dev_ranging_x100;  // Max % above EMA in ranging
    int16_t ema_dev_highvol_x100;  // Max % above EMA in high vol

    // Position sizing (x100, e.g., 200 = 2%)
    int16_t base_position_x100;    // Base position % of capital
    int16_t max_position_x100;     // Max position % of capital

    // Trade filtering
    int16_t cooldown_ms;           // Cooldown between trades
    int8_t signal_strength;        // 1=Medium, 2=Strong
    int8_t reserved1;              // Padding

    // Profit targets (x100, e.g., 150 = 1.5%)
    int16_t target_pct_x100;       // Profit target %
    int16_t stop_pct_x100;         // Stop loss %
    int16_t pullback_pct_x100;     // Trend exit pullback %

    // Slippage/commission (for this symbol specifically)
    int16_t slippage_bps_x100;     // Expected slippage
    int16_t commission_x10000;     // Commission rate

    // Order execution preferences (AI-controlled per symbol)
    uint8_t order_type_preference; // 0=Auto, 1=MarketOnly, 2=LimitOnly, 3=Adaptive

    // Use global default flags (bit flags)
    // Bit 0: position sizing (base_position, max_position)
    // Bit 1: target/stop (target_pct, stop_pct, pullback_pct)
    // Bit 2: trade filtering (cooldown_ms, signal_strength)
    // Bit 3: EMA thresholds (ema_dev_*)
    uint8_t use_global_flags;      // Default: 0x0F (use global for all)

    int16_t limit_offset_bps_x100; // Limit price offset (bps * 100), e.g., 200 = 2 bps
    int16_t limit_timeout_ms;      // Adaptive mode: limit→market timeout (ms)

    // Performance tracking (tuner updates these)
    int32_t total_trades;          // Total trades
    int32_t winning_trades;        // Winning trades
    int64_t total_pnl_x100;        // Total P&L in cents
    int64_t last_update_ns;        // Last tuner update timestamp

    // Reserved for future use (24 bytes remaining after order fields)
    uint8_t reserved[24];

    // === Helper methods ===

    void init(const char* sym) {
        using namespace config;

        std::memset(this, 0, sizeof(*this));
        std::strncpy(symbol, sym, SYMBOL_NAME_LEN - 1);
        symbol[SYMBOL_NAME_LEN - 1] = '\0';

        // Trading costs
        slippage_bps_x100 = costs::SLIPPAGE_BPS_X100;
        commission_x10000 = costs::COMMISSION_X10000;

        // Target/stop (derived from round-trip costs in defaults.hpp)
        target_pct_x100 = targets::TARGET_X100;
        stop_pct_x100 = targets::STOP_X100;
        pullback_pct_x100 = targets::PULLBACK_X100;

        // Position sizing
        base_position_x100 = position::BASE_X100;
        max_position_x100 = position::MAX_X100;

        // EMA deviation thresholds
        ema_dev_trending_x100 = ema::DEV_TRENDING_X100;
        ema_dev_ranging_x100 = ema::DEV_RANGING_X100;
        ema_dev_highvol_x100 = ema::DEV_HIGHVOL_X100;

        // Execution settings
        cooldown_ms = execution::COOLDOWN_MS;
        signal_strength = execution::SIGNAL_STRENGTH;

        // Order execution
        order_type_preference = execution::ORDER_TYPE_AUTO;
        limit_offset_bps_x100 = execution::LIMIT_OFFSET_BPS_X100;
        limit_timeout_ms = execution::LIMIT_TIMEOUT_MS;

        // Feature flags
        enabled = 1;
        regime_override = 0;           // auto
        use_global_flags = flags::USE_GLOBAL_ALL;
    }

    // Accessors (convert from fixed-point)
    double ema_dev_trending() const { return ema_dev_trending_x100 / 100.0 / 100.0; }  // As decimal
    double ema_dev_ranging() const { return ema_dev_ranging_x100 / 100.0 / 100.0; }
    double ema_dev_highvol() const { return ema_dev_highvol_x100 / 100.0 / 100.0; }
    double base_position_pct() const { return base_position_x100 / 100.0; }
    double max_position_pct() const { return max_position_x100 / 100.0; }
    double target_pct() const { return target_pct_x100 / 100.0; }
    double stop_pct() const { return stop_pct_x100 / 100.0; }
    double pullback_pct() const { return pullback_pct_x100 / 100.0; }
    double win_rate() const { return total_trades > 0 ? (100.0 * winning_trades / total_trades) : 0; }
    double avg_pnl() const { return total_trades > 0 ? (total_pnl_x100 / 100.0 / total_trades) : 0; }

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

    // Use global flags accessors
    bool use_global_position() const { return (use_global_flags & 0x01) != 0; }
    bool use_global_target() const { return (use_global_flags & 0x02) != 0; }
    bool use_global_filtering() const { return (use_global_flags & 0x04) != 0; }
    bool use_global_ema() const { return (use_global_flags & 0x08) != 0; }

    void set_use_global_position(bool v) {
        if (v) use_global_flags |= 0x01;
        else use_global_flags &= ~0x01;
    }
    void set_use_global_target(bool v) {
        if (v) use_global_flags |= 0x02;
        else use_global_flags &= ~0x02;
    }
    void set_use_global_filtering(bool v) {
        if (v) use_global_flags |= 0x04;
        else use_global_flags &= ~0x04;
    }
    void set_use_global_ema(bool v) {
        if (v) use_global_flags |= 0x08;
        else use_global_flags &= ~0x08;
    }
};
#pragma pack(pop)

static_assert(sizeof(SymbolTuningConfig) == 96, "SymbolTuningConfig must be 96 bytes for binary protocol");

/**
 * Tuner command from AI
 * Binary struct that Claude returns (base64/hex encoded)
 */
#pragma pack(push, 1)
struct TunerCommand {
    static constexpr uint32_t MAGIC = 0x54554E45;  // "TUNE" in little-endian
    static constexpr uint8_t VERSION = 1;

    enum class Action : uint8_t {
        NoChange = 0,
        UpdateSymbolConfig = 1,
        PauseSymbol = 2,
        ResumeSymbol = 3,
        PauseAllTrading = 4,
        ResumeAllTrading = 5,
        EmergencyExitSymbol = 6,  // Close position for symbol
        EmergencyExitAll = 7      // Close all positions
    };

    // Header
    uint32_t magic;
    uint8_t version;
    Action action;
    uint16_t reserved_header;

    // Target
    char symbol[SYMBOL_NAME_LEN];  // Target symbol or "*" for all

    // Config update (when action == UpdateSymbolConfig)
    SymbolTuningConfig config;

    // Confidence and reasoning
    uint8_t confidence;           // 0-100 confidence score
    uint8_t urgency;              // 0=low, 1=medium, 2=high
    uint16_t reserved_meta;
    char reason[64];              // Human-readable reason

    // Checksum
    uint32_t checksum;

    // === Helper methods ===

    bool is_valid() const {
        return magic == MAGIC && version == VERSION && verify_checksum();
    }

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

    bool verify_checksum() const {
        return checksum == calculate_checksum();
    }
};
#pragma pack(pop)

static_assert(sizeof(TunerCommand) == 192, "TunerCommand must be 192 bytes");

/**
 * Shared memory structure for per-symbol configs
 * HFT reads, Tuner writes
 */
struct SharedSymbolConfigs {
    static constexpr uint64_t MAGIC = 0x53594D43464700ULL;  // "SYMCFG\0"
    static constexpr uint32_t VERSION = 1;

    // Header
    uint64_t magic;
    uint32_t version;
    std::atomic<uint32_t> sequence;  // Incremented on each change

    // Symbol configs
    std::atomic<uint32_t> symbol_count;
    SymbolTuningConfig symbols[MAX_TUNED_SYMBOLS];

    // Global tuner state
    std::atomic<int64_t> last_tune_ns;     // Last AI tuning timestamp
    std::atomic<uint32_t> tune_count;      // Total tuning operations
    std::atomic<uint8_t> tuner_connected;  // Is tuner process alive?

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

    bool is_valid() const {
        return magic == MAGIC && version == VERSION;
    }

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
        if (!existing) return false;

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
            if (win) cfg->winning_trades++;
            cfg->total_pnl_x100 += static_cast<int64_t>(pnl * 100);
        }
    }

    // === Shared Memory Factory ===

    static SharedSymbolConfigs* create(const char* name) {
        int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
        if (fd < 0) return nullptr;

        if (ftruncate(fd, sizeof(SharedSymbolConfigs)) < 0) {
            close(fd);
            return nullptr;
        }

        void* ptr = mmap(nullptr, sizeof(SharedSymbolConfigs),
                         PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);

        if (ptr == MAP_FAILED) return nullptr;

        auto* cfg = static_cast<SharedSymbolConfigs*>(ptr);
        cfg->init();
        return cfg;
    }

    static SharedSymbolConfigs* open(const char* name) {
        int fd = shm_open(name, O_RDONLY, 0666);
        if (fd < 0) return nullptr;

        void* ptr = mmap(nullptr, sizeof(SharedSymbolConfigs),
                         PROT_READ, MAP_SHARED, fd, 0);
        close(fd);

        if (ptr == MAP_FAILED) return nullptr;

        auto* cfg = static_cast<SharedSymbolConfigs*>(ptr);
        if (!cfg->is_valid()) {
            munmap(ptr, sizeof(SharedSymbolConfigs));
            return nullptr;
        }
        return cfg;
    }

    static SharedSymbolConfigs* open_rw(const char* name) {
        int fd = shm_open(name, O_RDWR, 0666);
        if (fd < 0) return nullptr;

        void* ptr = mmap(nullptr, sizeof(SharedSymbolConfigs),
                         PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);

        if (ptr == MAP_FAILED) return nullptr;

        auto* cfg = static_cast<SharedSymbolConfigs*>(ptr);
        if (!cfg->is_valid()) {
            munmap(ptr, sizeof(SharedSymbolConfigs));
            return nullptr;
        }
        return cfg;
    }
};

}  // namespace ipc
}  // namespace hft
