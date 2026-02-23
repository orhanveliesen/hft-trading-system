#pragma once

/**
 * Dedicated Shared Memory for Tuner Decisions
 *
 * Separate from TradeEvent (which has 128 byte limit).
 * This allows storing:
 * - Full AI reason text (256 bytes)
 * - Multiple parameter changes per decision
 * - Ring buffer history of recent decisions
 *
 * Usage:
 *   Writer (trader_tuner):
 *     auto* state = SharedTunerState::create(SharedTunerState::SHM_NAME);
 *     auto* decision = state->write_next();
 *     decision->set_symbol("BTCUSDT");
 *     decision->set_reason("Win rate too low...");
 *     decision->add_change(TunerParam::Cooldown, 2000, 5000);
 *     state->commit_write();
 *
 *   Reader (trader_dashboard):
 *     auto* state = SharedTunerState::open(SharedTunerState::SHM_NAME);
 *     const auto* latest = state->get_latest();
 *     printf("Reason: %s\n", latest->reason);
 */

#include "trade_event.hpp" // For TunerParam, TunerConcern enums

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace hft {
namespace ipc {

// ============================================================================
// Constants
// ============================================================================

static constexpr size_t MAX_REASON_LEN = 256;
static constexpr size_t MAX_TUNER_HISTORY = 16; // Ring buffer size
static constexpr size_t MAX_PARAM_CHANGES = 6;  // Max changes per decision
static constexpr size_t TUNER_SYMBOL_LEN = 16;

// ============================================================================
// ParamChange - Single parameter change
// ============================================================================

struct ParamChange {
    uint8_t param; // TunerParam enum value
    uint8_t padding[3];
    float old_value;
    float new_value;

    void clear() {
        param = 0;
        padding[0] = padding[1] = padding[2] = 0;
        old_value = 0;
        new_value = 0;
    }
};

static_assert(sizeof(ParamChange) == 12, "ParamChange must be 12 bytes");

// ============================================================================
// TunerDecision - Full details of a single tuning decision
// ============================================================================

/**
 * Stores complete tuner decision details.
 * Size: ~384 bytes (no 128 byte limit!)
 */
struct TunerDecision {
    // =========================================================================
    // Metadata (24 bytes)
    // =========================================================================
    uint64_t timestamp_ns; // Decision timestamp (steady_clock)
    uint32_t sequence;     // Monotonic sequence number
    uint8_t confidence;    // AI confidence (0-100)
    uint8_t action;        // TunerCommand::Action enum
    uint8_t urgency;       // 0=low, 1=medium, 2=high
    uint8_t concern;       // TunerConcern enum (why)
    uint8_t padding1[8];

    // =========================================================================
    // Symbol (16 bytes)
    // =========================================================================
    char symbol[TUNER_SYMBOL_LEN]; // "BTCUSDT\0"

    // =========================================================================
    // AI Reason (256 bytes) - FULL TEXT!
    // =========================================================================
    char reason[MAX_REASON_LEN];

    // =========================================================================
    // Parameter Changes (72 bytes)
    // =========================================================================
    ParamChange changes[MAX_PARAM_CHANGES];

    // =========================================================================
    // Change Count (8 bytes)
    // =========================================================================
    uint8_t num_changes;
    uint8_t padding2[7];

    // =========================================================================
    // Helper Methods
    // =========================================================================

    void clear() { std::memset(this, 0, sizeof(*this)); }

    void set_symbol(const char* s) {
        std::strncpy(symbol, s, sizeof(symbol) - 1);
        symbol[sizeof(symbol) - 1] = '\0';
    }

    void set_reason(const char* r) {
        std::strncpy(reason, r, sizeof(reason) - 1);
        reason[sizeof(reason) - 1] = '\0';
    }

    bool add_change(TunerParam p, float old_v, float new_v) {
        if (num_changes >= MAX_PARAM_CHANGES)
            return false;
        changes[num_changes].param = static_cast<uint8_t>(p);
        changes[num_changes].old_value = old_v;
        changes[num_changes].new_value = new_v;
        num_changes++;
        return true;
    }

    bool add_change(uint8_t p, float old_v, float new_v) {
        return add_change(static_cast<TunerParam>(p), old_v, new_v);
    }

    // Accessors
    TunerConcern get_concern() const { return static_cast<TunerConcern>(concern); }

    const char* get_concern_name() const { return TradeEvent::concern_name(get_concern()); }

    bool has_changes() const { return num_changes > 0; }

    bool is_valid() const { return sequence > 0; }
};

static_assert(sizeof(TunerDecision) == 376, "TunerDecision size check");

// ============================================================================
// SharedTunerState - Shared memory structure with ring buffer
// ============================================================================

/**
 * Shared memory structure for tuner decisions.
 * Contains a ring buffer of recent decisions.
 */
struct SharedTunerState {
    static constexpr const char* SHM_NAME = "/tuner_decisions";
    static constexpr uint32_t MAGIC = 0x54554E52; // "TUNR"
    static constexpr uint32_t VERSION = 1;

    // =========================================================================
    // Header (64 bytes, cache-line aligned)
    // =========================================================================
    uint32_t magic;
    uint32_t version;
    std::atomic<uint32_t> write_index;     // Current write position in ring buffer
    std::atomic<uint32_t> total_decisions; // Total decisions ever written
    std::atomic<uint64_t> last_update_ns;  // Last write timestamp
    uint8_t padding[40];

    // =========================================================================
    // Ring Buffer of Decisions
    // =========================================================================
    TunerDecision decisions[MAX_TUNER_HISTORY];

    // =========================================================================
    // Initialization
    // =========================================================================

    void init() {
        magic = MAGIC;
        version = VERSION;
        write_index.store(0, std::memory_order_relaxed);
        total_decisions.store(0, std::memory_order_relaxed);
        last_update_ns.store(0, std::memory_order_relaxed);
        std::memset(padding, 0, sizeof(padding));
        for (auto& d : decisions) {
            d.clear();
        }
    }

    bool is_valid() const { return magic == MAGIC && version == VERSION; }

    // =========================================================================
    // Writer Methods (trader_tuner)
    // =========================================================================

    /**
     * Get next slot for writing.
     * Call commit_write() after filling in the decision.
     */
    TunerDecision* write_next() {
        uint32_t idx = write_index.load(std::memory_order_relaxed);
        uint32_t next = (idx + 1) % MAX_TUNER_HISTORY;
        decisions[next].clear();
        return &decisions[next];
    }

    /**
     * Commit the write - makes it visible to readers.
     */
    void commit_write() {
        uint32_t idx = write_index.load(std::memory_order_relaxed);
        uint32_t next = (idx + 1) % MAX_TUNER_HISTORY;

        // Set sequence number
        uint32_t total = total_decisions.fetch_add(1, std::memory_order_relaxed) + 1;
        decisions[next].sequence = total;

        // Update timestamp
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        decisions[next].timestamp_ns = now_ns;
        last_update_ns.store(now_ns, std::memory_order_relaxed);

        // Advance write index (release to make all writes visible)
        write_index.store(next, std::memory_order_release);
    }

    // =========================================================================
    // Reader Methods (trader_dashboard)
    // =========================================================================

    /**
     * Get the most recent decision.
     * Returns nullptr if no decisions yet.
     */
    const TunerDecision* get_latest() const {
        uint32_t idx = write_index.load(std::memory_order_acquire);
        const auto& d = decisions[idx];
        if (d.sequence == 0)
            return nullptr;
        return &d;
    }

    /**
     * Get decision by offset from latest.
     * offset=0 is latest, offset=1 is second most recent, etc.
     */
    const TunerDecision* get_by_offset(size_t offset) const {
        if (offset >= MAX_TUNER_HISTORY)
            return nullptr;

        uint32_t total = total_decisions.load(std::memory_order_relaxed);
        if (offset >= total)
            return nullptr;

        uint32_t idx = write_index.load(std::memory_order_acquire);
        uint32_t pos = (idx + MAX_TUNER_HISTORY - offset) % MAX_TUNER_HISTORY;
        const auto& d = decisions[pos];
        if (d.sequence == 0)
            return nullptr;
        return &d;
    }

    /**
     * Iterate over recent decisions (newest first).
     * Callback signature: void(const TunerDecision& decision)
     */
    template <typename Callback>
    void for_recent_decisions(size_t count, Callback&& cb) const {
        uint32_t total = total_decisions.load(std::memory_order_relaxed);
        if (total == 0)
            return;

        size_t actual = std::min(count, static_cast<size_t>(std::min(total, static_cast<uint32_t>(MAX_TUNER_HISTORY))));
        uint32_t idx = write_index.load(std::memory_order_acquire);

        for (size_t i = 0; i < actual; ++i) {
            const auto& d = decisions[idx];
            if (d.sequence > 0) {
                cb(d);
            }
            idx = (idx == 0) ? MAX_TUNER_HISTORY - 1 : idx - 1;
        }
    }

    /**
     * Get count of available decisions in ring buffer.
     */
    size_t available_count() const {
        uint32_t total = total_decisions.load(std::memory_order_relaxed);
        return std::min(static_cast<size_t>(total), MAX_TUNER_HISTORY);
    }

    /**
     * Check if there are new decisions since last check.
     */
    bool has_new_since(uint32_t last_seen_seq) const {
        uint32_t total = total_decisions.load(std::memory_order_relaxed);
        return total > last_seen_seq;
    }

    // =========================================================================
    // Shared Memory Factory
    // =========================================================================

    /**
     * Create new shared memory (writer - trader_tuner).
     */
    static SharedTunerState* create(const char* name = SHM_NAME) {
        int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
        if (fd < 0)
            return nullptr;

        if (ftruncate(fd, sizeof(SharedTunerState)) < 0) {
            ::close(fd);
            return nullptr;
        }

        void* ptr = mmap(nullptr, sizeof(SharedTunerState), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);

        if (ptr == MAP_FAILED)
            return nullptr;

        auto* state = static_cast<SharedTunerState*>(ptr);
        state->init();
        return state;
    }

    /**
     * Open existing shared memory read-only (reader - dashboard).
     */
    static SharedTunerState* open(const char* name = SHM_NAME) {
        int fd = shm_open(name, O_RDONLY, 0666);
        if (fd < 0)
            return nullptr;

        void* ptr = mmap(nullptr, sizeof(SharedTunerState), PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);

        if (ptr == MAP_FAILED)
            return nullptr;

        auto* state = static_cast<SharedTunerState*>(ptr);
        if (!state->is_valid()) {
            munmap(ptr, sizeof(SharedTunerState));
            return nullptr;
        }
        return state;
    }

    /**
     * Open existing shared memory read-write.
     */
    static SharedTunerState* open_rw(const char* name = SHM_NAME) {
        int fd = shm_open(name, O_RDWR, 0666);
        if (fd < 0)
            return nullptr;

        void* ptr = mmap(nullptr, sizeof(SharedTunerState), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);

        if (ptr == MAP_FAILED)
            return nullptr;

        auto* state = static_cast<SharedTunerState*>(ptr);
        if (!state->is_valid()) {
            munmap(ptr, sizeof(SharedTunerState));
            return nullptr;
        }
        return state;
    }

    /**
     * Close and unmap shared memory.
     */
    static void close(SharedTunerState* state) {
        if (state) {
            munmap(state, sizeof(SharedTunerState));
        }
    }

    /**
     * Remove shared memory segment.
     */
    static void destroy(const char* name = SHM_NAME) { shm_unlink(name); }
};

// Size check
static_assert(sizeof(SharedTunerState) == 64 + (376 * 16), "SharedTunerState size check");

} // namespace ipc
} // namespace hft
