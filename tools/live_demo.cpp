/**
 * Live Trading Demo
 *
 * Loads strategy config and generates trading signals from real-time data.
 * This is a simulation - no real orders are placed.
 *
 * Usage:
 *   ./live_demo [config_file] [duration_seconds]
 *
 * Examples:
 *   ./live_demo trading_config.json 60
 *   ./live_demo                         # Uses default config, runs 30 seconds
 */

#include "../include/config/strategy_config.hpp"
#include "../include/config/strategy_factory.hpp"
#include "../include/backtest/kline_backtest.hpp"
#include "../include/exchange/market_data.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <map>
#include <deque>
#include <atomic>
#include <signal.h>
#include <cmath>

using namespace hft;
using namespace hft::config;
using namespace hft::backtest;
using namespace hft::exchange;

std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running = false;
}

/**
 * Simulated price feed using random walk
 * In production, this would be replaced with WebSocket feed
 */
class SimulatedPriceFeed {
public:
    struct PriceUpdate {
        std::string symbol;
        double price;
        double volume;
        Timestamp time;
    };

    SimulatedPriceFeed() {
        // Initial prices (approximate current prices)
        prices_["BTCUSDT"] = 98500.0;
        prices_["ETHUSDT"] = 3100.0;
        prices_["SOLUSDT"] = 131.0;
        prices_["BNBUSDT"] = 877.0;
    }

    void add_symbol(const std::string& symbol, double initial_price = 0) {
        if (initial_price > 0) {
            prices_[symbol] = initial_price;
        } else if (prices_.find(symbol) == prices_.end()) {
            prices_[symbol] = 100.0;  // Default
        }
    }

    PriceUpdate get_update(const std::string& symbol) {
        // Simulate price movement (random walk with slight trend)
        double& price = prices_[symbol];
        double volatility = 0.0002;  // 0.02% per tick
        double change = (rand() % 200 - 100) / 100.0 * volatility;
        price *= (1.0 + change);

        PriceUpdate update;
        update.symbol = symbol;
        update.price = price;
        update.volume = (rand() % 100) / 10.0;
        update.time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        return update;
    }

private:
    std::map<std::string, double> prices_;
};

/**
 * Aggregates ticks into klines for strategy consumption
 */
class KlineAggregator {
public:
    explicit KlineAggregator(int period_seconds = 60)
        : period_ms_(period_seconds * 1000)
        , current_open_(0)
        , current_high_(0)
        , current_low_(0)
        , current_close_(0)
        , current_volume_(0)
        , period_start_(0)
    {}

    // Returns true if a new kline is completed
    bool add_tick(double price, double volume, Timestamp time) {
        Price price_int = static_cast<Price>(price * 10000);

        if (period_start_ == 0) {
            // First tick
            period_start_ = time;
            current_open_ = current_high_ = current_low_ = current_close_ = price_int;
            current_volume_ = volume;
            return false;
        }

        // Update current candle
        current_close_ = price_int;
        if (price_int > current_high_) current_high_ = price_int;
        if (price_int < current_low_) current_low_ = price_int;
        current_volume_ += volume;

        // Check if period is complete
        if (time - period_start_ >= period_ms_) {
            // Complete the kline
            last_kline_.open_time = period_start_;
            last_kline_.close_time = time;
            last_kline_.open = current_open_;
            last_kline_.high = current_high_;
            last_kline_.low = current_low_;
            last_kline_.close = current_close_;
            last_kline_.volume = current_volume_;

            // Start new period
            period_start_ = time;
            current_open_ = current_high_ = current_low_ = current_close_ = price_int;
            current_volume_ = 0;

            return true;
        }

        return false;
    }

    const Kline& last_kline() const { return last_kline_; }

    // Get current incomplete kline for display
    Kline current_kline(Timestamp now) const {
        Kline k;
        k.open_time = period_start_;
        k.close_time = now;
        k.open = current_open_;
        k.high = current_high_;
        k.low = current_low_;
        k.close = current_close_;
        k.volume = current_volume_;
        return k;
    }

private:
    int64_t period_ms_;
    Price current_open_, current_high_, current_low_, current_close_;
    double current_volume_;
    Timestamp period_start_;
    Kline last_kline_;
};

/**
 * Paper trading account
 */
struct PaperAccount {
    double capital = 10000.0;
    std::map<std::string, Position> positions;
    int total_trades = 0;
    double total_pnl = 0;

    void print_status() const {
        std::cout << "\n=== Account Status ===\n";
        std::cout << "Capital: $" << std::fixed << std::setprecision(2) << capital << "\n";
        std::cout << "Total Trades: " << total_trades << "\n";
        std::cout << "Total P&L: $" << total_pnl << "\n";

        if (!positions.empty()) {
            std::cout << "\nOpen Positions:\n";
            for (const auto& [symbol, pos] : positions) {
                if (!pos.is_flat()) {
                    std::cout << "  " << symbol << ": "
                              << (pos.is_long() ? "LONG " : "SHORT ")
                              << pos.quantity << " @ $" << pos.avg_price << "\n";
                }
            }
        }
    }
};

std::string signal_to_string(Signal sig) {
    switch (sig) {
        case Signal::Buy: return "BUY";
        case Signal::Sell: return "SELL";
        case Signal::Close: return "CLOSE";
        default: return "HOLD";
    }
}

int main(int argc, char* argv[]) {
    std::string config_file = "trading_config.json";
    int duration_seconds = 30;

    if (argc >= 2) config_file = argv[1];
    if (argc >= 3) duration_seconds = std::stoi(argv[2]);

    signal(SIGINT, signal_handler);

    std::cout << "=== Live Trading Demo ===\n";
    std::cout << "Config: " << config_file << "\n";
    std::cout << "Duration: " << duration_seconds << " seconds\n";
    std::cout << "Press Ctrl+C to stop early\n\n";

    // Load config
    TradingConfig config;
    try {
        config = ConfigParser::load(config_file);
    } catch (const std::exception& e) {
        std::cerr << "Error loading config: " << e.what() << "\n";
        return 1;
    }

    std::cout << "Loaded " << config.symbols.size() << " symbol configurations:\n";
    for (const auto& sym : config.symbols) {
        std::cout << "  " << sym.symbol << " -> "
                  << StrategyFactory::get_name(sym.strategy, sym.params) << "\n";
    }

    // Create strategies for each symbol
    std::map<std::string, std::unique_ptr<IStrategy>> strategies;
    std::map<std::string, KlineAggregator> aggregators;
    PaperAccount account;
    account.capital = config.initial_capital;

    SimulatedPriceFeed feed;

    for (const auto& sym_config : config.symbols) {
        strategies[sym_config.symbol] = StrategyFactory::create(sym_config);
        aggregators[sym_config.symbol] = KlineAggregator(60);  // 1 minute klines
        account.positions[sym_config.symbol] = Position{};
        feed.add_symbol(sym_config.symbol);
    }

    std::cout << "\n--- Starting Live Feed ---\n\n";

    auto start_time = std::chrono::steady_clock::now();
    int tick_count = 0;

    while (g_running) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();

        if (elapsed >= duration_seconds) break;

        // Process each symbol
        for (const auto& sym_config : config.symbols) {
            const std::string& symbol = sym_config.symbol;

            // Get price update
            auto update = feed.get_update(symbol);

            // Aggregate into klines
            auto& agg = aggregators[symbol];
            bool new_kline = agg.add_tick(update.price, update.volume, update.time);

            if (new_kline) {
                // Get signal from strategy
                auto& strategy = strategies[symbol];
                auto& position = account.positions[symbol];

                Kline kline = agg.last_kline();
                Signal signal = strategy->on_kline(kline, position);

                if (signal != Signal::None) {
                    std::cout << "[" << std::setw(3) << elapsed << "s] "
                              << std::left << std::setw(10) << symbol
                              << std::right << std::setw(8) << signal_to_string(signal)
                              << " @ $" << std::fixed << std::setprecision(2) << update.price
                              << " (strategy: " << strategy_type_to_string(sym_config.strategy) << ")\n";

                    // Execute paper trade
                    if (signal == Signal::Buy && position.is_flat()) {
                        double qty = (account.capital * sym_config.max_position_pct) / update.price;
                        position.quantity = qty;
                        position.avg_price = update.price;
                        position.entry_time = update.time;
                        account.total_trades++;
                    }
                    else if ((signal == Signal::Sell || signal == Signal::Close) && position.is_long()) {
                        double pnl = (update.price - position.avg_price) * position.quantity;
                        account.total_pnl += pnl;
                        account.capital += pnl;

                        std::cout << "         -> Closed position, P&L: $"
                                  << std::fixed << std::setprecision(2) << pnl << "\n";

                        position = Position{};  // Reset
                    }
                }
            }
        }

        tick_count++;

        // Sleep to simulate real-time (faster than real for demo)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\n--- Demo Complete ---\n";
    std::cout << "Processed " << tick_count << " ticks\n";
    account.print_status();

    return 0;
}
