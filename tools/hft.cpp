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
#include "../include/symbol_config.hpp"
#include "../include/risk/enhanced_risk_manager.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
#include <map>
#include <vector>
#include <mutex>
#include <random>
#include <sstream>
#include <algorithm>

using namespace hft;
using namespace hft::exchange;
using namespace hft::strategy;

// ============================================================================
// Global State
// ============================================================================

std::atomic<bool> g_running{true};

void signal_handler(int) {
    std::cout << "\n\n[SHUTDOWN] Stopping...\n";
    g_running = false;
}

// ============================================================================
// CLI Arguments
// ============================================================================

struct CLIArgs {
    bool paper_mode = false;
    bool help = false;
    bool verbose = false;
    std::vector<std::string> symbols;
    int duration = 0;  // 0 = unlimited
    double capital = 100000.0;
    int max_position = 10;
};

void print_help() {
    std::cout << R"(
HFT Trading System
==================

Usage: hft [options]

Modes:
  (default)              Production mode - REAL orders
  --paper, -p            Paper trading mode - simulated fills

Options:
  -s, --symbols SYMS     Symbols (comma-separated, default: all USDT pairs)
  -d, --duration SECS    Duration in seconds (0 = unlimited)
  -c, --capital USD      Initial capital (default: 100000)
  -m, --max-pos N        Max position per symbol (default: 10)
  -v, --verbose          Verbose output
  -h, --help             Show this help

Examples:
  hft                              # Production, all symbols
  hft --paper                      # Paper trading, all symbols
  hft -s BTCUSDT                   # Production, single symbol
  hft --paper -s BTCUSDT,ETHUSDT   # Paper, two symbols
  hft --paper -d 300 -c 50000      # Paper, 5 min, $50k capital

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
    MarketRegime current_regime = MarketRegime::Unknown;
    Price last_mid = 0;
    uint64_t last_signal_time = 0;
    std::string ticker;  // for logging
};

// Portfolio: tracks cash and holdings (no leverage, no shorting)
struct Portfolio {
    double cash = 0;
    std::map<Symbol, double> holdings;  // symbol -> quantity held

    void init(double capital) {
        cash = capital;
        holdings.clear();
    }

    double get_holding(Symbol s) const {
        auto it = holdings.find(s);
        return it != holdings.end() ? it->second : 0;
    }

    bool can_buy(double price, double qty) const {
        return cash >= price * qty;
    }

    bool can_sell(Symbol s, double qty) const {
        return get_holding(s) >= qty;
    }

    void buy(Symbol s, double price, double qty) {
        cash -= price * qty;
        holdings[s] += qty;
    }

    void sell(Symbol s, double price, double qty) {
        cash += price * qty;
        holdings[s] -= qty;
        if (holdings[s] <= 0) holdings.erase(s);
    }

    double total_value(const std::map<Symbol, double>& prices) const {
        double value = cash;
        for (auto& [s, qty] : holdings) {
            auto it = prices.find(s);
            if (it != prices.end()) {
                value += qty * it->second;
            }
        }
        return value;
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
    {
        portfolio_.init(args.capital);

        if constexpr (std::is_same_v<OrderSender, PaperOrderSender>) {
            sender_.set_fill_callback([this](Symbol s, OrderId id, Side side, Quantity q, Price p) {
                on_fill(s, id, side, q, p);
            });
        }
    }

    void add_symbol(const std::string& ticker) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (engine_.lookup_symbol(ticker).has_value()) return;

        SymbolConfig cfg;
        cfg.symbol = ticker;
        cfg.max_position = args_.max_position;
        cfg.max_loss = 1000 * risk::PRICE_SCALE;

        Symbol id = engine_.add_symbol(cfg);
        auto strat = std::make_unique<SymbolStrategy>();
        strat->ticker = ticker;
        strategies_[id] = std::move(strat);
    }

    void on_quote(const std::string& ticker, Price bid, Price ask) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto opt = engine_.lookup_symbol(ticker);
        if (!opt) return;

        Symbol id = *opt;
        auto* world = engine_.get_symbol_world(id);
        if (!world) return;

        total_ticks_++;

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

        // Update regime
        auto* strat = strategies_[id].get();
        if (!strat) return;

        double mid = (bid + ask) / 2.0 / risk::PRICE_SCALE;
        strat->regime.update(mid);

        MarketRegime new_regime = strat->regime.current_regime();
        if (new_regime != strat->current_regime && strat->current_regime != MarketRegime::Unknown) {
            // Regime changed - log it
            add_log(strat->ticker, strat->current_regime, new_regime);
        }
        strat->current_regime = new_regime;

        // Generate signals
        if (engine_.can_trade() && !world->is_halted()) {
            check_signal(id, world, strat, bid, ask);
        }
    }

    struct Stats {
        size_t symbols = 0;
        uint64_t ticks = 0;
        uint64_t orders = 0;
        uint64_t fills = 0;
        double cash = 0;
        double holdings_value = 0;
        double equity = 0;
        double pnl = 0;
        int positions = 0;  // symbols with holdings
        bool halted = false;
    };

    Stats get_stats() {
        std::lock_guard<std::mutex> lock(mutex_);

        Stats s;
        s.symbols = engine_.symbol_count();
        s.ticks = total_ticks_;
        s.halted = !engine_.can_trade();
        s.cash = portfolio_.cash;

        if constexpr (std::is_same_v<OrderSender, PaperOrderSender>) {
            s.orders = sender_.total_orders();
            s.fills = sender_.total_fills();
        }

        // Calculate holdings value at current prices
        std::map<Symbol, double> prices;
        engine_.for_each_symbol([&](const SymbolWorld& w) {
            Price mid = w.top().mid_price();
            if (mid > 0) {
                prices[w.id()] = static_cast<double>(mid) / risk::PRICE_SCALE;
            }
        });

        s.holdings_value = 0;
        for (auto& [sym, qty] : portfolio_.holdings) {
            auto it = prices.find(sym);
            if (it != prices.end()) {
                s.holdings_value += qty * it->second;
                s.positions++;
            }
        }

        s.equity = s.cash + s.holdings_value;
        s.pnl = s.equity - args_.capital;
        return s;
    }

    std::map<MarketRegime, int> get_regimes() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::map<MarketRegime, int> dist;
        for (auto& [id, strat] : strategies_) {
            dist[strat->current_regime]++;
        }
        return dist;
    }

    struct SymbolInfo {
        std::string ticker;
        MarketRegime regime;
        double holding;     // quantity held (0 = no position)
        double value;       // holding * price
        double mid_price;
    };

    std::vector<SymbolInfo> get_symbol_details() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<SymbolInfo> result;

        engine_.for_each_symbol([&](const SymbolWorld& w) {
            SymbolInfo info;
            info.ticker = w.ticker();
            info.holding = portfolio_.get_holding(w.id());

            Price mid = w.top().mid_price();
            info.mid_price = mid > 0 ? static_cast<double>(mid) / risk::PRICE_SCALE : 0;
            info.value = info.holding * info.mid_price;

            auto it = strategies_.find(w.id());
            info.regime = (it != strategies_.end()) ? it->second->current_regime : MarketRegime::Unknown;

            result.push_back(info);
        });

        // Sort by value descending (largest holdings first)
        std::sort(result.begin(), result.end(), [](const SymbolInfo& a, const SymbolInfo& b) {
            return a.value > b.value;
        });

        return result;
    }

    bool is_halted() const { return !engine_.can_trade(); }

    struct LogEntry {
        std::string ticker;
        MarketRegime from;
        MarketRegime to;
        uint64_t timestamp;
    };

    std::vector<LogEntry> get_recent_logs(size_t max_count = 5) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<LogEntry> result;
        size_t start = logs_.size() > max_count ? logs_.size() - max_count : 0;
        for (size_t i = start; i < logs_.size(); ++i) {
            result.push_back(logs_[i]);
        }
        return result;
    }

private:
    CLIArgs args_;
    OrderSender sender_;
    TradingEngine<OrderSender> engine_;
    std::map<Symbol, std::unique_ptr<SymbolStrategy>> strategies_;
    std::atomic<uint64_t> total_ticks_;
    std::mutex mutex_;
    std::vector<LogEntry> logs_;
    Portfolio portfolio_;

    void add_log(const std::string& ticker, MarketRegime from, MarketRegime to) {
        uint64_t now = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        logs_.push_back({ticker, from, to, now});
        // Keep only last 100 entries
        if (logs_.size() > 100) {
            logs_.erase(logs_.begin(), logs_.begin() + 50);
        }
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

    void check_signal(Symbol id, SymbolWorld* world, SymbolStrategy* strat, Price bid, Price ask) {
        uint64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
        if (now - strat->last_signal_time < 300'000'000) return;  // 300ms cooldown

        Price mid = (bid + ask) / 2;
        if (strat->last_mid == 0) { strat->last_mid = mid; return; }

        // CRITICAL: Cast to double BEFORE subtraction to avoid unsigned underflow
        double change = (static_cast<double>(mid) - static_cast<double>(strat->last_mid)) / strat->last_mid;

        strat->last_mid = mid;

        double ask_usd = static_cast<double>(ask) / risk::PRICE_SCALE;
        double holding = portfolio_.get_holding(id);

        bool buy = false, sell = false;

        // SPOT TRADING: No shorting allowed!
        // - Buy: need cash, adds to holding
        // - Sell: need holding, reduces position

        // Thresholds for tick-by-tick trading (very small moves)
        constexpr double BUY_THRESHOLD = -0.00005;   // -0.005% dip
        constexpr double SELL_THRESHOLD = 0.00005;   // +0.005% rally

        switch (strat->current_regime) {
            case MarketRegime::TrendingUp:
                // Trend following: buy dips
                if (change < BUY_THRESHOLD && holding < args_.max_position) buy = true;
                break;
            case MarketRegime::TrendingDown:
                // Sell holdings to reduce exposure
                if (holding > 0 && change > SELL_THRESHOLD) sell = true;
                break;
            case MarketRegime::Ranging:
            case MarketRegime::LowVolatility:
                // Mean reversion: buy dips, sell rallies
                if (change < BUY_THRESHOLD && holding < args_.max_position) buy = true;
                else if (change > SELL_THRESHOLD && holding > 0) sell = true;
                break;
            case MarketRegime::HighVolatility:
                // Reduce risk: sell if holding too much
                if (holding > 2 && change > SELL_THRESHOLD) sell = true;
                break;
            default:
                break;
        }

        // Check portfolio constraints (SPOT: no leverage, no shorting)
        if (buy && !portfolio_.can_buy(ask_usd, 1)) {
            buy = false;  // Not enough cash
        }
        if (sell) {
            if (!portfolio_.can_sell(id, 1)) {
                sell = false;  // No holdings to sell (this is normal, not an error)
            }
        }

        if (buy && world->can_trade(Side::Buy, 1)) {
            if (args_.verbose) {
                std::cout << "[SIGNAL] " << strat->ticker << " BUY @ $" << ask_usd
                          << " (change=" << (change * 100) << "%)\n";
            }
            engine_.send_order(id, Side::Buy, 1, true);
            strat->last_signal_time = now;
        } else if (sell && world->can_trade(Side::Sell, 1)) {
            if (args_.verbose) {
                std::cout << "[SIGNAL] " << strat->ticker << " SELL (change=" << (change * 100) << "%)\n";
            }
            engine_.send_order(id, Side::Sell, 1, true);
            strat->last_signal_time = now;
        }
    }
};

// ============================================================================
// Display
// ============================================================================

void clear_screen() { std::cout << "\033[2J\033[H"; }

const char* regime_str(MarketRegime r) {
    switch (r) {
        case MarketRegime::TrendingUp: return "TREND-UP";
        case MarketRegime::TrendingDown: return "TREND-DN";
        case MarketRegime::Ranging: return "RANGING ";
        case MarketRegime::HighVolatility: return "HIGH-VOL";
        case MarketRegime::LowVolatility: return "LOW-VOL ";
        default: return "UNKNOWN ";
    }
}

const char* regime_short(MarketRegime r) {
    switch (r) {
        case MarketRegime::TrendingUp: return "UP  ";
        case MarketRegime::TrendingDown: return "DOWN";
        case MarketRegime::Ranging: return "RANG";
        case MarketRegime::HighVolatility: return "HVOL";
        case MarketRegime::LowVolatility: return "LVOL";
        default: return "??? ";
    }
}

const char* strategy_for_regime(MarketRegime r) {
    switch (r) {
        case MarketRegime::TrendingUp: return "MOMENTUM (buy dips)";
        case MarketRegime::TrendingDown: return "MOMENTUM (sell rallies)";
        case MarketRegime::Ranging: return "MEAN-REVERT";
        case MarketRegime::HighVolatility: return "REDUCE-RISK";
        case MarketRegime::LowVolatility: return "MEAN-REVERT";
        default: return "WAIT";
    }
}

// Short strategy name for table display
const char* strategy_short(MarketRegime r) {
    switch (r) {
        case MarketRegime::TrendingUp: return "MOM-BUY";
        case MarketRegime::TrendingDown: return "MOM-SEL";
        case MarketRegime::Ranging: return "MR     ";
        case MarketRegime::HighVolatility: return "REDUCE ";
        case MarketRegime::LowVolatility: return "MR     ";
        default: return "WAIT   ";
    }
}

template<typename App>
void print_status(App& app, int elapsed, bool paper_mode, double capital) {
    auto stats = app.get_stats();
    auto symbols = app.get_symbol_details();

    clear_screen();
    std::cout << "HFT Trading System - " << (paper_mode ? "PAPER" : "PRODUCTION") << " MODE\n";
    std::cout << "================================================================\n\n";

    std::cout << "  Time: " << elapsed << "s  |  Symbols: " << stats.symbols
              << "  |  Ticks: " << stats.ticks << "  |  Fills: " << stats.fills
              << "  |  " << (stats.halted ? "HALTED" : "ACTIVE") << "\n\n";

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  Cash: $" << stats.cash
              << "  |  Holdings: $" << stats.holdings_value
              << "  |  Equity: $" << stats.equity << "\n";
    double pnl_pct = capital > 0 ? (stats.pnl / capital * 100) : 0;
    std::cout << "  P&L: $" << (stats.pnl >= 0 ? "+" : "") << stats.pnl
              << "  (" << (pnl_pct >= 0 ? "+" : "") << pnl_pct << "%)"
              << "  |  Positions: " << stats.positions << " symbols\n\n";

    // Regime distribution summary
    std::map<MarketRegime, int> regime_counts;
    for (const auto& s : symbols) {
        regime_counts[s.regime]++;
    }
    std::cout << "  Regimes: ";
    for (auto& [r, c] : regime_counts) {
        if (r != MarketRegime::Unknown) {
            std::cout << regime_short(r) << ":" << c << "  ";
        }
    }
    std::cout << "\n\n";

    // Symbol details table with individual strategy (top 15 by value)
    std::cout << "  SYMBOL      REGIME  STRATEGY   QTY      PRICE        VALUE\n";
    std::cout << "  ---------------------------------------------------------------\n";

    size_t show_count = std::min(symbols.size(), size_t(15));
    for (size_t i = 0; i < show_count; ++i) {
        const auto& s = symbols[i];
        if (s.holding > 0 || i < 5) {  // Show holdings or at least top 5
            std::cout << "  " << std::left << std::setw(10) << s.ticker
                      << "  " << regime_short(s.regime)
                      << "  " << strategy_short(s.regime)
                      << "  " << std::right << std::setw(5) << std::fixed << std::setprecision(2) << s.holding
                      << "  $" << std::setw(9) << s.mid_price
                      << "  $" << std::setw(9) << s.value
                      << "\n";
        }
    }

    if (symbols.size() > 15) {
        std::cout << "  ... and " << (symbols.size() - 15) << " more symbols\n";
    }

    // Recent regime changes
    auto logs = app.get_recent_logs(5);
    if (!logs.empty()) {
        std::cout << "\n  Recent Regime Changes:\n";
        for (const auto& log : logs) {
            std::cout << "    " << std::left << std::setw(10) << log.ticker
                      << ": " << regime_short(log.from) << " -> " << regime_short(log.to) << "\n";
        }
    }

    std::cout << "\n================================================================\n";
    std::cout << "  Press Ctrl+C to stop\n";
}

// ============================================================================
// Main
// ============================================================================

template<typename OrderSender>
int run(const CLIArgs& args) {
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

    auto start = std::chrono::steady_clock::now();

    while (g_running) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();

        if (args.duration > 0 && elapsed >= args.duration) break;

        print_status(app, static_cast<int>(elapsed), args.paper_mode, args.capital);

        if (app.is_halted()) {
            std::cout << "\n  TRADING HALTED - Risk limit breached\n";
            break;
        }

        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    ws.disconnect();

    // Summary
    auto stats = app.get_stats();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();

    clear_screen();
    std::cout << "\nFINAL SUMMARY\n";
    std::cout << "================================================================\n";
    std::cout << "  Mode:      " << (args.paper_mode ? "PAPER" : "PRODUCTION") << "\n";
    std::cout << "  Duration:  " << elapsed << " seconds\n";
    std::cout << "  Symbols:   " << stats.symbols << "\n";
    std::cout << "  Ticks:     " << stats.ticks << "\n";
    std::cout << "  Fills:     " << stats.fills << "\n";
    std::cout << "  Capital:   $" << std::fixed << std::setprecision(2) << args.capital << "\n";
    std::cout << "  Equity:    $" << stats.equity << "\n";
    std::cout << "  P&L:       $" << (stats.pnl >= 0 ? "+" : "") << stats.pnl << "\n";
    std::cout << "  Status:    " << (stats.halted ? "HALTED" : "OK") << "\n";
    std::cout << "================================================================\n\n";

    return 0;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);

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
