#pragma once

#include "../util/string_utils.hpp"

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
#include <fcntl.h>
#include <random>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hft {
namespace ipc {

// Fixed-point scaling factor for atomic int64 <-> double conversions
// Using 1e8 provides 8 decimal places of precision (sufficient for crypto prices)
static constexpr double FIXED_POINT_SCALE = 1e8;

// Maximum number of symbols we can track
static constexpr size_t MAX_PORTFOLIO_SYMBOLS = 64;

// Position snapshot for tracking OHLC and technical indicators
// Size: 6x int64_t (48) + int32_t (4) + int8_t (1) + uint32_t (4) + padding (15) = 72 bytes
struct PositionSnapshot {
    std::atomic<int64_t> price_open_x8{0};   // Opening price * FIXED_POINT_SCALE
    std::atomic<int64_t> price_high_x8{0};   // High price * FIXED_POINT_SCALE
    std::atomic<int64_t> price_low_x8{0};    // Low price * FIXED_POINT_SCALE
    std::atomic<int64_t> ema_20_x8{0};       // 20-period EMA * FIXED_POINT_SCALE
    std::atomic<int64_t> atr_14_x8{0};       // 14-period ATR * FIXED_POINT_SCALE (or BB width)
    std::atomic<int64_t> volume_sum_x8{0};   // Sum of volume * FIXED_POINT_SCALE
    std::atomic<int32_t> volatility_x100{0}; // Volatility * 100
    std::atomic<int8_t> trend_direction{0};  // -1, 0, +1
    std::atomic<uint32_t> tick_count{0};     // Number of ticks received

    void clear() {
        price_open_x8.store(0);
        price_high_x8.store(0);
        price_low_x8.store(0);
        ema_20_x8.store(0);
        atr_14_x8.store(0);
        volume_sum_x8.store(0);
        volatility_x100.store(0);
        trend_direction.store(0);
        tick_count.store(0);
    }

    double price_open() const { return price_open_x8.load() / FIXED_POINT_SCALE; }
    double price_high() const { return price_high_x8.load() / FIXED_POINT_SCALE; }
    double price_low() const { return price_low_x8.load() / FIXED_POINT_SCALE; }
    double ema_20() const { return ema_20_x8.load() / FIXED_POINT_SCALE; }
    double atr_14() const { return atr_14_x8.load() / FIXED_POINT_SCALE; }
    double volume_sum() const { return volume_sum_x8.load() / FIXED_POINT_SCALE; }
    double volatility() const { return volatility_x100.load() / 100.0; }
    double volatility_pct() const { return volatility(); }

    double price_range_pct() const {
        double high = price_high();
        double low = price_low();
        if (low <= 0)
            return 0.0;
        return (high - low) / low * 100.0;
    }
};

// Position data for a single symbol
struct PositionSlot {
    char symbol[16];                      // Symbol name (null-terminated)
    std::atomic<int64_t> quantity_x8;     // Quantity * FIXED_POINT_SCALE (for atomic int ops)
    std::atomic<int64_t> avg_price_x8;    // Avg entry price * FIXED_POINT_SCALE
    std::atomic<int64_t> last_price_x8;   // Last market price * FIXED_POINT_SCALE
    std::atomic<int64_t> realized_pnl_x8; // Realized P&L * FIXED_POINT_SCALE
    std::atomic<uint32_t> buy_count;      // Number of buys
    std::atomic<uint32_t> sell_count;     // Number of sells
    std::atomic<uint8_t> active;          // Is this slot in use?
    std::atomic<uint8_t> regime;          // Current market regime (0=Unknown, 1=TrendingUp, etc.)
    PositionSnapshot snapshot;            // Last snapshot for tracking changes
    uint8_t padding[6];                   // Align to 8 bytes

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
        snapshot.clear();
    }

    // Conversion helpers (atomic int64 <-> double)
    double quantity() const { return quantity_x8.load() / FIXED_POINT_SCALE; }
    double avg_price() const { return avg_price_x8.load() / FIXED_POINT_SCALE; }
    double last_price() const { return last_price_x8.load() / FIXED_POINT_SCALE; }
    double realized_pnl() const { return realized_pnl_x8.load() / FIXED_POINT_SCALE; }

    double unrealized_pnl() const {
        double qty = quantity();
        if (qty == 0)
            return 0;
        return qty * (last_price() - avg_price());
    }

    double market_value() const { return quantity() * last_price(); }
};

// Main shared portfolio state structure
struct SharedPortfolioState {
    // Magic number for validation
    static constexpr uint64_t MAGIC = 0x48465450464F4C49ULL; // "HFTPFOLI"
#ifdef HFT_BUILD_HASH
    static constexpr uint32_t VERSION = util::hex_to_u32(HFT_BUILD_HASH);
#else
    static constexpr uint32_t VERSION = 0;
#endif

    // Header
    uint64_t magic;
    uint32_t version;
    uint32_t session_id;            // Random session identifier
    std::atomic<uint32_t> sequence; // Incremented on each update

    // Global portfolio state
    std::atomic<int64_t> cash_x8;         // Available cash * FIXED_POINT_SCALE
    std::atomic<int64_t> initial_cash_x8; // Starting cash * FIXED_POINT_SCALE
    std::atomic<int64_t> total_realized_pnl_x8;
    std::atomic<uint64_t> total_events;
    std::atomic<uint32_t> winning_trades;
    std::atomic<uint32_t> losing_trades;
    std::atomic<uint32_t> total_fills;
    std::atomic<uint32_t> total_targets;
    std::atomic<uint32_t> total_stops;
    std::atomic<int64_t> start_time_ns; // Epoch nanoseconds
    std::atomic<uint8_t> trading_active;
    uint8_t padding1[7];

    // Trading costs tracking
    std::atomic<int64_t> total_slippage_x8;    // Total slippage * FIXED_POINT_SCALE
    std::atomic<int64_t> total_commissions_x8; // Total commissions * FIXED_POINT_SCALE
    std::atomic<int64_t> total_spread_cost_x8; // Total spread cost * FIXED_POINT_SCALE
    std::atomic<int64_t> total_volume_x8;      // Total volume traded * FIXED_POINT_SCALE

    // Position slots
    PositionSlot positions[MAX_PORTFOLIO_SYMBOLS];

    // === Accessors ===
    double cash() const { return cash_x8.load() / FIXED_POINT_SCALE; }
    double initial_cash() const { return initial_cash_x8.load() / FIXED_POINT_SCALE; }
    double total_realized_pnl() const { return total_realized_pnl_x8.load() / FIXED_POINT_SCALE; }

    double total_unrealized_pnl() const {
        double total = 0;
        for (size_t i = 0; i < MAX_PORTFOLIO_SYMBOLS; ++i) {
            if (positions[i].active.load()) {
                total += positions[i].unrealized_pnl();
            }
        }
        return total;
    }

    double total_market_value() const {
        double total = 0;
        for (size_t i = 0; i < MAX_PORTFOLIO_SYMBOLS; ++i) {
            if (positions[i].active.load()) {
                total += positions[i].market_value();
            }
        }
        return total;
    }

    // Equity = cash + market value of all positions
    double total_equity() const { return cash() + total_market_value(); }

    // P&L = current equity - initial capital
    double total_pnl() const { return total_equity() - initial_cash(); }

    double win_rate() const {
        uint32_t wins = winning_trades.load();
        uint32_t losses = losing_trades.load();
        uint32_t total = wins + losses;
        return total > 0 ? (double)wins / total * 100.0 : 0.0;
    }

    double total_slippage() const { return total_slippage_x8.load() / FIXED_POINT_SCALE; }
    double total_commissions() const { return total_commissions_x8.load() / FIXED_POINT_SCALE; }
    double total_spread_cost() const { return total_spread_cost_x8.load() / FIXED_POINT_SCALE; }
    double total_volume() const { return total_volume_x8.load() / FIXED_POINT_SCALE; }
    double total_costs() const { return total_slippage() + total_commissions() + total_spread_cost(); }

    double gross_pnl() const { return total_realized_pnl() + total_costs(); }

    double cost_per_trade() const {
        uint32_t fills = total_fills.load();
        return fills > 0 ? total_costs() / fills : 0.0;
    }

    double avg_trade_value() const {
        uint32_t fills = total_fills.load();
        return fills > 0 ? total_volume() / fills : 0.0;
    }

    double cost_pct_per_trade() const {
        double avg_val = avg_trade_value();
        return avg_val > 0 ? (cost_per_trade() / avg_val) * 100.0 : 0.0;
    }

    // === Mutators (for writer) ===
    void set_cash(double value) {
        cash_x8.store(static_cast<int64_t>(value * FIXED_POINT_SCALE));
        sequence.fetch_add(1);
    }

    void set_initial_cash(double value) { initial_cash_x8.store(static_cast<int64_t>(value * FIXED_POINT_SCALE)); }

    void add_realized_pnl(double pnl) {
        int64_t pnl_x8 = static_cast<int64_t>(pnl * FIXED_POINT_SCALE);
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

    void add_slippage(double value) { total_slippage_x8.fetch_add(static_cast<int64_t>(value * FIXED_POINT_SCALE)); }

    void add_commission(double value) {
        total_commissions_x8.fetch_add(static_cast<int64_t>(value * FIXED_POINT_SCALE));
    }

    void add_spread_cost(double value) {
        total_spread_cost_x8.fetch_add(static_cast<int64_t>(value * FIXED_POINT_SCALE));
    }

    void add_volume(double value) { total_volume_x8.fetch_add(static_cast<int64_t>(value * FIXED_POINT_SCALE)); }

    // Find or create position slot for symbol
    PositionSlot* get_or_create_position(const char* symbol) {
        // First, try to find existing
        for (size_t i = 0; i < MAX_PORTFOLIO_SYMBOLS; ++i) {
            if (positions[i].active.load() && std::strncmp(positions[i].symbol, symbol, 15) == 0) {
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
        return nullptr; // No slots available
    }

    void update_position(const char* symbol, double qty, double avg_price, double last_price, double realized = 0) {
        PositionSlot* pos = get_or_create_position(symbol);
        if (pos) {
            pos->quantity_x8.store(static_cast<int64_t>(qty * FIXED_POINT_SCALE));
            pos->avg_price_x8.store(static_cast<int64_t>(avg_price * FIXED_POINT_SCALE));
            pos->last_price_x8.store(static_cast<int64_t>(last_price * FIXED_POINT_SCALE));
            if (realized != 0) {
                pos->realized_pnl_x8.fetch_add(static_cast<int64_t>(realized * FIXED_POINT_SCALE));
            }
            sequence.fetch_add(1);
        }
    }

    void update_last_price(const char* symbol, double price) {
        // Get or create position slot - ensures all tracked symbols show prices
        PositionSlot* pos = get_or_create_position(symbol);
        if (pos) {
            pos->last_price_x8.store(static_cast<int64_t>(price * FIXED_POINT_SCALE));
            // Note: not incrementing sequence here to reduce overhead (called every tick)
        }
    }

    // Fast path: Direct index access (no search) - use when symbol_id is known
    void update_last_price_fast(size_t symbol_id, double price) {
        if (symbol_id < MAX_PORTFOLIO_SYMBOLS) {
            positions[symbol_id].last_price_x8.store(static_cast<int64_t>(price * FIXED_POINT_SCALE));
        }
    }

    void update_position_fast(size_t symbol_id, double qty, double avg_price, double last_price, double realized = 0) {
        if (symbol_id >= MAX_PORTFOLIO_SYMBOLS)
            return;
        auto& pos = positions[symbol_id];
        pos.quantity_x8.store(static_cast<int64_t>(qty * FIXED_POINT_SCALE));
        pos.avg_price_x8.store(static_cast<int64_t>(avg_price * FIXED_POINT_SCALE));
        pos.last_price_x8.store(static_cast<int64_t>(last_price * FIXED_POINT_SCALE));
        if (realized != 0) {
            pos.realized_pnl_x8.fetch_add(static_cast<int64_t>(realized * FIXED_POINT_SCALE));
        }
        sequence.fetch_add(1);
    }

    // === Ultra-low latency path (relaxed memory ordering) ===
    // For single-writer scenarios where dashboard doesn't need strict consistency
    // Price update: ~1 cycle instead of ~15 cycles

    void update_last_price_relaxed(size_t symbol_id, int64_t price_x8) {
        if (symbol_id < MAX_PORTFOLIO_SYMBOLS) {
            positions[symbol_id].last_price_x8.store(price_x8, std::memory_order_relaxed);
        }
    }

    void update_position_relaxed(size_t symbol_id, int64_t qty_x8, int64_t avg_price_x8, int64_t last_price_x8) {
        if (symbol_id >= MAX_PORTFOLIO_SYMBOLS)
            return;
        auto& pos = positions[symbol_id];
        pos.quantity_x8.store(qty_x8, std::memory_order_relaxed);
        pos.avg_price_x8.store(avg_price_x8, std::memory_order_relaxed);
        pos.last_price_x8.store(last_price_x8, std::memory_order_relaxed);
        // Use release on sequence so reader with acquire sees all prior stores
        sequence.store(sequence.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    // Readers should use acquire ordering
    uint32_t get_sequence_acquire() const { return sequence.load(std::memory_order_acquire); }

    // Initialize slot with symbol name (call once at startup)
    void init_slot(size_t symbol_id, const char* symbol) {
        if (symbol_id >= MAX_PORTFOLIO_SYMBOLS)
            return;
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
        if (pos)
            pos->buy_count.fetch_add(1);
    }

    void record_sell(const char* symbol) {
        PositionSlot* pos = get_or_create_position(symbol);
        if (pos)
            pos->sell_count.fetch_add(1);
    }

    // === Initialization ===
    void init(double starting_cash) {
        magic = MAGIC;
        version = VERSION;

        // Generate random session ID
        std::random_device rd;
        session_id = rd();

        sequence.store(0);
        cash_x8.store(static_cast<int64_t>(starting_cash * FIXED_POINT_SCALE));
        initial_cash_x8.store(static_cast<int64_t>(starting_cash * FIXED_POINT_SCALE));
        total_realized_pnl_x8.store(0);
        total_events.store(0);
        winning_trades.store(0);
        losing_trades.store(0);
        total_fills.store(0);
        total_targets.store(0);
        total_stops.store(0);
        start_time_ns.store(std::chrono::steady_clock::now().time_since_epoch().count());
        trading_active.store(1);
        total_slippage_x8.store(0);
        total_commissions_x8.store(0);
        total_spread_cost_x8.store(0);
        total_volume_x8.store(0);

        for (size_t i = 0; i < MAX_PORTFOLIO_SYMBOLS; ++i) {
            positions[i].clear();
        }
    }

    bool is_valid() const { return magic == MAGIC && version == VERSION; }

    // === Shared Memory Factory ===
    static SharedPortfolioState* create(const char* name, double starting_cash = 100000.0) {
        int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
        if (fd < 0)
            return nullptr;

        if (ftruncate(fd, sizeof(SharedPortfolioState)) < 0) {
            close(fd);
            return nullptr;
        }

        void* ptr = mmap(nullptr, sizeof(SharedPortfolioState), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);

        if (ptr == MAP_FAILED)
            return nullptr;

        auto* state = static_cast<SharedPortfolioState*>(ptr);
        state->init(starting_cash);
        return state;
    }

    // Open for reading only (dashboard/observer use)
    static SharedPortfolioState* open(const char* name) {
        int fd = shm_open(name, O_RDONLY, 0666);
        if (fd < 0)
            return nullptr;

        void* ptr = mmap(nullptr, sizeof(SharedPortfolioState), PROT_READ, MAP_SHARED, fd, 0);
        close(fd);

        if (ptr == MAP_FAILED)
            return nullptr;

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
        if (fd < 0)
            return nullptr;

        void* ptr = mmap(nullptr, sizeof(SharedPortfolioState), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);

        if (ptr == MAP_FAILED)
            return nullptr;

        auto* state = static_cast<SharedPortfolioState*>(ptr);
        if (!state->is_valid()) {
            munmap(ptr, sizeof(SharedPortfolioState));
            return nullptr;
        }
        return state;
    }

    static void destroy(const char* name) { shm_unlink(name); }
};

// PositionSlot size is calculated automatically by compiler.
// Use sizeof(PositionSlot) instead of hardcoded values.
// Alignment requirements for atomic operations and IPC compatibility:
static_assert(sizeof(PositionSlot) % 8 == 0, "PositionSlot size must be 8-byte aligned for atomic ops");
static_assert(alignof(PositionSlot) >= 8, "PositionSlot must have at least 8-byte alignment");

// Current size for documentation (will update automatically if struct changes):
// sizeof(PositionSlot) = sizeof(symbol) + 4*sizeof(atomic<int64_t>) + 2*sizeof(atomic<uint32_t>)
//                      + 2*sizeof(atomic<uint8_t>) + sizeof(PositionSnapshot) + padding
constexpr size_t POSITION_SLOT_SIZE = sizeof(PositionSlot);

} // namespace ipc
} // namespace hft
