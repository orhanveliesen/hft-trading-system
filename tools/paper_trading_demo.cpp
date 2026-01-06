/**
 * Paper Trading Demo
 *
 * Demonstrates the complete paper trading system with:
 * - Regime detection
 * - Adaptive strategy selection
 * - Order simulation with realistic fills
 * - Live dashboard display
 *
 * Usage:
 *   ./paper_trading_demo [--no-dashboard] [--fast] [--duration=SECONDS]
 */

#include "../include/paper/paper_trading_engine.hpp"
#include "../include/paper/live_dashboard.hpp"
#include "../include/strategy/adaptive_strategy.hpp"
#include <iostream>
#include <random>
#include <csignal>
#include <cstring>
#include <thread>
#include <chrono>

using namespace hft;
using namespace hft::paper;
using namespace hft::strategy;

// Global flag for graceful shutdown
volatile sig_atomic_t running = 1;

void signal_handler(int /*sig*/) {
    running = 0;
}

/**
 * Market Data Simulator
 *
 * Generates realistic price movements with:
 * - Trending periods
 * - Mean reversion
 * - Volatility clusters
 */
class MarketSimulator {
public:
    MarketSimulator(Price initial_mid, double tick_size = 0.01)
        : mid_price_(initial_mid)
        , tick_size_(tick_size)
        , spread_(100)  // 1 cent spread
        , volatility_(0.0002)
        , trend_(0)
        , regime_duration_(0)
        , rng_(std::random_device{}())
    {}

    struct Quote {
        Price bid;
        Price ask;
        uint64_t timestamp_ns;
    };

    Quote next() {
        // Update regime periodically
        if (regime_duration_ <= 0) {
            change_regime();
        }
        regime_duration_--;

        // Generate price movement
        std::normal_distribution<double> noise(0, volatility_);
        double move = noise(rng_) + trend_;

        // Apply move (in price units)
        int64_t price_move = static_cast<int64_t>(move * mid_price_);
        mid_price_ += price_move;

        // Ensure positive price
        if (mid_price_ < 10000) mid_price_ = 10000;  // Min $1.00

        // Calculate bid/ask
        Price half_spread = spread_ / 2;
        Quote quote;
        quote.bid = mid_price_ - half_spread;
        quote.ask = mid_price_ + half_spread;
        quote.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();

        return quote;
    }

    void set_volatility(double vol) { volatility_ = vol; }
    void set_trend(double trend) { trend_ = trend; }

private:
    Price mid_price_;
    double tick_size_;
    Price spread_;
    double volatility_;
    double trend_;
    int regime_duration_;
    std::mt19937_64 rng_;

    void change_regime() {
        std::uniform_int_distribution<int> regime_dist(0, 4);
        int regime = regime_dist(rng_);

        std::uniform_int_distribution<int> duration_dist(100, 500);
        regime_duration_ = duration_dist(rng_);

        switch (regime) {
            case 0:  // Trending up
                trend_ = 0.0001;
                volatility_ = 0.0002;
                break;
            case 1:  // Trending down
                trend_ = -0.0001;
                volatility_ = 0.0002;
                break;
            case 2:  // High volatility
                trend_ = 0;
                volatility_ = 0.0005;
                break;
            case 3:  // Low volatility (mean reversion)
                trend_ = 0;
                volatility_ = 0.0001;
                break;
            case 4:  // Ranging
                trend_ = 0;
                volatility_ = 0.00015;
                break;
        }
    }
};

/**
 * Simple Strategy Logic
 *
 * Uses regime to determine action:
 * - Trending Up: Go long
 * - Trending Down: Go short
 * - Ranging: Mean revert
 * - High Vol: Reduce position
 * - Low Vol: Build position slowly
 */
class PaperTradingStrategy {
public:
    explicit PaperTradingStrategy(PaperTradingEngine& engine, bool fast_mode = false)
        : engine_(engine)
        , last_signal_time_(0)
        , signal_cooldown_ns_(fast_mode ? 10'000'000 : 500'000'000)  // 10ms or 500ms
    {}

    void on_quote(Symbol symbol, Price bid, Price ask) {
        auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();

        // Check cooldown
        if (now - last_signal_time_ < signal_cooldown_ns_) {
            return;
        }

        MarketRegime regime = engine_.current_regime();
        const auto& pos = engine_.get_position(symbol);

        int target_position = calculate_target(regime, pos.quantity, bid, ask);
        int delta = target_position - pos.quantity;

        if (std::abs(delta) >= 10) {  // Minimum order size
            Side side = (delta > 0) ? Side::Buy : Side::Sell;
            Quantity qty = static_cast<Quantity>(std::abs(delta));

            if (engine_.submit_order(symbol, side, qty, true)) {
                last_signal_time_ = now;
            }
        }
    }

private:
    PaperTradingEngine& engine_;
    uint64_t last_signal_time_;
    uint64_t signal_cooldown_ns_;

    int calculate_target(MarketRegime regime, int64_t current_pos, Price bid, Price ask) {
        (void)bid;
        (void)ask;

        double confidence = engine_.regime_confidence();

        switch (regime) {
            case MarketRegime::TrendingUp:
                return static_cast<int>(100 * confidence);  // Long

            case MarketRegime::TrendingDown:
                return static_cast<int>(-100 * confidence);  // Short

            case MarketRegime::Ranging:
                // Mean revert towards zero
                return static_cast<int>(current_pos * -0.5);

            case MarketRegime::HighVolatility:
                // Reduce position
                return static_cast<int>(current_pos * 0.3);

            case MarketRegime::LowVolatility:
                // Small position in trend direction, or base position if no trend
                if (std::abs(engine_.trend_strength()) > 0.05) {
                    return static_cast<int>(engine_.trend_strength() * 100);
                }
                return 20;  // Default small long position in quiet markets

            default:
                return 0;  // Unknown regime: flat
        }
    }
};

int main(int argc, char* argv[]) {
    // Parse arguments
    bool use_dashboard = true;
    bool fast_mode = false;
    int duration_secs = 60;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--no-dashboard") == 0) {
            use_dashboard = false;
        } else if (std::strcmp(argv[i], "--fast") == 0) {
            fast_mode = true;
        } else if (std::strncmp(argv[i], "--duration=", 11) == 0) {
            duration_secs = std::atoi(argv[i] + 11);
        }
    }

    // Set up signal handler
    std::signal(SIGINT, signal_handler);

    // Initialize paper trading engine
    PaperTradingConfig config;
    config.initial_capital = 100000 * hft::strategy::PRICE_SCALE;  // $100k scaled
    config.default_max_position = 200;
    config.max_drawdown_pct = 0.05;  // 5% max drawdown
    config.daily_loss_limit = 5000 * hft::strategy::PRICE_SCALE;   // $5k daily loss limit
    config.fill_config.min_latency_ns = fast_mode ? 0 : 100'000;
    config.fill_config.max_latency_ns = fast_mode ? 0 : 500'000;
    config.fill_config.slippage_bps = 0.5;
    config.fill_config.enable_partial_fills = !fast_mode;
    config.enable_logging = false;  // Dashboard handles display

    PaperTradingEngine engine(config);

    // Initialize market simulator
    MarketSimulator market(1500000);  // Start at $150.00

    // Initialize strategy
    PaperTradingStrategy strategy(engine, fast_mode);

    // Initialize dashboard
    DashboardConfig dash_config;
    dash_config.refresh_interval_ms = 100;
    dash_config.use_colors = true;
    dash_config.clear_screen = use_dashboard;

    LiveDashboard dashboard(engine, dash_config);
    StatusLine status(engine);

    // Set symbol info
    Symbol symbol = 1;
    dashboard.set_symbol_info(symbol, "AAPL", 1500000, 1501000);

    std::cout << "\n=== Paper Trading Demo ===\n";
    std::cout << "Initial Capital: $" << config.initial_capital << "\n";
    std::cout << "Duration: " << duration_secs << " seconds\n";
    std::cout << "Mode: " << (fast_mode ? "Fast" : "Realistic") << "\n";
    std::cout << "Dashboard: " << (use_dashboard ? "Full" : "Status Line") << "\n";
    std::cout << "\nStarting in 3 seconds...\n";
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // Main loop
    auto start = std::chrono::steady_clock::now();
    uint64_t tick_count = 0;

    while (running) {
        // Check duration
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= duration_secs) {
            break;
        }

        // Generate market data
        auto quote = market.next();

        // Update dashboard symbol info
        dashboard.set_symbol_info(symbol, "AAPL", quote.bid, quote.ask);

        // Process market data
        engine.on_market_data(symbol, quote.bid, quote.ask, quote.timestamp_ns);

        // Run strategy
        strategy.on_quote(symbol, quote.bid, quote.ask);

        // Update display
        if (use_dashboard) {
            dashboard.update();
        } else {
            status.print();
        }

        tick_count++;

        // Throttle if not in fast mode
        if (!fast_mode) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // Final summary
    if (use_dashboard) {
        std::cout << "\033[2J\033[H";  // Clear screen
    }
    std::cout << "\n";

    std::cout << "\n=== Paper Trading Summary ===\n\n";
    std::cout << "Duration: " << std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count() << " seconds\n";
    std::cout << "Ticks Processed: " << tick_count << "\n";
    std::cout << "\n";

    std::cout << "--- Performance ---\n";
    std::cout << "Initial Capital: $" << config.initial_capital << "\n";
    std::cout << "Final Equity:    $" << std::fixed << std::setprecision(2) << engine.equity() << "\n";
    std::cout << "Total P&L:       $" << (engine.total_pnl() >= 0 ? "+" : "") << engine.total_pnl() << "\n";
    std::cout << "Return:          " << ((engine.equity() / config.initial_capital - 1) * 100) << "%\n";
    std::cout << "Max Drawdown:    " << (engine.drawdown() * 100) << "%\n";
    std::cout << "\n";

    std::cout << "--- Activity ---\n";
    std::cout << "Total Orders:    " << engine.total_orders() << "\n";
    std::cout << "Total Fills:     " << engine.total_fills() << "\n";
    std::cout << "\n";

    std::cout << "--- Final Position ---\n";
    const auto& pos = engine.get_position(symbol);
    std::cout << "AAPL: " << pos.quantity << " shares\n";
    std::cout << "Unrealized P&L: $" << pos.unrealized_pnl << "\n";
    std::cout << "Realized P&L:   $" << pos.realized_pnl << "\n";
    std::cout << "\n";

    if (engine.is_halted()) {
        std::cout << "*** TRADING WAS HALTED DUE TO RISK LIMITS ***\n";
    }

    std::cout << "=== Demo Complete ===\n\n";

    return 0;
}
