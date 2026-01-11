#pragma once

/**
 * SharedConfig - Bidirectional config exchange between HFT and Dashboard
 *
 * Dashboard can modify config values, HFT reads them on next check.
 * Lock-free using atomic operations.
 *
 * Usage:
 *   HFT (reader):
 *     auto cfg = SharedConfig::open("/hft_config");
 *     if (cfg->sequence() != last_seq) {
 *         apply_config(*cfg);
 *         last_seq = cfg->sequence();
 *     }
 *
 *   Dashboard (writer):
 *     auto cfg = SharedConfig::open_rw("/hft_config");
 *     cfg->set_spread_multiplier(20);  // 2.0x
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

namespace hft {
namespace ipc {

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
#ifdef HFT_BUILD_HASH
    static constexpr uint32_t VERSION = hex_to_u32(HFT_BUILD_HASH);
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
    std::atomic<int32_t> target_pct_x100;         // Default: 150 (1.5%)
    std::atomic<int32_t> stop_pct_x100;           // Default: 100 (1%)

    // Mode overrides
    std::atomic<uint8_t> force_mode;    // 0 = auto, 1-5 = force specific mode
    std::atomic<uint8_t> trading_enabled;  // 0 = paused, 1 = active

    // HFT writes these (dashboard reads)
    std::atomic<uint8_t> active_mode;   // Current active mode (HFT sets this)
    std::atomic<uint8_t> active_signals; // Number of active signals
    std::atomic<int32_t> consecutive_losses; // Current loss streak
    std::atomic<int32_t> consecutive_wins;   // Current win streak

    // HFT lifecycle (heartbeat)
    std::atomic<int64_t> heartbeat_ns;  // Last update timestamp (epoch ns)
    std::atomic<int32_t> hft_pid;       // HFT process ID
    std::atomic<uint8_t> hft_status;    // 0=stopped, 1=starting, 2=running, 3=shutting_down

    // Build info
    char build_hash[12];  // Git commit hash (8 chars + null + padding)

    uint8_t padding[1];

    // === Accessors ===
    double spread_multiplier() const { return spread_multiplier_x10.load() / 10.0; }
    double drawdown_threshold() const { return drawdown_threshold_x100.load() / 100.0; }
    int loss_streak() const { return loss_streak_threshold.load(); }
    double base_position_pct() const { return base_position_pct_x100.load() / 100.0; }
    double max_position_pct() const { return max_position_pct_x100.load() / 100.0; }
    double target_pct() const { return target_pct_x100.load() / 100.0; }
    double stop_pct() const { return stop_pct_x100.load() / 100.0; }

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
    void set_trading_enabled(bool enabled) {
        trading_enabled.store(enabled ? 1 : 0);
        sequence.fetch_add(1);
    }
    void set_force_mode(uint8_t mode) {
        force_mode.store(mode);
        sequence.fetch_add(1);
    }
    uint8_t get_force_mode() const { return force_mode.load(); }

    // HFT updates these (no sequence bump - read-only for dashboard)
    void set_active_mode(uint8_t mode) { active_mode.store(mode); }
    void set_active_signals(uint8_t count) { active_signals.store(count); }
    void set_consecutive_losses(int32_t count) { consecutive_losses.store(count); }
    void set_consecutive_wins(int32_t count) { consecutive_wins.store(count); }

    uint8_t get_active_mode() const { return active_mode.load(); }
    uint8_t get_active_signals() const { return active_signals.load(); }
    int32_t get_consecutive_losses() const { return consecutive_losses.load(); }
    int32_t get_consecutive_wins() const { return consecutive_wins.load(); }

    // HFT lifecycle
    void set_hft_status(uint8_t status) { hft_status.store(status); }
    void set_hft_pid(int32_t pid) { hft_pid.store(pid); }
    void update_heartbeat() {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        heartbeat_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    }

    uint8_t get_hft_status() const { return hft_status.load(); }
    int32_t get_hft_pid() const { return hft_pid.load(); }
    int64_t get_heartbeat_ns() const { return heartbeat_ns.load(); }

    // Check if HFT is alive (heartbeat within last N seconds)
    bool is_hft_alive(int timeout_seconds = 5) const {
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

        // Defaults
        spread_multiplier_x10.store(15);      // 1.5x
        drawdown_threshold_x100.store(200);   // 2%
        loss_streak_threshold.store(2);
        base_position_pct_x100.store(200);    // 2%
        max_position_pct_x100.store(500);     // 5%
        target_pct_x100.store(150);           // 1.5%
        stop_pct_x100.store(100);             // 1%
        force_mode.store(0);                  // auto
        trading_enabled.store(1);             // active

        // HFT status (defaults)
        active_mode.store(2);                 // NORMAL
        active_signals.store(0);
        consecutive_losses.store(0);
        consecutive_wins.store(0);

        // HFT lifecycle
        heartbeat_ns.store(0);
        hft_pid.store(0);
        hft_status.store(0);                  // stopped

        // Build info
#ifdef HFT_BUILD_HASH
        std::strncpy(build_hash, HFT_BUILD_HASH, 11);
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
