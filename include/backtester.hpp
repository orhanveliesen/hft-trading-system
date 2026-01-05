#pragma once

#include "types.hpp"
#include "trading_simulator.hpp"
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <iostream>

namespace hft {

// Market data tick for backtesting
struct TickData {
    Timestamp timestamp;
    Price bid;
    Price ask;
    Quantity bid_size;
    Quantity ask_size;
};

// Backtest performance metrics
struct BacktestResult {
    int64_t total_pnl;
    int64_t realized_pnl;
    int64_t max_drawdown;
    uint64_t total_trades;
    uint64_t total_quotes;
    double sharpe_ratio;
    double win_rate;
    int64_t max_position;
    double avg_position;
};

// Fill simulation mode
enum class FillMode {
    Aggressive,   // Fill when market crosses our price
    Passive,      // Fill when market touches our price
    Probabilistic // Random fill based on size ratio
};

class Backtester {
public:
    explicit Backtester(const SimulatorConfig& config, FillMode fill_mode = FillMode::Aggressive)
        : simulator_(config)
        , fill_mode_(fill_mode)
        , current_bid_order_id_(0)
        , current_ask_order_id_(0)
    {}

    // Load tick data from CSV file
    // Expected format: timestamp,bid,ask,bid_size,ask_size
    bool load_csv(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            return false;
        }

        ticks_.clear();
        std::string line;

        // Skip header if present
        if (std::getline(file, line)) {
            // Check if it's a header (first char is not a digit)
            if (!line.empty() && !std::isdigit(line[0])) {
                // It's a header, continue
            } else {
                // Not a header, parse it
                file.seekg(0);
            }
        }

        while (std::getline(file, line)) {
            TickData tick;
            if (parse_csv_line(line, tick)) {
                ticks_.push_back(tick);
            }
        }

        return !ticks_.empty();
    }

    // Add tick data programmatically
    void add_tick(const TickData& tick) {
        ticks_.push_back(tick);
    }

    void add_tick(Timestamp ts, Price bid, Price ask, Quantity bid_size, Quantity ask_size) {
        ticks_.push_back(TickData{ts, bid, ask, bid_size, ask_size});
    }

    // Run backtest
    BacktestResult run() {
        simulator_.reset();
        pnl_history_.clear();
        trade_results_.clear();

        strategy::Quote current_quote{};
        int64_t peak_pnl = 0;
        int64_t max_drawdown = 0;
        int64_t position_sum = 0;
        int64_t max_pos = 0;

        for (size_t i = 0; i < ticks_.size(); ++i) {
            const auto& tick = ticks_[i];

            // Check for fills on existing quotes
            if (current_quote.has_bid || current_quote.has_ask) {
                check_fills(tick, current_quote);
            }

            // Generate new quotes
            current_quote = simulator_.on_market_data(
                tick.bid, tick.ask, tick.bid_size, tick.ask_size
            );

            // Track P&L and drawdown
            int64_t current_pnl = simulator_.total_pnl();
            pnl_history_.push_back(current_pnl);

            if (current_pnl > peak_pnl) {
                peak_pnl = current_pnl;
            }
            int64_t drawdown = peak_pnl - current_pnl;
            if (drawdown > max_drawdown) {
                max_drawdown = drawdown;
            }

            // Track position
            int64_t pos = std::abs(simulator_.position());
            position_sum += pos;
            if (pos > max_pos) max_pos = pos;
        }

        // Calculate metrics
        BacktestResult result{};
        result.total_pnl = simulator_.total_pnl();
        result.realized_pnl = simulator_.realized_pnl();
        result.max_drawdown = max_drawdown;
        result.total_trades = trade_results_.size();
        result.total_quotes = simulator_.total_quotes_generated();
        result.sharpe_ratio = calculate_sharpe();
        result.win_rate = calculate_win_rate();
        result.max_position = max_pos;
        result.avg_position = ticks_.empty() ? 0 : static_cast<double>(position_sum) / ticks_.size();

        return result;
    }

    // Print results
    static void print_result(const BacktestResult& r) {
        std::cout << "=== Backtest Results ===\n";
        std::cout << "Total P&L:     " << r.total_pnl << "\n";
        std::cout << "Realized P&L:  " << r.realized_pnl << "\n";
        std::cout << "Max Drawdown:  " << r.max_drawdown << "\n";
        std::cout << "Total Trades:  " << r.total_trades << "\n";
        std::cout << "Total Quotes:  " << r.total_quotes << "\n";
        std::cout << "Sharpe Ratio:  " << r.sharpe_ratio << "\n";
        std::cout << "Win Rate:      " << (r.win_rate * 100) << "%\n";
        std::cout << "Max Position:  " << r.max_position << "\n";
        std::cout << "Avg Position:  " << r.avg_position << "\n";
    }

    const std::vector<TickData>& ticks() const { return ticks_; }
    const std::vector<int64_t>& pnl_history() const { return pnl_history_; }

private:
    TradingSimulator simulator_;
    FillMode fill_mode_;
    std::vector<TickData> ticks_;
    std::vector<int64_t> pnl_history_;
    std::vector<int64_t> trade_results_;  // P&L per trade

    OrderId current_bid_order_id_;
    OrderId current_ask_order_id_;

    bool parse_csv_line(const std::string& line, TickData& tick) {
        std::stringstream ss(line);
        std::string token;

        try {
            if (!std::getline(ss, token, ',')) return false;
            tick.timestamp = std::stoull(token);

            if (!std::getline(ss, token, ',')) return false;
            tick.bid = static_cast<Price>(std::stoul(token));

            if (!std::getline(ss, token, ',')) return false;
            tick.ask = static_cast<Price>(std::stoul(token));

            if (!std::getline(ss, token, ',')) return false;
            tick.bid_size = static_cast<Quantity>(std::stoul(token));

            if (!std::getline(ss, token, ',')) return false;
            tick.ask_size = static_cast<Quantity>(std::stoul(token));

            return true;
        } catch (...) {
            return false;
        }
    }

    void check_fills(const TickData& tick, const strategy::Quote& quote) {
        // Check bid fill (someone sells to us)
        if (quote.has_bid && should_fill_bid(tick, quote)) {
            int64_t pnl_before = simulator_.realized_pnl();
            simulator_.on_fill(Side::Buy, quote.bid_size, quote.bid_price);
            trade_results_.push_back(simulator_.realized_pnl() - pnl_before);
        }

        // Check ask fill (someone buys from us)
        if (quote.has_ask && should_fill_ask(tick, quote)) {
            int64_t pnl_before = simulator_.realized_pnl();
            simulator_.on_fill(Side::Sell, quote.ask_size, quote.ask_price);
            trade_results_.push_back(simulator_.realized_pnl() - pnl_before);
        }
    }

    bool should_fill_bid(const TickData& tick, const strategy::Quote& quote) const {
        switch (fill_mode_) {
            case FillMode::Aggressive:
                // Fill if market ask <= our bid (someone willing to sell at or below our bid)
                return tick.ask <= quote.bid_price;
            case FillMode::Passive:
                // Fill if market ask touches our bid
                return tick.ask == quote.bid_price;
            case FillMode::Probabilistic:
                // TODO: Implement probabilistic fill
                return tick.ask <= quote.bid_price;
        }
        return false;
    }

    bool should_fill_ask(const TickData& tick, const strategy::Quote& quote) const {
        switch (fill_mode_) {
            case FillMode::Aggressive:
                // Fill if market bid >= our ask (someone willing to buy at or above our ask)
                return tick.bid >= quote.ask_price;
            case FillMode::Passive:
                return tick.bid == quote.ask_price;
            case FillMode::Probabilistic:
                return tick.bid >= quote.ask_price;
        }
        return false;
    }

    double calculate_sharpe() const {
        if (pnl_history_.size() < 2) return 0.0;

        // Calculate returns
        std::vector<double> returns;
        for (size_t i = 1; i < pnl_history_.size(); ++i) {
            returns.push_back(static_cast<double>(pnl_history_[i] - pnl_history_[i-1]));
        }

        // Mean return
        double sum = 0;
        for (double r : returns) sum += r;
        double mean = sum / returns.size();

        // Std dev
        double sq_sum = 0;
        for (double r : returns) {
            sq_sum += (r - mean) * (r - mean);
        }
        double std_dev = std::sqrt(sq_sum / returns.size());

        if (std_dev == 0) return 0.0;

        // Annualize (assuming ~252 trading days, ~6.5 hours, ~23400 seconds)
        // Sharpe = mean / std * sqrt(N) where N is periods per year
        // For tick data, we'll just return raw Sharpe
        return mean / std_dev;
    }

    double calculate_win_rate() const {
        if (trade_results_.empty()) return 0.0;

        int wins = 0;
        for (int64_t pnl : trade_results_) {
            if (pnl > 0) ++wins;
        }
        return static_cast<double>(wins) / trade_results_.size();
    }
};

}  // namespace hft
