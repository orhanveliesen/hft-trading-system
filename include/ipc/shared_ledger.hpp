#pragma once

#include "../util/string_utils.hpp"

/**
 * SharedLedger - Shared memory ledger for real-time transaction monitoring
 *
 * This provides a circular buffer of recent transactions that can be read
 * by dashboards for debugging and audit purposes.
 *
 * Layout:
 *   - Header with magic, version, counts
 *   - Fixed array of SharedLedgerEntry (circular buffer)
 *   - All numeric fields use fixed-point for atomic operations
 *
 * Usage:
 *   Writer (trader):
 *     auto ledger = SharedLedger::create("/trader_ledger");
 *     ledger->append_entry(entry);
 *
 *   Reader (dashboard):
 *     auto ledger = SharedLedger::open("/trader_ledger");
 *     for (size_t i = 0; i < ledger->count(); i++) {
 *         auto* e = ledger->entry(i);
 *         // display entry
 *     }
 */

#include <atomic>
#include <cstring>
#include <ctime>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace hft {
namespace ipc {

// Fixed-point scaling (same as SharedPortfolioState)
static constexpr double LEDGER_FIXED_SCALE = 1e8;

// Maximum entries in shared ledger
static constexpr size_t MAX_SHARED_LEDGER_ENTRIES = 1000;  // ~160KB total

/**
 * SharedLedgerEntry - Single transaction record for IPC
 * All double fields converted to int64_t with fixed-point scaling
 * ~160 bytes per entry
 */
struct SharedLedgerEntry {
    // Identity
    std::atomic<uint64_t> timestamp_ns;
    std::atomic<uint32_t> sequence;
    std::atomic<uint32_t> symbol;
    char ticker[12];

    // Transaction details (fixed-point)
    std::atomic<int64_t> price_x8;
    std::atomic<int64_t> quantity_x8;
    std::atomic<int64_t> commission_x8;

    // Cash flow (fixed-point)
    std::atomic<int64_t> cash_before_x8;
    std::atomic<int64_t> cash_after_x8;
    std::atomic<int64_t> cash_expected_x8;

    // Calculation breakdown (fixed-point)
    std::atomic<int64_t> trade_value_x8;
    std::atomic<int64_t> expected_cash_change_x8;

    // P&L (fixed-point)
    std::atomic<int64_t> realized_pnl_x8;
    std::atomic<int64_t> avg_entry_x8;
    std::atomic<int64_t> pnl_per_unit_x8;
    std::atomic<int64_t> expected_pnl_x8;

    // Position state (fixed-point)
    std::atomic<int64_t> position_qty_x8;
    std::atomic<int64_t> position_avg_x8;

    // Running totals (fixed-point)
    std::atomic<int64_t> running_realized_pnl_x8;
    std::atomic<int64_t> running_commission_x8;

    // Flags
    std::atomic<uint8_t> is_buy;
    std::atomic<uint8_t> is_exit;
    std::atomic<uint8_t> exit_reason;
    std::atomic<uint8_t> balance_ok;
    std::atomic<uint8_t> pnl_ok;
    std::atomic<uint8_t> valid;  // 1 = entry is valid and populated
    uint8_t padding[2];

    // Conversion helpers
    double price() const { return price_x8.load() / LEDGER_FIXED_SCALE; }
    double quantity() const { return quantity_x8.load() / LEDGER_FIXED_SCALE; }
    double commission() const { return commission_x8.load() / LEDGER_FIXED_SCALE; }
    double cash_before() const { return cash_before_x8.load() / LEDGER_FIXED_SCALE; }
    double cash_after() const { return cash_after_x8.load() / LEDGER_FIXED_SCALE; }
    double cash_expected() const { return cash_expected_x8.load() / LEDGER_FIXED_SCALE; }
    double trade_value() const { return trade_value_x8.load() / LEDGER_FIXED_SCALE; }
    double expected_cash_change() const { return expected_cash_change_x8.load() / LEDGER_FIXED_SCALE; }
    double realized_pnl() const { return realized_pnl_x8.load() / LEDGER_FIXED_SCALE; }
    double avg_entry() const { return avg_entry_x8.load() / LEDGER_FIXED_SCALE; }
    double pnl_per_unit() const { return pnl_per_unit_x8.load() / LEDGER_FIXED_SCALE; }
    double expected_pnl() const { return expected_pnl_x8.load() / LEDGER_FIXED_SCALE; }
    double position_qty() const { return position_qty_x8.load() / LEDGER_FIXED_SCALE; }
    double position_avg() const { return position_avg_x8.load() / LEDGER_FIXED_SCALE; }
    double running_realized_pnl() const { return running_realized_pnl_x8.load() / LEDGER_FIXED_SCALE; }
    double running_commission() const { return running_commission_x8.load() / LEDGER_FIXED_SCALE; }

    double cash_discrepancy() const { return cash_after() - cash_expected(); }
    double pnl_discrepancy() const { return realized_pnl() - expected_pnl(); }
    bool has_mismatch() const { return balance_ok.load() == 0 || pnl_ok.load() == 0; }

    void clear() {
        timestamp_ns.store(0);
        sequence.store(0);
        symbol.store(0);
        std::memset(ticker, 0, sizeof(ticker));
        price_x8.store(0);
        quantity_x8.store(0);
        commission_x8.store(0);
        cash_before_x8.store(0);
        cash_after_x8.store(0);
        cash_expected_x8.store(0);
        trade_value_x8.store(0);
        expected_cash_change_x8.store(0);
        realized_pnl_x8.store(0);
        avg_entry_x8.store(0);
        pnl_per_unit_x8.store(0);
        expected_pnl_x8.store(0);
        position_qty_x8.store(0);
        position_avg_x8.store(0);
        running_realized_pnl_x8.store(0);
        running_commission_x8.store(0);
        is_buy.store(0);
        is_exit.store(0);
        exit_reason.store(0);
        balance_ok.store(1);
        pnl_ok.store(1);
        valid.store(0);
    }
};

/**
 * SharedLedger - Main shared memory ledger structure
 */
struct SharedLedger {
    // Magic number for validation
    static constexpr uint64_t MAGIC = 0x4846544C45444752ULL;  // "HFTLEDGR"
#ifdef TRADER_BUILD_HASH
    static constexpr uint32_t VERSION = util::hex_to_u32(TRADER_BUILD_HASH);
#else
    static constexpr uint32_t VERSION = 0;
#endif

    // Header
    uint64_t magic;
    uint32_t version;
    uint32_t session_id;

    // Circular buffer state
    std::atomic<size_t> entry_count;    // Total entries (up to MAX)
    std::atomic<size_t> head;           // Index of oldest entry
    std::atomic<uint32_t> next_seq;     // Next sequence number
    std::atomic<uint32_t> write_lock;   // Simple spinlock for writes

    // Statistics
    std::atomic<size_t> total_entries;  // Total entries ever written
    std::atomic<size_t> mismatch_count; // Total mismatches detected

    uint8_t padding[32];  // Alignment

    // Circular buffer of entries
    SharedLedgerEntry entries[MAX_SHARED_LEDGER_ENTRIES];

    // === Accessors (for readers) ===

    size_t count() const {
        return entry_count.load();
    }

    const SharedLedgerEntry* entry(size_t index) const {
        size_t cnt = entry_count.load();
        if (index >= cnt) return nullptr;
        size_t h = head.load();
        size_t actual_idx = (h + index) % MAX_SHARED_LEDGER_ENTRIES;
        return &entries[actual_idx];
    }

    const SharedLedgerEntry* last() const {
        size_t cnt = entry_count.load();
        if (cnt == 0) return nullptr;
        return entry(cnt - 1);
    }

    const SharedLedgerEntry* first() const {
        if (entry_count.load() == 0) return nullptr;
        return entry(0);
    }

    size_t check_mismatches() const {
        size_t cnt = 0;
        for (size_t i = 0; i < entry_count.load(); i++) {
            auto* e = entry(i);
            if (e && e->has_mismatch()) cnt++;
        }
        return cnt;
    }

    // === Mutators (for writer) ===

    SharedLedgerEntry* append() {
        // Simple spinlock
        while (write_lock.exchange(1) == 1) {}

        size_t write_idx;
        size_t cnt = entry_count.load();

        if (cnt < MAX_SHARED_LEDGER_ENTRIES) {
            // Not full yet
            write_idx = cnt;
            entry_count.store(cnt + 1);
        } else {
            // Full - overwrite oldest
            write_idx = head.load();
            head.store((write_idx + 1) % MAX_SHARED_LEDGER_ENTRIES);
        }

        auto& e = entries[write_idx];
        e.clear();
        e.sequence.store(next_seq.fetch_add(1) + 1);
        e.valid.store(1);

        total_entries.fetch_add(1);

        write_lock.store(0);  // Release lock

        return &e;
    }

    void record_mismatch() {
        mismatch_count.fetch_add(1);
    }

    // === Initialization ===

    void init() {
        magic = MAGIC;
        version = VERSION;
        session_id = static_cast<uint32_t>(std::time(nullptr));

        entry_count.store(0);
        head.store(0);
        next_seq.store(0);
        write_lock.store(0);
        total_entries.store(0);
        mismatch_count.store(0);

        for (size_t i = 0; i < MAX_SHARED_LEDGER_ENTRIES; i++) {
            entries[i].clear();
        }
    }

    bool is_valid() const {
        return magic == MAGIC && version == VERSION;
    }

    // === Shared Memory Factory ===

    static SharedLedger* create(const char* name) {
        int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
        if (fd < 0) return nullptr;

        if (ftruncate(fd, sizeof(SharedLedger)) < 0) {
            close(fd);
            return nullptr;
        }

        void* ptr = mmap(nullptr, sizeof(SharedLedger),
                         PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);

        if (ptr == MAP_FAILED) return nullptr;

        auto* ledger = static_cast<SharedLedger*>(ptr);
        ledger->init();
        return ledger;
    }

    static SharedLedger* open(const char* name) {
        int fd = shm_open(name, O_RDONLY, 0666);
        if (fd < 0) return nullptr;

        void* ptr = mmap(nullptr, sizeof(SharedLedger),
                         PROT_READ, MAP_SHARED, fd, 0);
        close(fd);

        if (ptr == MAP_FAILED) return nullptr;

        auto* ledger = static_cast<SharedLedger*>(ptr);
        if (!ledger->is_valid()) {
            munmap(ptr, sizeof(SharedLedger));
            return nullptr;
        }
        return ledger;
    }

    static SharedLedger* open_rw(const char* name) {
        int fd = shm_open(name, O_RDWR, 0666);
        if (fd < 0) return nullptr;

        void* ptr = mmap(nullptr, sizeof(SharedLedger),
                         PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);

        if (ptr == MAP_FAILED) return nullptr;

        auto* ledger = static_cast<SharedLedger*>(ptr);
        if (!ledger->is_valid()) {
            munmap(ptr, sizeof(SharedLedger));
            return nullptr;
        }
        return ledger;
    }

    static void destroy(const char* name) {
        shm_unlink(name);
    }

    static void unmap(SharedLedger* ledger) {
        if (ledger) {
            munmap(ledger, sizeof(SharedLedger));
        }
    }
};

// Size verification
static_assert(sizeof(SharedLedgerEntry) % 8 == 0, "SharedLedgerEntry must be 8-byte aligned");

}  // namespace ipc
}  // namespace hft
