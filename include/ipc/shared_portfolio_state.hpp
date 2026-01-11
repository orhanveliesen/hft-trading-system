#pragma once

/**
 * SharedPortfolioState - Shared memory portfolio state for real-time monitoring
 *
 * This provides a snapshot of the current portfolio state that can be read
 * by observers/dashboards at any time, even if they miss individual events.
 *
 * Layout:
 *   - Header with global stats (cash, P&L, trade counts)
 *   - Fixed array of position slots for each symbol
 *   - All fields are atomic for lock-free reads
 *
 * Usage:
 *   Writer (hft):
 *     auto state = SharedPortfolioState::create("/hft_portfolio");
 *     state->update_cash(99000.0);
 *     state->update_position(0, "BTCUSDT", 0.1, 91000.0, 91500.0);
 *
 *   Reader (dashboard):
 *     auto state = SharedPortfolioState::open("/hft_portfolio");
 *     double cash = state->cash();
 *     auto pos = state->get_position(0);
 */

#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace hft {
namespace ipc {

// Maximum number of symbols we can track
static constexpr size_t MAX_PORTFOLIO_SYMBOLS = 64;

// Position data for a single symbol
struct PositionSlot {
    char symbol[16];                    // Symbol name (null-terminated)
    std::atomic<int64_t> quantity_x8;   // Quantity * 1e8 (for atomic int ops)
    std::atomic<int64_t> avg_price_x8;  // Avg entry price * 1e8
    std::atomic<int64_t> last_price_x8; // Last market price * 1e8
    std::atomic<int64_t> realized_pnl_x8; // Realized P&L * 1e8
    std::atomic<uint32_t> buy_count;    // Number of buys
    std::atomic<uint32_t> sell_count;   // Number of sells
    std::atomic<uint8_t> active;        // Is this slot in use?
    std::atomic<uint8_t> regime;        // Current market regime (0=Unknown, 1=TrendingUp, etc.)
    uint8_t padding[6];                 // Align to 8 bytes

    void clear() {
        std::memset(symbol, 0, sizeof(symbol));
        quantity_x8.store(0);
        avg_price_x8.store(0);
        last_price_x8.store(0);
        realized_pnl_x8.store(0);
        buy_count.store(0);
        sell_count.store(0);
        active.store(0);
        regime.store(0);
    }

    // Conversion helpers (atomic int64 <-> double)
    double quantity() const { return quantity_x8.load() / 1e8; }
    double avg_price() const { return avg_price_x8.load() / 1e8; }
    double last_price() const { return last_price_x8.load() / 1e8; }
    double realized_pnl() const { return realized_pnl_x8.load() / 1e8; }

    double unrealized_pnl() const {
        double qty = quantity();
        if (qty == 0) return 0;
        return qty * (last_price() - avg_price());
    }

    double market_value() const {
        return quantity() * last_price();
    }
};

// Convert 8-char hex string to uint32_t at compile time
constexpr uint32_t portfolio_hex_to_u32(const char* s) {
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

// Main shared portfolio state structure
struct SharedPortfolioState {
    // Magic number for validation
    static constexpr uint64_t MAGIC = 0x48465450464F4C49ULL;  // "HFTPFOLI"
#ifdef HFT_BUILD_HASH
    static constexpr uint32_t VERSION = portfolio_hex_to_u32(HFT_BUILD_HASH);
#else
    static constexpr uint32_t VERSION = 0;
#endif

    // Header
    uint64_t magic;
    uint32_t version;
    uint32_t session_id;                // Random session identifier
    std::atomic<uint32_t> sequence;     // Incremented on each update

    // Global portfolio state
    std::atomic<int64_t> cash_x8;       // Available cash * 1e8
    std::atomic<int64_t> initial_cash_x8; // Starting cash * 1e8
    std::atomic<int64_t> total_realized_pnl_x8;
    std::atomic<uint64_t> total_events;
    std::atomic<uint32_t> winning_trades;
    std::atomic<uint32_t> losing_trades;
    std::atomic<uint32_t> total_fills;
    std::atomic<uint32_t> total_targets;
    std::atomic<uint32_t> total_stops;
    std::atomic<int64_t> start_time_ns; // Epoch nanoseconds
    std::atomic<uint8_t> trading_active;
    uint8_t padding[7];

    // Position slots
    PositionSlot positions[MAX_PORTFOLIO_SYMBOLS];

    // === Accessors ===
    double cash() const { return cash_x8.load() / 1e8; }
    double initial_cash() const { return initial_cash_x8.load() / 1e8; }
    double total_realized_pnl() const { return total_realized_pnl_x8.load() / 1e8; }

    double total_unrealized_pnl() const {
        double total = 0;
        for (size_t i = 0; i < MAX_PORTFOLIO_SYMBOLS; ++i) {
            if (positions[i].active.load()) {
                total += positions[i].unrealized_pnl();
            }
        }
        return total;
    }

    double total_equity() const {
        return total_realized_pnl() + total_unrealized_pnl();
    }

    double win_rate() const {
        uint32_t wins = winning_trades.load();
        uint32_t losses = losing_trades.load();
        uint32_t total = wins + losses;
        return total > 0 ? (double)wins / total * 100.0 : 0.0;
    }

    // === Mutators (for writer) ===
    void set_cash(double value) {
        cash_x8.store(static_cast<int64_t>(value * 1e8));
        sequence.fetch_add(1);
    }

    void set_initial_cash(double value) {
        initial_cash_x8.store(static_cast<int64_t>(value * 1e8));
    }

    void add_realized_pnl(double pnl) {
        int64_t pnl_x8 = static_cast<int64_t>(pnl * 1e8);
        total_realized_pnl_x8.fetch_add(pnl_x8);
        if (pnl > 0) {
            winning_trades.fetch_add(1);
        } else if (pnl < 0) {
            losing_trades.fetch_add(1);
        }
        sequence.fetch_add(1);
    }

    void record_fill() { total_fills.fetch_add(1); }
    void record_target() { total_targets.fetch_add(1); }
    void record_stop() { total_stops.fetch_add(1); }
    void record_event() { total_events.fetch_add(1); }

    // Find or create position slot for symbol
    PositionSlot* get_or_create_position(const char* symbol) {
        // First, try to find existing
        for (size_t i = 0; i < MAX_PORTFOLIO_SYMBOLS; ++i) {
            if (positions[i].active.load() &&
                std::strncmp(positions[i].symbol, symbol, 15) == 0) {
                return &positions[i];
            }
        }
        // Find empty slot
        for (size_t i = 0; i < MAX_PORTFOLIO_SYMBOLS; ++i) {
            if (!positions[i].active.load()) {
                positions[i].clear();
                std::strncpy(positions[i].symbol, symbol, 15);
                positions[i].symbol[15] = '\0';
                positions[i].active.store(1);
                return &positions[i];
            }
        }
        return nullptr;  // No slots available
    }

    void update_position(const char* symbol, double qty, double avg_price,
                         double last_price, double realized = 0) {
        PositionSlot* pos = get_or_create_position(symbol);
        if (pos) {
            pos->quantity_x8.store(static_cast<int64_t>(qty * 1e8));
            pos->avg_price_x8.store(static_cast<int64_t>(avg_price * 1e8));
            pos->last_price_x8.store(static_cast<int64_t>(last_price * 1e8));
            if (realized != 0) {
                pos->realized_pnl_x8.fetch_add(static_cast<int64_t>(realized * 1e8));
            }
            sequence.fetch_add(1);
        }
    }

    void update_last_price(const char* symbol, double price) {
        // Get or create position slot - ensures all tracked symbols show prices
        PositionSlot* pos = get_or_create_position(symbol);
        if (pos) {
            pos->last_price_x8.store(static_cast<int64_t>(price * 1e8));
            // Note: not incrementing sequence here to reduce overhead (called every tick)
        }
    }

    // Fast path: Direct index access (no search) - use when symbol_id is known
    void update_last_price_fast(size_t symbol_id, double price) {
        if (symbol_id < MAX_PORTFOLIO_SYMBOLS) {
            positions[symbol_id].last_price_x8.store(static_cast<int64_t>(price * 1e8));
        }
    }

    void update_position_fast(size_t symbol_id, double qty, double avg_price,
                              double last_price, double realized = 0) {
        if (symbol_id >= MAX_PORTFOLIO_SYMBOLS) return;
        auto& pos = positions[symbol_id];
        pos.quantity_x8.store(static_cast<int64_t>(qty * 1e8));
        pos.avg_price_x8.store(static_cast<int64_t>(avg_price * 1e8));
        pos.last_price_x8.store(static_cast<int64_t>(last_price * 1e8));
        if (realized != 0) {
            pos.realized_pnl_x8.fetch_add(static_cast<int64_t>(realized * 1e8));
        }
        sequence.fetch_add(1);
    }

    // Initialize slot with symbol name (call once at startup)
    void init_slot(size_t symbol_id, const char* symbol) {
        if (symbol_id >= MAX_PORTFOLIO_SYMBOLS) return;
        auto& pos = positions[symbol_id];
        pos.clear();
        std::strncpy(pos.symbol, symbol, 15);
        pos.symbol[15] = '\0';
        pos.active.store(1);
    }

    void update_regime(const char* symbol, uint8_t regime) {
        PositionSlot* pos = get_or_create_position(symbol);
        if (pos) {
            pos->regime.store(regime);
            sequence.fetch_add(1);
        }
    }

    void record_buy(const char* symbol) {
        PositionSlot* pos = get_or_create_position(symbol);
        if (pos) pos->buy_count.fetch_add(1);
    }

    void record_sell(const char* symbol) {
        PositionSlot* pos = get_or_create_position(symbol);
        if (pos) pos->sell_count.fetch_add(1);
    }

    // === Initialization ===
    void init(double starting_cash) {
        magic = MAGIC;
        version = VERSION;

        // Generate random session ID
        std::random_device rd;
        session_id = rd();

        sequence.store(0);
        cash_x8.store(static_cast<int64_t>(starting_cash * 1e8));
        initial_cash_x8.store(static_cast<int64_t>(starting_cash * 1e8));
        total_realized_pnl_x8.store(0);
        total_events.store(0);
        winning_trades.store(0);
        losing_trades.store(0);
        total_fills.store(0);
        total_targets.store(0);
        total_stops.store(0);
        start_time_ns.store(std::chrono::steady_clock::now().time_since_epoch().count());
        trading_active.store(1);

        for (size_t i = 0; i < MAX_PORTFOLIO_SYMBOLS; ++i) {
            positions[i].clear();
        }
    }

    bool is_valid() const {
        return magic == MAGIC && version == VERSION;
    }

    // === Shared Memory Factory ===
    static SharedPortfolioState* create(const char* name, double starting_cash = 100000.0) {
        int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
        if (fd < 0) return nullptr;

        if (ftruncate(fd, sizeof(SharedPortfolioState)) < 0) {
            close(fd);
            return nullptr;
        }

        void* ptr = mmap(nullptr, sizeof(SharedPortfolioState),
                         PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);

        if (ptr == MAP_FAILED) return nullptr;

        auto* state = static_cast<SharedPortfolioState*>(ptr);
        state->init(starting_cash);
        return state;
    }

    // Open for reading only (dashboard/observer use)
    static SharedPortfolioState* open(const char* name) {
        int fd = shm_open(name, O_RDONLY, 0666);
        if (fd < 0) return nullptr;

        void* ptr = mmap(nullptr, sizeof(SharedPortfolioState),
                         PROT_READ, MAP_SHARED, fd, 0);
        close(fd);

        if (ptr == MAP_FAILED) return nullptr;

        auto* state = static_cast<SharedPortfolioState*>(ptr);
        if (!state->is_valid()) {
            munmap(ptr, sizeof(SharedPortfolioState));
            return nullptr;
        }
        return state;
    }

    // Open for read-write (crash recovery - writer process)
    static SharedPortfolioState* open_rw(const char* name) {
        int fd = shm_open(name, O_RDWR, 0666);
        if (fd < 0) return nullptr;

        void* ptr = mmap(nullptr, sizeof(SharedPortfolioState),
                         PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);

        if (ptr == MAP_FAILED) return nullptr;

        auto* state = static_cast<SharedPortfolioState*>(ptr);
        if (!state->is_valid()) {
            munmap(ptr, sizeof(SharedPortfolioState));
            return nullptr;
        }
        return state;
    }

    static void destroy(const char* name) {
        shm_unlink(name);
    }
};

static_assert(sizeof(PositionSlot) == 64, "PositionSlot size mismatch");

}  // namespace ipc
}  // namespace hft
