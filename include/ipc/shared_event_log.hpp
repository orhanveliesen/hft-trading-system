#pragma once

/**
 * Shared Event Log
 *
 * Lock-free ring buffer for event tracking across all HFT processes.
 * Writers: trader, trader_tuner
 * Readers: trader_web_api, trader_events, trader_dashboard
 *
 * Design:
 * - Single ring buffer for all events (16K slots, ~4MB)
 * - Per-symbol quick stats for dashboard
 * - Atomic write position for lock-free multi-writer
 * - Readers poll for new events
 */

#include "tuner_event.hpp"

#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace hft {
namespace ipc {

// Event log constants
constexpr size_t EVENT_LOG_RING_SIZE = 16384; // Power of 2 for fast modulo
constexpr size_t EVENT_LOG_MAX_SYMBOLS = 32;
constexpr char EVENT_LOG_SHM_NAME[] = "/trader_event_log";

/**
 * Per-symbol statistics (quick lookup for dashboard)
 */
struct SymbolEventStats {
    char symbol[EVENT_SYMBOL_LEN];

    // Trade stats
    std::atomic<uint32_t> signal_count;
    std::atomic<uint32_t> order_count;
    std::atomic<uint32_t> fill_count;
    std::atomic<uint32_t> cancel_count;

    // Performance
    std::atomic<int64_t> total_pnl_x100;   // Total P&L in cents
    std::atomic<int64_t> session_pnl_x100; // Session P&L (reset daily)
    std::atomic<uint32_t> winning_trades;
    std::atomic<uint32_t> losing_trades;

    // Tuner stats
    std::atomic<uint32_t> config_changes;
    std::atomic<uint32_t> pause_count;
    std::atomic<uint32_t> emergency_exits;

    // Timing
    std::atomic<uint64_t> last_signal_ns;
    std::atomic<uint64_t> last_fill_ns;
    std::atomic<uint64_t> last_config_ns;

    // Current state
    std::atomic<int8_t> current_regime;
    std::atomic<int8_t> is_paused;
    std::atomic<int8_t> has_position;
    std::atomic<int8_t> reserved;

    // === Methods ===

    void init(const char* sym) {
        std::memset(this, 0, sizeof(*this));
        std::strncpy(symbol, sym, EVENT_SYMBOL_LEN - 1);
    }

    bool matches(const char* sym) const { return std::strcmp(symbol, sym) == 0; }

    bool is_empty() const { return symbol[0] == '\0'; }

    double win_rate() const {
        uint32_t total = winning_trades.load() + losing_trades.load();
        return total > 0 ? (100.0 * winning_trades.load() / total) : 0.0;
    }

    double total_pnl() const { return total_pnl_x100.load() / 100.0; }

    double session_pnl() const { return session_pnl_x100.load() / 100.0; }

    void reset_session() { session_pnl_x100.store(0); }
};

/**
 * Global tuner statistics
 */
struct TunerStats {
    std::atomic<uint32_t> total_decisions;  // Total AI calls
    std::atomic<uint32_t> config_changes;   // Config changes made
    std::atomic<uint32_t> pauses_triggered; // Symbols paused
    std::atomic<uint32_t> emergency_exits;  // Emergency exits
    std::atomic<uint32_t> skipped_calls;    // Rate-limited calls

    std::atomic<uint64_t> total_latency_ms; // Sum of API latencies
    std::atomic<uint64_t> total_tokens_in;  // Total input tokens
    std::atomic<uint64_t> total_tokens_out; // Total output tokens
    std::atomic<int64_t> total_cost_x100;   // Total cost in cents

    std::atomic<uint64_t> last_decision_ns; // Last AI call timestamp
    std::atomic<uint64_t> last_trigger_ns;  // Last trigger timestamp

    // Performance
    std::atomic<int64_t> pnl_before_tuning_x100; // P&L snapshot at last tune
    std::atomic<int64_t> pnl_improvement_x100;   // Improvement since tuning

    void init() { std::memset(this, 0, sizeof(*this)); }

    double avg_latency_ms() const {
        uint32_t decisions = total_decisions.load();
        return decisions > 0 ? (static_cast<double>(total_latency_ms.load()) / decisions) : 0.0;
    }

    double total_cost() const { return total_cost_x100.load() / 100.0; }
};

/**
 * Main shared event log structure
 */
struct SharedEventLog {
    static constexpr uint64_t MAGIC = 0x4556544C4F4700ULL; // "EVTLOG\0"
    static constexpr uint32_t VERSION = 1;

    // === Header (32 bytes) ===
    uint64_t magic;
    uint32_t version;
    uint32_t ring_size;                 // EVENT_LOG_RING_SIZE
    std::atomic<uint64_t> write_pos;    // Next write position (never wraps)
    std::atomic<uint64_t> total_events; // Total events ever written

    // === Ring Buffer ===
    TunerEvent events[EVENT_LOG_RING_SIZE];

    // === Per-Symbol Stats ===
    std::atomic<uint32_t> symbol_count;
    SymbolEventStats symbol_stats[EVENT_LOG_MAX_SYMBOLS];

    // === Tuner Stats ===
    TunerStats tuner_stats;

    // === Session Info ===
    std::atomic<uint64_t> session_start_ns;
    std::atomic<int64_t> session_pnl_x100;

    // === Methods ===

    void init() {
        magic = MAGIC;
        version = VERSION;
        ring_size = EVENT_LOG_RING_SIZE;
        write_pos.store(0);
        total_events.store(0);
        symbol_count.store(0);
        session_start_ns.store(std::chrono::steady_clock::now().time_since_epoch().count());
        session_pnl_x100.store(0);
        std::memset(events, 0, sizeof(events));
        std::memset(symbol_stats, 0, sizeof(symbol_stats));
        tuner_stats.init();
    }

    bool is_valid() const { return magic == MAGIC && version == VERSION; }

    /**
     * Log an event (lock-free, multiple writers safe)
     */
    void log(TunerEvent event) {
        // Assign sequence number atomically
        uint64_t pos = write_pos.fetch_add(1);
        event.sequence = static_cast<uint32_t>(pos);

        // Write to ring buffer (modulo for wrap-around)
        size_t idx = pos & (EVENT_LOG_RING_SIZE - 1);
        events[idx] = event;

        // Update stats
        total_events.fetch_add(1);
        update_stats(event);
    }

    /**
     * Get event by sequence number
     * Returns nullptr if event has been overwritten
     */
    const TunerEvent* get_event(uint64_t seq) const {
        uint64_t current = write_pos.load();

        // Check if event is still in buffer
        if (seq >= current || (current - seq) > EVENT_LOG_RING_SIZE) {
            return nullptr;
        }

        size_t idx = seq & (EVENT_LOG_RING_SIZE - 1);
        const TunerEvent* e = &events[idx];

        // Verify sequence matches (race protection)
        if (e->sequence != static_cast<uint32_t>(seq)) {
            return nullptr;
        }

        return e;
    }

    /**
     * Get events since a sequence number
     * Returns number of events copied
     */
    size_t get_events_since(uint64_t since_seq, TunerEvent* out, size_t max_count) const {
        uint64_t current = write_pos.load();
        if (since_seq >= current)
            return 0;

        // Clamp to available range
        uint64_t start = since_seq;
        if ((current - start) > EVENT_LOG_RING_SIZE) {
            start = current - EVENT_LOG_RING_SIZE;
        }

        size_t count = 0;
        for (uint64_t seq = start; seq < current && count < max_count; ++seq) {
            const TunerEvent* e = get_event(seq);
            if (e) {
                out[count++] = *e;
            }
        }

        return count;
    }

    /**
     * Get events for a specific symbol
     */
    size_t get_symbol_events(const char* sym, TunerEvent* out, size_t max_count) const {
        uint64_t current = write_pos.load();
        uint64_t start = current > EVENT_LOG_RING_SIZE ? current - EVENT_LOG_RING_SIZE : 0;

        size_t count = 0;
        for (uint64_t seq = start; seq < current && count < max_count; ++seq) {
            const TunerEvent* e = get_event(seq);
            if (e && std::strcmp(e->symbol, sym) == 0) {
                out[count++] = *e;
            }
        }

        return count;
    }

    /**
     * Get or create symbol stats
     */
    SymbolEventStats* get_or_create_symbol_stats(const char* sym) {
        uint32_t count = symbol_count.load();

        // Find existing
        for (uint32_t i = 0; i < count; ++i) {
            if (symbol_stats[i].matches(sym)) {
                return &symbol_stats[i];
            }
        }

        // Create new if room
        if (count < EVENT_LOG_MAX_SYMBOLS) {
            // CAS to claim a slot
            if (symbol_count.compare_exchange_strong(count, count + 1)) {
                symbol_stats[count].init(sym);
                return &symbol_stats[count];
            }
            // Retry on CAS failure
            return get_or_create_symbol_stats(sym);
        }

        return nullptr;
    }

    /**
     * Get symbol stats (read-only)
     */
    const SymbolEventStats* find_symbol_stats(const char* sym) const {
        uint32_t count = symbol_count.load();
        for (uint32_t i = 0; i < count; ++i) {
            if (symbol_stats[i].matches(sym)) {
                return &symbol_stats[i];
            }
        }
        return nullptr;
    }

    /**
     * Get latest write position (for polling)
     */
    uint64_t current_position() const { return write_pos.load(); }

    /**
     * Reset session stats (call at start of trading day)
     */
    void reset_session() {
        session_start_ns.store(std::chrono::steady_clock::now().time_since_epoch().count());
        session_pnl_x100.store(0);

        uint32_t count = symbol_count.load();
        for (uint32_t i = 0; i < count; ++i) {
            symbol_stats[i].reset_session();
        }
    }

    // === Shared Memory Factory ===

    static SharedEventLog* create(const char* name = EVENT_LOG_SHM_NAME) {
        int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
        if (fd < 0)
            return nullptr;

        if (ftruncate(fd, sizeof(SharedEventLog)) < 0) {
            close(fd);
            return nullptr;
        }

        void* ptr = mmap(nullptr, sizeof(SharedEventLog), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);

        if (ptr == MAP_FAILED)
            return nullptr;

        auto* log = static_cast<SharedEventLog*>(ptr);
        log->init();
        return log;
    }

    static SharedEventLog* open_readonly(const char* name = EVENT_LOG_SHM_NAME) {
        int fd = shm_open(name, O_RDONLY, 0666);
        if (fd < 0)
            return nullptr;

        void* ptr = mmap(nullptr, sizeof(SharedEventLog), PROT_READ, MAP_SHARED, fd, 0);
        close(fd);

        if (ptr == MAP_FAILED)
            return nullptr;

        auto* log = static_cast<SharedEventLog*>(ptr);
        if (!log->is_valid()) {
            munmap(ptr, sizeof(SharedEventLog));
            return nullptr;
        }
        return log;
    }

    static SharedEventLog* open_readwrite(const char* name = EVENT_LOG_SHM_NAME) {
        int fd = shm_open(name, O_RDWR, 0666);
        if (fd < 0)
            return nullptr;

        void* ptr = mmap(nullptr, sizeof(SharedEventLog), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);

        if (ptr == MAP_FAILED)
            return nullptr;

        auto* log = static_cast<SharedEventLog*>(ptr);
        if (!log->is_valid()) {
            munmap(ptr, sizeof(SharedEventLog));
            return nullptr;
        }
        return log;
    }

    static void destroy(const char* name = EVENT_LOG_SHM_NAME) { shm_unlink(name); }

private:
    /**
     * Update statistics based on event type
     */
    void update_stats(const TunerEvent& event) {
        // Skip global events for symbol stats
        if (event.symbol[0] == '*' || event.symbol[0] == '\0') {
            // Update tuner stats for AI decisions
            if (event.type == TunerEventType::AIDecision) {
                tuner_stats.total_decisions.fetch_add(1);
                tuner_stats.total_latency_ms.fetch_add(event.payload.ai.latency_ms);
                tuner_stats.total_tokens_in.fetch_add(event.payload.ai.tokens_input);
                tuner_stats.total_tokens_out.fetch_add(event.payload.ai.tokens_output);
                tuner_stats.last_decision_ns.store(event.timestamp_ns);
            }
            return;
        }

        auto* stats = get_or_create_symbol_stats(event.symbol);
        if (!stats)
            return;

        switch (event.type) {
        case TunerEventType::Signal:
            stats->signal_count.fetch_add(1);
            stats->last_signal_ns.store(event.timestamp_ns);
            break;

        case TunerEventType::Order:
            stats->order_count.fetch_add(1);
            break;

        case TunerEventType::Fill:
            stats->fill_count.fetch_add(1);
            stats->last_fill_ns.store(event.timestamp_ns);
            stats->total_pnl_x100.fetch_add(event.payload.trade.pnl_x100);
            stats->session_pnl_x100.fetch_add(event.payload.trade.pnl_x100);
            session_pnl_x100.fetch_add(event.payload.trade.pnl_x100);
            if (event.payload.trade.pnl_x100 >= 0) {
                stats->winning_trades.fetch_add(1);
            } else {
                stats->losing_trades.fetch_add(1);
            }
            break;

        case TunerEventType::Cancel:
            stats->cancel_count.fetch_add(1);
            break;

        case TunerEventType::ConfigChange:
            stats->config_changes.fetch_add(1);
            stats->last_config_ns.store(event.timestamp_ns);
            tuner_stats.config_changes.fetch_add(1);
            break;

        case TunerEventType::PauseSymbol:
            stats->pause_count.fetch_add(1);
            stats->is_paused.store(1);
            tuner_stats.pauses_triggered.fetch_add(1);
            break;

        case TunerEventType::ResumeSymbol:
            stats->is_paused.store(0);
            break;

        case TunerEventType::EmergencyExit:
            stats->emergency_exits.fetch_add(1);
            tuner_stats.emergency_exits.fetch_add(1);
            break;

        case TunerEventType::RegimeChange:
            stats->current_regime.store(event.payload.regime.new_regime);
            break;

        case TunerEventType::PositionOpen:
            stats->has_position.store(1);
            break;

        case TunerEventType::PositionClose:
            stats->has_position.store(0);
            break;

        default:
            break;
        }
    }
};

// Verify size is reasonable
static_assert(sizeof(SharedEventLog) < 8 * 1024 * 1024, "SharedEventLog too large");

} // namespace ipc
} // namespace hft
