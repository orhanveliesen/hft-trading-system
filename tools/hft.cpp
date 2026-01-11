/**
 * HFT - Unified Trading Application
 *
 * Single entry point for all HFT trading operations.
 * Default: Production mode (real orders)
 * Use --paper for paper trading with simulated fills.
 *
 * Usage:
 *   hft                              # Production mode, all symbols
 *   hft --paper                      # Paper trading mode
 *   hft -s BTCUSDT                   # Single symbol
 *   hft -s BTCUSDT,ETHUSDT           # Multiple symbols
 *   hft --paper -d 300               # Paper trade for 5 minutes
 *   hft -h                           # Help
 */

#include "../include/exchange/binance_ws.hpp"
#include "../include/trading_engine.hpp"
#include "../include/strategy/regime_detector.hpp"
#include "../include/strategy/technical_indicators.hpp"
#include "../include/symbol_config.hpp"
#include "../include/risk/enhanced_risk_manager.hpp"
#include "../include/ipc/trade_event.hpp"
#include "../include/ipc/shared_ring_buffer.hpp"
#include "../include/ipc/shared_portfolio_state.hpp"
#include "../include/ipc/shared_config.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
#include <array>
#include <map>
#include <vector>
#include <mutex>
#include <random>
#include <sstream>
#include <algorithm>
#include <sched.h>   // For CPU affinity
#include <cstring>   // For strncpy

using namespace hft;
using namespace hft::exchange;
using namespace hft::strategy;

// ============================================================================
// Pre-allocation Constants (HFT: no new/delete on hot path)
// ============================================================================

constexpr size_t MAX_SYMBOLS = 64;              // Max symbols we can track
constexpr size_t MAX_POSITIONS_PER_SYMBOL = 32; // Max open positions per symbol

// ============================================================================
// Global State
// ============================================================================

std::atomic<bool> g_running{true};
ipc::SharedConfig* g_shared_config = nullptr;  // For graceful shutdown signaling

void signal_handler(int sig) {
    // Mark as shutting down in shared memory (dashboard can see this immediately)
    if (g_shared_config) {
        g_shared_config->set_hft_status(3);  // shutting_down
        g_shared_config->update_heartbeat();
    }
    std::cout << "\n\n[SHUTDOWN] Received signal " << sig << ", stopping gracefully...\n";
    g_running = false;
}

// ============================================================================
// Event Publisher (lock-free IPC to observer)
// ============================================================================

using namespace hft::ipc;

/**
 * EventPublisher - Publishes trading events to shared memory
 *
 * Used by HFT engine to send events to observer process.
 * Lock-free, ~5ns per publish, no allocation.
 */
class EventPublisher {
public:
    explicit EventPublisher(bool enabled = true) : enabled_(enabled) {
        if (enabled_) {
            try {
                buffer_ = std::make_unique<SharedRingBuffer<TradeEvent>>("/hft_events", true);
                std::cout << "[IPC] Event publisher initialized (buffer: "
                          << buffer_->capacity() << " events)\n";
            } catch (const std::exception& e) {
                std::cerr << "[IPC] Warning: Could not create shared memory: " << e.what() << "\n";
                enabled_ = false;
            }
        }
    }

    // Publish fill event
    void fill(uint32_t sym, const char* ticker, uint8_t side, double price, double qty, uint32_t oid) {
        if (!enabled_) return;
        auto ts = now_ns();
        buffer_->push(TradeEvent::fill(seq_++, ts, sym, ticker, side, price, qty, oid));
    }

    // Publish target hit event
    void target_hit(uint32_t sym, const char* ticker, double entry, double exit, double qty) {
        if (!enabled_) return;
        auto ts = now_ns();
        int64_t pnl_cents = static_cast<int64_t>((exit - entry) * qty * 100);
        buffer_->push(TradeEvent::target_hit(seq_++, ts, sym, ticker, entry, exit, qty, pnl_cents));
    }

    // Publish stop loss event
    void stop_loss(uint32_t sym, const char* ticker, double entry, double exit, double qty) {
        if (!enabled_) return;
        auto ts = now_ns();
        int64_t pnl_cents = static_cast<int64_t>((exit - entry) * qty * 100);  // Negative for loss
        buffer_->push(TradeEvent::stop_loss(seq_++, ts, sym, ticker, entry, exit, qty, pnl_cents));
    }

    // Publish signal event
    void signal(uint32_t sym, const char* ticker, uint8_t side, uint8_t strength, double price) {
        if (!enabled_) return;
        auto ts = now_ns();
        buffer_->push(TradeEvent::signal(seq_++, ts, sym, ticker, side, strength, price));
    }

    // Publish regime change event
    void regime_change(uint32_t sym, const char* ticker, uint8_t new_regime) {
        if (!enabled_) return;
        auto ts = now_ns();
        buffer_->push(TradeEvent::regime_change(seq_++, ts, sym, ticker, new_regime));
    }

    bool enabled() const { return enabled_; }
    uint32_t sequence() const { return seq_; }

private:
    bool enabled_ = false;
    std::unique_ptr<SharedRingBuffer<TradeEvent>> buffer_;
    std::atomic<uint32_t> seq_{0};

    static uint64_t now_ns() {
        return std::chrono::steady_clock::now().time_since_epoch().count();
    }
};

// ============================================================================
// CLI Arguments
// ============================================================================

struct CLIArgs {
    bool paper_mode = false;
    bool help = false;
    bool verbose = false;
    int cpu_affinity = -1;    // CPU core to pin to (-1 = no pinning)
    std::vector<std::string> symbols;
    int duration = 0;  // 0 = unlimited
    double capital = 100000.0;
    int max_position = 10;
};

void print_help() {
    std::cout << R"(
HFT Trading System (Lock-Free)
==============================

Usage: hft [options]

Modes:
  (default)              Production mode - REAL orders
  --paper, -p            Paper trading mode - simulated fills

Options:
  -s, --symbols SYMS     Symbols (comma-separated, default: all USDT pairs)
  -d, --duration SECS    Duration in seconds (0 = unlimited)
  -c, --capital USD      Initial capital (default: 100000)
  -m, --max-pos N        Max position per symbol (default: 10)
  --cpu N                Pin to CPU core N (reduces latency)
  -v, --verbose          Verbose output (fills, targets, stops)
  -h, --help             Show this help

Examples:
  hft --paper                      # Paper trading, all symbols
  hft --paper -s BTCUSDT,ETHUSDT   # Paper, two symbols
  hft --paper -d 300 --cpu 2       # Paper, 5 min, pinned to CPU 2

Monitoring:
  Use hft_observer for real-time dashboard (separate process, lock-free IPC)

WARNING: Without --paper flag, REAL orders will be sent!
)";
}

std::vector<std::string> split_symbols(const std::string& s) {
    std::vector<std::string> result;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        // Trim and uppercase
        item.erase(0, item.find_first_not_of(" \t"));
        item.erase(item.find_last_not_of(" \t") + 1);
        std::transform(item.begin(), item.end(), item.begin(), ::toupper);
        if (!item.empty()) {
            result.push_back(item);
        }
    }
    return result;
}

bool parse_args(int argc, char* argv[], CLIArgs& args) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--paper" || arg == "-p") {
            args.paper_mode = true;
        }
        else if (arg == "--help" || arg == "-h") {
            args.help = true;
        }
        else if (arg == "--verbose" || arg == "-v") {
            args.verbose = true;
        }
        else if ((arg == "--symbols" || arg == "-s") && i + 1 < argc) {
            args.symbols = split_symbols(argv[++i]);
        }
        else if ((arg == "--duration" || arg == "-d") && i + 1 < argc) {
            args.duration = std::stoi(argv[++i]);
        }
        else if ((arg == "--capital" || arg == "-c") && i + 1 < argc) {
            args.capital = std::stod(argv[++i]);
        }
        else if ((arg == "--max-pos" || arg == "-m") && i + 1 < argc) {
            args.max_position = std::stoi(argv[++i]);
        }
        else if (arg == "--cpu" && i + 1 < argc) {
            args.cpu_affinity = std::stoi(argv[++i]);
        }
        else {
            std::cerr << "Unknown option: " << arg << "\n";
            std::cerr << "Use --help for usage information.\n";
            return false;
        }
    }
    return true;
}

std::vector<std::string> get_default_symbols() {
    return {
        "BTCUSDT", "ETHUSDT", "BNBUSDT", "XRPUSDT", "SOLUSDT",
        "ADAUSDT", "DOGEUSDT", "TRXUSDT", "DOTUSDT", "MATICUSDT",
        "LINKUSDT", "UNIUSDT", "AVAXUSDT", "ATOMUSDT", "LTCUSDT",
        "ETCUSDT", "XLMUSDT", "NEARUSDT", "APTUSDT", "FILUSDT",
        "ARBUSDT", "OPUSDT", "INJUSDT", "SUIUSDT", "SEIUSDT",
        "TIAUSDT", "JUPUSDT", "STXUSDT", "AAVEUSDT", "MKRUSDT"
    };
}

// ============================================================================
// Order Senders
// ============================================================================

/**
 * PaperOrderSender - Simulates exchange for paper trading
 *
 * Generates fake exchange signals for all order events.
 * Pessimistic fills: Buy at ask, Sell at bid.
 */
class PaperOrderSender {
public:
    static constexpr OrderId PAPER_ID_MASK = 0x8000000000000000ULL;

    enum class Event { Accepted, Filled, Cancelled, Rejected };

    using FillCallback = std::function<void(Symbol, OrderId, Side, Quantity, Price)>;

    PaperOrderSender() : next_id_(1), total_orders_(0), total_fills_(0) {}

    bool send_order(Symbol symbol, Side side, Quantity qty, bool /*is_market*/) {
        OrderId id = PAPER_ID_MASK | next_id_++;
        total_orders_++;
        pending_.push_back({symbol, id, side, qty});
        return true;
    }

    bool cancel_order(Symbol /*symbol*/, OrderId id) {
        auto it = std::find_if(pending_.begin(), pending_.end(),
            [id](const Order& o) { return o.id == id; });
        if (it != pending_.end()) {
            pending_.erase(it);
            return true;
        }
        return false;
    }

    void process_fills(Symbol symbol, Price bid, Price ask) {
        std::vector<Order> remaining;
        for (auto& o : pending_) {
            if (o.symbol != symbol) {
                remaining.push_back(o);
                continue;
            }
            Price fill_price = (o.side == Side::Buy) ? ask : bid;
            if (on_fill_) on_fill_(o.symbol, o.id, o.side, o.qty, fill_price);
            total_fills_++;
        }
        pending_ = std::move(remaining);
    }

    void set_fill_callback(FillCallback cb) { on_fill_ = std::move(cb); }
    uint64_t total_orders() const { return total_orders_; }
    uint64_t total_fills() const { return total_fills_; }

private:
    struct Order { Symbol symbol; OrderId id; Side side; Quantity qty; };
    OrderId next_id_;
    uint64_t total_orders_;
    uint64_t total_fills_;
    std::vector<Order> pending_;
    FillCallback on_fill_;
};

/**
 * ProductionOrderSender - Real order sender for Binance
 *
 * TODO: Implement actual order submission via REST API
 */
class ProductionOrderSender {
public:
    ProductionOrderSender() : total_orders_(0) {}

    bool send_order(Symbol /*symbol*/, Side /*side*/, Quantity /*qty*/, bool /*is_market*/) {
        // TODO: Implement real order submission
        // - Sign request with API key/secret
        // - Send via REST API
        // - Handle response
        total_orders_++;
        std::cerr << "[PRODUCTION] Order would be sent here\n";
        return false;  // Not implemented yet
    }

    bool cancel_order(Symbol /*symbol*/, OrderId /*id*/) {
        // TODO: Implement real cancel
        return false;
    }

    uint64_t total_orders() const { return total_orders_; }

private:
    uint64_t total_orders_;
};

// Verify concepts
static_assert(is_order_sender_v<PaperOrderSender>, "PaperOrderSender must satisfy OrderSender");
static_assert(is_order_sender_v<ProductionOrderSender>, "ProductionOrderSender must satisfy OrderSender");

// ============================================================================
// Strategy State
// ============================================================================

struct SymbolStrategy {
    RegimeDetector regime{RegimeConfig{}};
    TechnicalIndicators indicators{TechnicalIndicators::Config{}};
    MarketRegime current_regime = MarketRegime::Unknown;
    Price last_mid = 0;
    uint64_t last_signal_time = 0;
    char ticker[16] = {0};  // Fixed size, no std::string allocation
    bool active = false;    // Is this slot in use?

    // Dynamic spread tracking (EMA of spread)
    double ema_spread_pct = 0.001;  // Start with 0.1% default
    static constexpr double SPREAD_ALPHA = 0.1;  // EMA decay

    void init(const std::string& symbol) {
        active = true;
        std::strncpy(ticker, symbol.c_str(), sizeof(ticker) - 1);
        ticker[sizeof(ticker) - 1] = '\0';
    }

    void update_spread(Price bid, Price ask) {
        if (bid > 0 && ask > bid) {
            double spread_pct = static_cast<double>(ask - bid) / static_cast<double>(bid);
            ema_spread_pct = SPREAD_ALPHA * spread_pct + (1.0 - SPREAD_ALPHA) * ema_spread_pct;
        }
    }

    // Threshold = 3x spread with 0.02% (2 bps) minimum floor
    // This ensures we only trade when expected profit > spread cost
    // Math: entry spread + exit spread = 2x spread, so need >2x to profit
    double buy_threshold() const {
        double threshold = ema_spread_pct * 3.0;
        return -std::max(threshold, 0.0002);  // At least -0.02%
    }
    double sell_threshold() const {
        double threshold = ema_spread_pct * 3.0;
        return std::max(threshold, 0.0002);   // At least +0.02%
    }
};

// OpenPosition: tracks a single buy with entry price and targets
// Pre-allocated slot - uses 'active' flag instead of dynamic allocation
struct OpenPosition {
    double entry_price = 0;      // What we paid
    double quantity = 0;         // How much we hold
    double target_price = 0;     // Sell limit price (entry + profit margin)
    double stop_loss_price = 0;  // Cut loss price (entry - max loss)
    uint64_t timestamp = 0;      // When we bought
    bool active = false;         // Is this slot in use?

    void clear() {
        entry_price = 0;
        quantity = 0;
        target_price = 0;
        stop_loss_price = 0;
        timestamp = 0;
        active = false;
    }
};

// SymbolPositions: pre-allocated position storage for one symbol
// No std::vector, no dynamic allocation
struct SymbolPositions {
    std::array<OpenPosition, MAX_POSITIONS_PER_SYMBOL> slots;
    size_t count = 0;  // Number of active positions

    // Add a new position - O(1)
    bool add(double entry, double qty, double target, double stop_loss) {
        if (count >= MAX_POSITIONS_PER_SYMBOL) return false;

        // Find first inactive slot
        for (auto& slot : slots) {
            if (!slot.active) {
                slot.entry_price = entry;
                slot.quantity = qty;
                slot.target_price = target;
                slot.stop_loss_price = stop_loss;
                slot.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
                slot.active = true;
                count++;
                return true;
            }
        }
        return false;
    }

    // Get total quantity held
    double total_quantity() const {
        double total = 0;
        for (const auto& slot : slots) {
            if (slot.active) total += slot.quantity;
        }
        return total;
    }

    // Get average entry price
    double avg_entry() const {
        double total_cost = 0, total_qty = 0;
        for (const auto& slot : slots) {
            if (slot.active) {
                total_cost += slot.entry_price * slot.quantity;
                total_qty += slot.quantity;
            }
        }
        return total_qty > 0 ? total_cost / total_qty : 0;
    }

    void clear_all() {
        for (auto& slot : slots) slot.clear();
        count = 0;
    }
};

// Portfolio: tracks cash and positions with pre-allocated storage
// No std::map, no std::vector, no new/delete
struct Portfolio {
    double cash = 0;
    std::array<SymbolPositions, MAX_SYMBOLS> positions;  // Pre-allocated!
    std::array<bool, MAX_SYMBOLS> symbol_active;         // Track which symbols have positions

    // Config
    double profit_margin_pct = 0.002;   // 0.2% profit target
    double stop_loss_pct = 0.01;        // 1% max loss

    void init(double capital) {
        cash = capital;
        for (size_t i = 0; i < MAX_SYMBOLS; i++) {
            positions[i].clear_all();
            symbol_active[i] = false;
        }
    }

    double get_holding(Symbol s) const {
        if (s >= MAX_SYMBOLS) return 0;
        return positions[s].total_quantity();
    }

    bool can_buy(double price, double qty) const {
        return cash >= price * qty;
    }

    bool can_sell(Symbol s, double qty) const {
        return get_holding(s) >= qty;
    }

    // Buy and create position with target/stop-loss - O(1) no allocation
    void buy(Symbol s, double price, double qty) {
        if (qty <= 0 || price <= 0) return;
        if (s >= MAX_SYMBOLS) return;

        double target = price * (1.0 + profit_margin_pct);
        double stop_loss = price * (1.0 - stop_loss_pct);

        if (positions[s].add(price, qty, target, stop_loss)) {
            cash -= price * qty;
            symbol_active[s] = true;
        }
    }

    // Sell specific quantity, FIFO order - O(n) where n = positions for symbol
    void sell(Symbol s, double price, double qty) {
        if (qty <= 0 || price <= 0) return;
        if (s >= MAX_SYMBOLS) return;

        double remaining = qty;
        auto& sym_pos = positions[s];

        for (auto& slot : sym_pos.slots) {
            if (!slot.active || remaining <= 0) continue;

            double sell_qty = std::min(remaining, slot.quantity);
            cash += price * sell_qty;
            slot.quantity -= sell_qty;
            remaining -= sell_qty;

            if (slot.quantity <= 0.0001) {
                slot.clear();
                sym_pos.count--;
            }
        }

        if (sym_pos.count == 0) {
            symbol_active[s] = false;
        }
    }

    // Get average entry price for a symbol
    double avg_entry_price(Symbol s) const {
        if (s >= MAX_SYMBOLS) return 0;
        return positions[s].avg_entry();
    }

    // Callback-based target/stop checking - NO allocation!
    // Returns number of positions closed
    template<typename OnTargetHit, typename OnStopHit>
    int check_and_close(Symbol s, double current_price, OnTargetHit on_target, OnStopHit on_stop) {
        if (s >= MAX_SYMBOLS) return 0;

        int closed = 0;
        auto& sym_pos = positions[s];

        for (auto& slot : sym_pos.slots) {
            if (!slot.active) continue;

            // TARGET HIT: price went UP to our target
            if (current_price >= slot.target_price) {
                double qty = slot.quantity;
                cash += current_price * qty;
                on_target(qty, slot.entry_price, current_price);
                slot.clear();
                sym_pos.count--;
                closed++;
                continue;
            }

            // STOP-LOSS HIT: price went DOWN to our stop
            if (current_price <= slot.stop_loss_price) {
                double qty = slot.quantity;
                cash += current_price * qty;
                on_stop(qty, slot.entry_price, current_price);
                slot.clear();
                sym_pos.count--;
                closed++;
            }
        }

        if (sym_pos.count == 0) {
            symbol_active[s] = false;
        }

        return closed;
    }

    // Calculate total portfolio value (cash + holdings at current prices)
    // prices array: prices[symbol_id] = current price
    double total_value(const std::array<double, MAX_SYMBOLS>& prices) const {
        double value = cash;
        for (size_t s = 0; s < MAX_SYMBOLS; s++) {
            if (symbol_active[s] && prices[s] > 0) {
                value += positions[s].total_quantity() * prices[s];
            }
        }
        return value;
    }

    // Overload for std::map (backwards compatibility, slightly slower)
    double total_value(const std::map<Symbol, double>& prices) const {
        double value = cash;
        for (size_t s = 0; s < MAX_SYMBOLS; s++) {
            if (symbol_active[s]) {
                auto it = prices.find(static_cast<Symbol>(s));
                if (it != prices.end()) {
                    value += positions[s].total_quantity() * it->second;
                }
            }
        }
        return value;
    }

    int position_count() const {
        int count = 0;
        for (size_t s = 0; s < MAX_SYMBOLS; s++) {
            if (symbol_active[s] && positions[s].count > 0) count++;
        }
        return count;
    }

    int total_position_slots() const {
        int count = 0;
        for (size_t s = 0; s < MAX_SYMBOLS; s++) {
            count += positions[s].count;
        }
        return count;
    }
};

// ============================================================================
// Trading Application
// ============================================================================

template<typename OrderSender>
class TradingApp {
public:
    explicit TradingApp(const CLIArgs& args)
        : args_(args)
        , sender_()
        , engine_(sender_)
        , total_ticks_(0)
        , publisher_(args.paper_mode)  // Only publish events in paper mode
    {
        portfolio_.init(args.capital);

        // Initialize shared portfolio state for dashboard/observer
        if (args.paper_mode) {
            // Try to open existing state first (crash recovery) - read-write mode
            portfolio_state_ = SharedPortfolioState::open_rw("/hft_portfolio");
            if (portfolio_state_) {
                std::cout << "[IPC] Recovered existing portfolio state "
                          << "(cash=$" << portfolio_state_->cash()
                          << ", fills=" << portfolio_state_->total_fills.load() << ")\n";
                // Sync local portfolio with shared state
                portfolio_.cash = portfolio_state_->cash();
                portfolio_state_->trading_active.store(1);
            } else {
                // Create new state
                portfolio_state_ = SharedPortfolioState::create("/hft_portfolio", args.capital);
                if (portfolio_state_) {
                    std::cout << "[IPC] Portfolio state initialized "
                              << "(session=" << std::hex << std::uppercase << portfolio_state_->session_id
                              << std::dec << ", cash=$" << args.capital << ")\n";
                }
            }

            // Open shared config (dashboard can modify this)
            // Try to open existing, if version mismatch destroy and recreate
            shared_config_ = SharedConfig::open_rw("/hft_config");
            if (!shared_config_) {
                // Either doesn't exist or version mismatch - destroy and create fresh
                SharedConfig::destroy("/hft_config");
                shared_config_ = SharedConfig::create("/hft_config");
            }
            if (shared_config_) {
                last_config_seq_ = shared_config_->sequence.load();
                std::cout << "[IPC] Config loaded (spread_mult="
                          << shared_config_->spread_multiplier() << "x)\n";

                // Register HFT lifecycle in shared config
                shared_config_->set_hft_pid(getpid());
                shared_config_->set_hft_status(1);  // starting
                shared_config_->update_heartbeat();
                g_shared_config = shared_config_;  // For signal handler
            }
        }

        if constexpr (std::is_same_v<OrderSender, PaperOrderSender>) {
            sender_.set_fill_callback([this](Symbol s, OrderId id, Side side, Quantity q, Price p) {
                on_fill(s, id, side, q, p);
            });
        }
    }

    ~TradingApp() {
        // Normal shutdown - cleanup shared memory
        if (portfolio_state_) {
            // Mark trading as inactive
            portfolio_state_->trading_active.store(0);

            // Print final summary before cleanup
            std::cout << "\n[CLEANUP] Final portfolio state:\n"
                      << "  Cash: $" << std::fixed << std::setprecision(2) << portfolio_state_->cash() << "\n"
                      << "  Realized P&L: $" << portfolio_state_->total_realized_pnl() << "\n"
                      << "  Fills: " << portfolio_state_->total_fills.load()
                      << ", Targets: " << portfolio_state_->total_targets.load()
                      << ", Stops: " << portfolio_state_->total_stops.load() << "\n"
                      << "  Win rate: " << std::setprecision(1) << portfolio_state_->win_rate() << "%\n";

            // Unmap and unlink shared memory
            munmap(portfolio_state_, sizeof(SharedPortfolioState));
            SharedPortfolioState::destroy("/hft_portfolio");
            std::cout << "[IPC] Portfolio state cleaned up\n";
        }

        // Cleanup shared config (mark as stopped before unmapping)
        if (shared_config_) {
            shared_config_->set_hft_status(0);  // stopped
            shared_config_->update_heartbeat();
            g_shared_config = nullptr;
            munmap(shared_config_, sizeof(SharedConfig));
            std::cout << "[IPC] Config unmapped, HFT marked as stopped\n";
        }
    }

    void add_symbol(const std::string& ticker) {
        // Called during init only, before trading starts
        if (engine_.lookup_symbol(ticker).has_value()) return;

        SymbolConfig cfg;
        cfg.symbol = ticker;
        cfg.max_position = args_.max_position;
        cfg.max_loss = 1000 * risk::PRICE_SCALE;

        Symbol id = engine_.add_symbol(cfg);
        if (id < MAX_SYMBOLS) {
            strategies_[id].init(ticker);
        }
    }

    void on_quote(const std::string& ticker, Price bid, Price ask) {
        // Hot path - no locks, O(1) array access
        auto opt = engine_.lookup_symbol(ticker);
        if (!opt) return;

        Symbol id = *opt;
        if (id >= MAX_SYMBOLS) return;

        auto* world = engine_.get_symbol_world(id);
        if (!world) return;

        total_ticks_.fetch_add(1, std::memory_order_relaxed);

        // Update L1 - order: bid_price, bid_size, ask_price, ask_size
        L1Snapshot snap;
        snap.bid_price = bid;
        snap.bid_size = 100;
        snap.ask_price = ask;
        snap.ask_size = 100;
        world->apply_snapshot(snap);

        // Process paper fills
        if constexpr (std::is_same_v<OrderSender, PaperOrderSender>) {
            sender_.process_fills(id, bid, ask);
        }

        // Update regime and spread - O(1) array access
        auto& strat = strategies_[id];
        if (!strat.active) return;

        // Track spread for dynamic thresholds
        strat.update_spread(bid, ask);

        double mid = (bid + ask) / 2.0 / risk::PRICE_SCALE;

        // Update last price in shared state for dashboard charts
        // Ultra-low latency: relaxed memory ordering (~1 cycle vs ~15)
        if (portfolio_state_) {
            portfolio_state_->update_last_price_relaxed(
                static_cast<size_t>(id), static_cast<int64_t>(mid * 1e8));
        }
        strat.regime.update(mid);
        strat.indicators.update(mid);  // Update technical indicators

        MarketRegime new_regime = strat.regime.current_regime();
        if (new_regime != strat.current_regime) {
            // Regime changed - publish to observer
            if (strat.current_regime != MarketRegime::Unknown) {
                publisher_.regime_change(id, strat.ticker, static_cast<uint8_t>(new_regime));
            }
            strat.current_regime = new_regime;

            // Update shared state for dashboard (~5ns)
            if (portfolio_state_) {
                portfolio_state_->update_regime(strat.ticker, static_cast<uint8_t>(new_regime));
            }
        }

        // Generate buy signals
        if (engine_.can_trade() && !world->is_halted()) {
            check_signal(id, world, &strat, bid, ask);
        }

        // Check target/stop-loss for this symbol - O(n), no allocation
        if (portfolio_.symbol_active[id]) {
            double bid_usd = static_cast<double>(bid) / risk::PRICE_SCALE;
            const char* ticker = strat.ticker;

            portfolio_.check_and_close(id, bid_usd,
                // On target hit (profit)
                [&](double qty, double entry, double exit) {
                    double profit = (exit - entry) * qty;

                    // Update shared portfolio state (~5ns)
                    if (portfolio_state_) {
                        portfolio_state_->set_cash(portfolio_.cash);
                        portfolio_state_->add_realized_pnl(profit);
                        portfolio_state_->record_target();
                        portfolio_state_->record_event();
                        // Update position (will be 0 qty if fully closed)
                        auto& pos = portfolio_.positions[id];
                        portfolio_state_->update_position(ticker, pos.total_quantity(), pos.avg_entry(), exit);
                    }

                    // Track win streak
                    record_win();

                    // Publish to observer (~5ns)
                    publisher_.target_hit(id, ticker, entry, exit, qty);

                    if (args_.verbose) {
                        std::cout << "[TARGET] " << ticker << " SELL " << qty
                                  << " @ $" << std::fixed << std::setprecision(2) << exit
                                  << " (entry=$" << entry
                                  << ", profit=$" << std::setprecision(2) << profit << ")\n";
                    }
                },
                // On stop-loss hit (cut loss)
                [&](double qty, double entry, double exit) {
                    double loss = (exit - entry) * qty;  // Will be negative

                    // Update shared portfolio state (~5ns)
                    if (portfolio_state_) {
                        portfolio_state_->set_cash(portfolio_.cash);
                        portfolio_state_->add_realized_pnl(loss);
                        portfolio_state_->record_stop();
                        portfolio_state_->record_event();
                        // Update position (will be 0 qty if fully closed)
                        auto& pos = portfolio_.positions[id];
                        portfolio_state_->update_position(ticker, pos.total_quantity(), pos.avg_entry(), exit);
                    }

                    // Track loss streak
                    record_loss();

                    // Publish to observer (~5ns)
                    publisher_.stop_loss(id, ticker, entry, exit, qty);

                    if (args_.verbose) {
                        std::cout << "[STOP] " << ticker << " SELL " << qty
                                  << " @ $" << std::fixed << std::setprecision(2) << exit
                                  << " (entry=$" << entry
                                  << ", loss=$" << std::setprecision(2) << -loss << ")\n";
                    }
                }
            );
        }
    }

    // Stats for final summary (called after trading stops, not on hot path)
    struct Stats {
        size_t symbols = 0;
        uint64_t ticks = 0;
        uint64_t orders = 0;
        uint64_t fills = 0;
        double cash = 0;
        double holdings_value = 0;
        double equity = 0;
        double pnl = 0;
        int positions = 0;
        bool halted = false;
    };

    Stats get_stats() {
        // Called after trading stops - not performance critical
        Stats s;
        s.symbols = engine_.symbol_count();
        s.ticks = total_ticks_.load(std::memory_order_relaxed);
        s.halted = !engine_.can_trade();
        s.cash = portfolio_.cash;

        if constexpr (std::is_same_v<OrderSender, PaperOrderSender>) {
            s.orders = sender_.total_orders();
            s.fills = sender_.total_fills();
        }

        // Calculate holdings value using fixed array (no std::map)
        std::array<double, MAX_SYMBOLS> prices{};
        engine_.for_each_symbol([&](const SymbolWorld& w) {
            Price mid = w.top().mid_price();
            if (mid > 0 && w.id() < MAX_SYMBOLS) {
                prices[w.id()] = static_cast<double>(mid) / risk::PRICE_SCALE;
            }
        });

        s.holdings_value = 0;
        s.positions = 0;
        for (size_t sym = 0; sym < MAX_SYMBOLS; sym++) {
            if (!portfolio_.symbol_active[sym] || prices[sym] <= 0) continue;

            double sym_qty = portfolio_.positions[sym].total_quantity();
            if (sym_qty > 0) {
                s.holdings_value += sym_qty * prices[sym];
                s.positions++;
            }
        }

        s.equity = s.cash + s.holdings_value;
        s.pnl = s.equity - args_.capital;
        return s;
    }

    bool is_halted() const { return !engine_.can_trade(); }

private:
    CLIArgs args_;
    OrderSender sender_;
    TradingEngine<OrderSender> engine_;
    std::array<SymbolStrategy, MAX_SYMBOLS> strategies_;  // Fixed array, O(1) access
    std::atomic<uint64_t> total_ticks_;
    // No mutex - single-threaded hot path, lock-free design
    Portfolio portfolio_;
    EventPublisher publisher_;  // Lock-free event publishing to observer
    SharedPortfolioState* portfolio_state_ = nullptr;  // Shared state for dashboard
    SharedConfig* shared_config_ = nullptr;           // Shared config from dashboard
    uint32_t last_config_seq_ = 0;                    // Track config changes

    // Strategy mode tracking
    int32_t consecutive_wins_ = 0;
    int32_t consecutive_losses_ = 0;
    uint8_t active_mode_ = 2;  // NORMAL by default

    void update_active_mode() {
        if (!shared_config_) return;

        // Determine mode based on performance and config
        uint8_t force = shared_config_->get_force_mode();
        if (force > 0) {
            // Manual override
            active_mode_ = force;
        } else {
            // Auto mode - adjust based on performance
            int32_t loss_limit = shared_config_->loss_streak();
            if (consecutive_losses_ >= loss_limit) {
                active_mode_ = 3;  // CAUTIOUS
            } else if (consecutive_losses_ >= loss_limit + 2) {
                active_mode_ = 4;  // DEFENSIVE
            } else if (consecutive_wins_ >= 3) {
                active_mode_ = 1;  // AGGRESSIVE
            } else {
                active_mode_ = 2;  // NORMAL
            }
        }

        // Update shared config for dashboard
        shared_config_->set_active_mode(active_mode_);
        shared_config_->set_consecutive_wins(consecutive_wins_);
        shared_config_->set_consecutive_losses(consecutive_losses_);
    }

    void record_win() {
        consecutive_wins_++;
        consecutive_losses_ = 0;
        update_active_mode();
    }

    void record_loss() {
        consecutive_losses_++;
        consecutive_wins_ = 0;
        update_active_mode();
    }

    void on_fill(Symbol symbol, OrderId id, Side side, Quantity qty, Price price) {
        auto* world = engine_.get_symbol_world(symbol);
        if (!world) return;

        double price_usd = static_cast<double>(price) / risk::PRICE_SCALE;
        double qty_d = static_cast<double>(qty);

        // Update portfolio (spot trading: no leverage, no shorting)
        if (side == Side::Buy) {
            portfolio_.buy(symbol, price_usd, qty_d);
        } else {
            portfolio_.sell(symbol, price_usd, qty_d);
        }

        // Update shared portfolio state for dashboard (~5ns)
        if (portfolio_state_) {
            portfolio_state_->set_cash(portfolio_.cash);
            portfolio_state_->record_fill();
            portfolio_state_->record_event();

            auto& pos = portfolio_.positions[symbol];
            portfolio_state_->update_position(
                world->ticker().c_str(),
                pos.total_quantity(),
                pos.avg_entry(),
                price_usd
            );

            if (side == Side::Buy) {
                portfolio_state_->record_buy(world->ticker().c_str());
            } else {
                portfolio_state_->record_sell(world->ticker().c_str());
            }
        }

        // Publish fill event to observer (~5ns, lock-free)
        publisher_.fill(symbol, world->ticker().c_str(),
                       side == Side::Buy ? 0 : 1, price_usd, qty_d, id);

        // Debug: log fill details
        if (args_.verbose) {
            std::cout << "[FILL] " << world->ticker()
                      << " " << (side == Side::Buy ? "BUY" : "SELL")
                      << " " << qty << " @ $" << std::fixed << std::setprecision(2) << price_usd
                      << " (cash=$" << portfolio_.cash << ")\n";
        }

        world->on_fill(side, qty, price);
        world->on_our_fill(id, qty);
    }

    // Note: check_targets_and_stops removed - now handled inline in on_quote

    void check_signal(Symbol id, SymbolWorld* world, SymbolStrategy* strat, Price bid, Price ask) {
        uint64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
        if (now - strat->last_signal_time < 300'000'000) return;  // 300ms cooldown

        Price mid = (bid + ask) / 2;
        if (strat->last_mid == 0) { strat->last_mid = mid; return; }

        strat->last_mid = mid;

        // Wait for indicators to have enough data
        if (!strat->indicators.ready()) return;

        double ask_usd = static_cast<double>(ask) / risk::PRICE_SCALE;
        double bid_usd = static_cast<double>(bid) / risk::PRICE_SCALE;
        double holding = portfolio_.get_holding(id);

        // NEW LOGIC:
        // - BUY based on regime + indicators
        // - SELL handled by target/stop-loss (not here!)

        bool should_buy = false;

        auto buy_strength = strat->indicators.buy_signal();
        using SS = TechnicalIndicators::SignalStrength;

        switch (strat->current_regime) {
            case MarketRegime::TrendingUp:
                // Uptrend: Buy on medium signal, let target take profit
                if (buy_strength >= SS::Medium && holding < args_.max_position) {
                    should_buy = true;
                }
                break;

            case MarketRegime::TrendingDown:
                // Downtrend: DON'T BUY! Stop-loss will handle exits
                // Just wait for trend reversal
                break;

            case MarketRegime::Ranging:
            case MarketRegime::LowVolatility:
                // Mean reversion: Buy on dips (oversold signals)
                if (buy_strength >= SS::Medium && holding < args_.max_position) {
                    should_buy = true;
                }
                break;

            case MarketRegime::HighVolatility:
                // High vol: Be careful, only buy on very strong signals
                if (buy_strength >= SS::Strong && holding < args_.max_position) {
                    should_buy = true;
                }
                break;

            default:
                break;
        }

        // Price check: Only buy if price is attractive (below slow EMA)
        double ema = strat->indicators.ema_slow();
        if (should_buy && ema > 0) {
            double deviation = (ask_usd - ema) / ema;
            // Only buy if price is at or below EMA (deviation <= 0)
            // Or at most slightly above (small positive deviation OK in uptrend)
            double max_deviation = (strat->current_regime == MarketRegime::TrendingUp) ? 0.001 : 0.0;
            if (deviation > max_deviation) {
                should_buy = false;  // Price too high relative to EMA
            }
        }

        // Portfolio constraint
        if (should_buy && !portfolio_.can_buy(ask_usd, 1)) {
            should_buy = false;
        }

        auto signal_str = [](TechnicalIndicators::SignalStrength s) {
            switch (s) {
                case SS::Strong: return "STRONG";
                case SS::Medium: return "MEDIUM";
                case SS::Weak: return "WEAK";
                default: return "NONE";
            }
        };

        // Execute buy if conditions met
        if (should_buy && world->can_trade(Side::Buy, 1)) {
            if (args_.verbose) {
                std::cout << "[BUY] " << strat->ticker << " @ $" << std::fixed << std::setprecision(2) << ask_usd
                          << " (signal=" << signal_str(buy_strength)
                          << ", RSI=" << std::setprecision(0) << strat->indicators.rsi()
                          << ", target=$" << std::setprecision(2) << ask_usd * (1.0 + portfolio_.profit_margin_pct)
                          << ", stop=$" << ask_usd * (1.0 - portfolio_.stop_loss_pct) << ")\n";
            }
            engine_.send_order(id, Side::Buy, 1, true);
            strat->last_signal_time = now;
        }
        // NOTE: Selling is handled by check_targets_and_stops(), not here!
    }
};

// ============================================================================
// CPU Affinity
// ============================================================================

// Pin current thread to specific CPU core
bool set_cpu_affinity(int cpu) {
    if (cpu < 0) return true;  // No pinning requested

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);

    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) != 0) {
        std::cerr << "[WARN] Could not pin to CPU " << cpu << ": " << strerror(errno) << "\n";
        return false;
    }
    std::cout << "[CPU] Pinned to core " << cpu << "\n";
    return true;
}

// Note: Dashboard removed - use hft_observer for real-time monitoring
// This keeps HFT process lean with zero display overhead

// ============================================================================
// Main
// ============================================================================

template<typename OrderSender>
int run(const CLIArgs& args) {
    // Pin to CPU core if requested (reduces latency variance)
    set_cpu_affinity(args.cpu_affinity);

    std::cout << "\nHFT Trading System - " << (args.paper_mode ? "PAPER" : "PRODUCTION") << " MODE\n";
    std::cout << "================================================================\n\n";

    if (!args.paper_mode) {
        std::cout << "WARNING: Production mode - real orders will be sent!\n";
        std::cout << "Press Ctrl+C within 5 seconds to abort...\n\n";
        for (int i = 5; i > 0 && g_running; --i) {
            std::cout << "  " << i << "...\n";
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!g_running) return 0;
    }

    TradingApp<OrderSender> app(args);

    auto symbols = args.symbols.empty() ? get_default_symbols() : args.symbols;
    std::cout << "Registering " << symbols.size() << " symbols...\n";
    for (auto& s : symbols) app.add_symbol(s);

    BinanceWs ws(false);

    ws.set_connect_callback([](bool c) {
        if (c) std::cout << "[OK] Connected to Binance\n\n";
        else std::cout << "[DISCONNECTED] from Binance\n";
    });

    ws.set_error_callback([](const std::string& err) {
        std::cerr << "[WS ERROR] " << err << "\n";
    });

    ws.set_book_ticker_callback([&](const BookTicker& bt) {
        app.on_quote(bt.symbol, bt.bid_price, bt.ask_price);
    });

    for (auto& s : symbols) ws.subscribe_book_ticker(s);

    std::cout << "Connecting...\n";
    if (!ws.connect()) {
        std::cerr << "Connection failed\n";
        return 1;
    }

    for (int i = 0; i < 50 && !ws.is_connected() && g_running; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!ws.is_connected()) {
        std::cerr << "Connection timeout\n";
        return 1;
    }

    // Mark as running now that we're connected
    if (g_shared_config) {
        g_shared_config->set_hft_status(2);  // running
        g_shared_config->update_heartbeat();
    }

    auto start = std::chrono::steady_clock::now();
    auto last_heartbeat = start;

    while (g_running) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();

        if (args.duration > 0 && elapsed >= args.duration) break;

        if (app.is_halted()) {
            std::cout << "\n  TRADING HALTED - Risk limit breached\n";
            break;
        }

        // Update heartbeat every second
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_heartbeat).count() >= 1) {
            if (g_shared_config) {
                g_shared_config->update_heartbeat();
            }
            last_heartbeat = now;
        }

        // No dashboard here - use hft_observer for real-time monitoring
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ws.disconnect();

    // Final summary
    auto stats = app.get_stats();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();

    std::cout << "\n[DONE] " << elapsed << "s | "
              << stats.ticks << " ticks | "
              << stats.fills << " fills | P&L: $"
              << std::fixed << std::setprecision(2)
              << (stats.pnl >= 0 ? "+" : "") << stats.pnl << "\n";

    return 0;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    CLIArgs args;
    if (!parse_args(argc, argv, args)) return 1;

    if (args.help) {
        print_help();
        return 0;
    }

    if (args.paper_mode) {
        return run<PaperOrderSender>(args);
    } else {
        return run<ProductionOrderSender>(args);
    }
}
