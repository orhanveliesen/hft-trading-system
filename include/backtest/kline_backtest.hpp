#pragma once

#include "../exchange/market_data.hpp"
#include "../types.hpp"
#include "../strategy/signal.hpp"
#include "../strategy/trading_position.hpp"
#include <vector>
#include <string>
#include <functional>
#include <cmath>
#include <iostream>

namespace hft {
namespace backtest {

// Import generic types from strategy namespace
using Signal = strategy::Signal;
using TradingPosition = strategy::TradingPosition;

// Backward compatibility alias (deprecated)
using BacktestPosition = strategy::TradingPosition;

/**
 * Trade Record
 */
struct TradeRecord {
    Timestamp entry_time = 0;
    Timestamp exit_time = 0;
    Price entry_price = 0;
    Price exit_price = 0;
    double quantity = 0;
    Side side = Side::Buy;
    double pnl = 0;
    double fees = 0;
};

/**
 * Backtest Configuration
 */
struct BacktestConfig {
    double initial_capital = 10000.0;    // Starting capital (USD)
    double fee_rate = 0.001;             // 0.1% per trade
    double slippage = 0.0005;            // 0.05% slippage
    double max_position_pct = 0.5;       // Max 50% of capital per trade
    bool allow_shorting = false;         // Allow short selling

    // Risk management
    double stop_loss_pct = 0.02;         // 2% stop loss
    double take_profit_pct = 0.04;       // 4% take profit
    bool use_stops = true;               // Enable stop loss/take profit
};

/**
 * Backtest Result
 */
struct BacktestStats {
    double initial_capital = 0;
    double final_capital = 0;
    double total_return_pct = 0;
    double max_drawdown_pct = 0;
    double sharpe_ratio = 0;
    double sortino_ratio = 0;
    double win_rate = 0;
    double profit_factor = 0;
    int total_trades = 0;
    int winning_trades = 0;
    int losing_trades = 0;
    double avg_win = 0;
    double avg_loss = 0;
    double largest_win = 0;
    double largest_loss = 0;
    double total_fees = 0;
    Timestamp start_time = 0;
    Timestamp end_time = 0;

    void print() const;
};

/**
 * Strategy Interface
 *
 * Implement this to create custom strategies for backtesting.
 */
class IStrategy {
public:
    virtual ~IStrategy() = default;

    // Called once at start of backtest
    virtual void on_start(double capital) { (void)capital; }

    // Called for each kline - return signal
    virtual Signal on_kline(const exchange::Kline& kline, const BacktestPosition& position) = 0;

    // Called when trade is executed
    virtual void on_trade(const TradeRecord& trade) { (void)trade; }

    // Called at end of backtest
    virtual void on_end(const BacktestStats& stats) { (void)stats; }
};

/**
 * Kline Backtester
 *
 * Backtests strategies on OHLCV kline data.
 */
class KlineBacktester {
public:
    explicit KlineBacktester(const BacktestConfig& config = BacktestConfig())
        : config_(config)
        , capital_(config.initial_capital)
        , peak_capital_(config.initial_capital)
        , max_drawdown_(0)
    {}

    // Load klines from CSV
    bool load_klines(const std::string& filename) {
        klines_ = exchange::load_klines_csv(filename);
        return !klines_.empty();
    }

    // Set klines directly
    void set_klines(const std::vector<exchange::Kline>& klines) {
        klines_ = klines;
    }

    // Run backtest
    BacktestStats run(IStrategy& strategy) {
        // Reset state
        capital_ = config_.initial_capital;
        peak_capital_ = config_.initial_capital;
        max_drawdown_ = 0;
        position_ = BacktestPosition{};
        trades_.clear();
        equity_curve_.clear();

        strategy.on_start(capital_);

        for (size_t i = 0; i < klines_.size(); ++i) {
            const auto& kline = klines_[i];

            // Check stop loss / take profit
            if (config_.use_stops && !position_.is_flat()) {
                check_stops(kline);
            }

            // Get signal from strategy
            Signal signal = strategy.on_kline(kline, position_);

            // Execute signal
            execute_signal(signal, kline);

            // Track equity
            double equity = calculate_equity(kline);
            equity_curve_.push_back(equity);

            // Track drawdown
            if (equity > peak_capital_) {
                peak_capital_ = equity;
            }
            double drawdown = (peak_capital_ - equity) / peak_capital_;
            if (drawdown > max_drawdown_) {
                max_drawdown_ = drawdown;
            }
        }

        // Close any open position at end
        if (!position_.is_flat() && !klines_.empty()) {
            close_position(klines_.back());
        }

        // Calculate stats
        BacktestStats stats = calculate_stats();
        strategy.on_end(stats);

        return stats;
    }

    const std::vector<TradeRecord>& trades() const { return trades_; }
    const std::vector<double>& equity_curve() const { return equity_curve_; }
    const std::vector<exchange::Kline>& klines() const { return klines_; }

private:
    BacktestConfig config_;
    std::vector<exchange::Kline> klines_;

    // State
    double capital_;
    double peak_capital_;
    double max_drawdown_;
    BacktestPosition position_;
    std::vector<TradeRecord> trades_;
    std::vector<double> equity_curve_;

    double total_fees_ = 0;

    void execute_signal(Signal signal, const exchange::Kline& kline) {
        if (signal == Signal::None) return;

        if (signal == Signal::Close && !position_.is_flat()) {
            close_position(kline);
            return;
        }

        if (signal == Signal::Buy) {
            if (position_.is_short()) {
                close_position(kline);  // Close short first
            }
            if (position_.is_flat()) {
                open_long(kline);
            }
        } else if (signal == Signal::Sell) {
            if (position_.is_long()) {
                close_position(kline);  // Close long first
            }
            if (position_.is_flat() && config_.allow_shorting) {
                open_short(kline);
            }
        }
    }

    void open_long(const exchange::Kline& kline) {
        // Use close price with slippage
        double price = (kline.close / 10000.0) * (1 + config_.slippage);

        // Calculate position size (max position % of capital)
        double position_value = capital_ * config_.max_position_pct;
        double qty = position_value / price;

        // Calculate fees
        double fee = position_value * config_.fee_rate;

        // Update state
        capital_ -= fee;
        total_fees_ += fee;

        position_.quantity = qty;
        position_.avg_price = price;
        position_.entry_time = kline.close_time;
    }

    void open_short(const exchange::Kline& kline) {
        double price = (kline.close / 10000.0) * (1 - config_.slippage);
        double position_value = capital_ * config_.max_position_pct;
        double qty = position_value / price;
        double fee = position_value * config_.fee_rate;

        capital_ -= fee;
        total_fees_ += fee;

        position_.quantity = -qty;  // Negative for short
        position_.avg_price = price;
        position_.entry_time = kline.close_time;
    }

    void close_position(const exchange::Kline& kline) {
        if (position_.is_flat()) return;

        double exit_price;
        double pnl;

        if (position_.is_long()) {
            exit_price = (kline.close / 10000.0) * (1 - config_.slippage);
            pnl = (exit_price - position_.avg_price) * position_.quantity;
        } else {
            exit_price = (kline.close / 10000.0) * (1 + config_.slippage);
            pnl = (position_.avg_price - exit_price) * (-position_.quantity);
        }

        double position_value = exit_price * std::abs(position_.quantity);
        double fee = position_value * config_.fee_rate;

        capital_ += pnl - fee;
        total_fees_ += fee;

        // Record trade
        TradeRecord trade;
        trade.entry_time = position_.entry_time;
        trade.exit_time = kline.close_time;
        trade.entry_price = static_cast<Price>(position_.avg_price * 10000);
        trade.exit_price = static_cast<Price>(exit_price * 10000);
        trade.quantity = std::abs(position_.quantity);
        trade.side = position_.is_long() ? Side::Buy : Side::Sell;
        trade.pnl = pnl;
        trade.fees = fee;
        trades_.push_back(trade);

        position_ = BacktestPosition{};
    }

    void check_stops(const exchange::Kline& kline) {
        if (position_.is_flat()) return;

        double current_price = kline.close / 10000.0;
        double entry_price = position_.avg_price;
        double pct_change;

        if (position_.is_long()) {
            pct_change = (current_price - entry_price) / entry_price;
        } else {
            pct_change = (entry_price - current_price) / entry_price;
        }

        // Check stop loss
        if (pct_change <= -config_.stop_loss_pct) {
            close_position(kline);
            return;
        }

        // Check take profit
        if (pct_change >= config_.take_profit_pct) {
            close_position(kline);
        }
    }

    double calculate_equity(const exchange::Kline& kline) const {
        if (position_.is_flat()) {
            return capital_;
        }

        double current_price = kline.close / 10000.0;
        double unrealized_pnl;

        if (position_.is_long()) {
            unrealized_pnl = (current_price - position_.avg_price) * position_.quantity;
        } else {
            unrealized_pnl = (position_.avg_price - current_price) * (-position_.quantity);
        }

        return capital_ + unrealized_pnl;
    }

    BacktestStats calculate_stats() {
        BacktestStats stats;

        stats.initial_capital = config_.initial_capital;
        stats.final_capital = capital_;
        stats.total_return_pct = ((capital_ - config_.initial_capital) / config_.initial_capital) * 100;
        stats.max_drawdown_pct = max_drawdown_ * 100;
        stats.total_trades = static_cast<int>(trades_.size());
        stats.total_fees = total_fees_;

        if (!klines_.empty()) {
            stats.start_time = klines_.front().open_time;
            stats.end_time = klines_.back().close_time;
        }

        // Calculate trade stats
        double total_profit = 0, total_loss = 0;
        for (const auto& trade : trades_) {
            if (trade.pnl > 0) {
                stats.winning_trades++;
                total_profit += trade.pnl;
                if (trade.pnl > stats.largest_win) {
                    stats.largest_win = trade.pnl;
                }
            } else if (trade.pnl < 0) {
                stats.losing_trades++;
                total_loss += std::abs(trade.pnl);
                if (std::abs(trade.pnl) > stats.largest_loss) {
                    stats.largest_loss = std::abs(trade.pnl);
                }
            }
        }

        if (stats.total_trades > 0) {
            stats.win_rate = (static_cast<double>(stats.winning_trades) / stats.total_trades) * 100;
        }

        if (stats.winning_trades > 0) {
            stats.avg_win = total_profit / stats.winning_trades;
        }
        if (stats.losing_trades > 0) {
            stats.avg_loss = total_loss / stats.losing_trades;
        }

        if (total_loss > 0) {
            stats.profit_factor = total_profit / total_loss;
        }

        // Calculate Sharpe ratio
        stats.sharpe_ratio = calculate_sharpe();
        stats.sortino_ratio = calculate_sortino();

        return stats;
    }

    double calculate_sharpe() const {
        if (equity_curve_.size() < 2) return 0.0;

        std::vector<double> returns;
        for (size_t i = 1; i < equity_curve_.size(); ++i) {
            if (equity_curve_[i-1] != 0) {
                returns.push_back((equity_curve_[i] - equity_curve_[i-1]) / equity_curve_[i-1]);
            }
        }

        if (returns.empty()) return 0.0;

        double mean = 0;
        for (double r : returns) mean += r;
        mean /= returns.size();

        double variance = 0;
        for (double r : returns) {
            variance += (r - mean) * (r - mean);
        }
        variance /= returns.size();

        double std_dev = std::sqrt(variance);
        if (std_dev == 0) return 0.0;

        // Annualize (assuming daily klines, 365 days)
        return (mean / std_dev) * std::sqrt(365.0);
    }

    double calculate_sortino() const {
        if (equity_curve_.size() < 2) return 0.0;

        std::vector<double> returns;
        std::vector<double> negative_returns;

        for (size_t i = 1; i < equity_curve_.size(); ++i) {
            if (equity_curve_[i-1] != 0) {
                double r = (equity_curve_[i] - equity_curve_[i-1]) / equity_curve_[i-1];
                returns.push_back(r);
                if (r < 0) {
                    negative_returns.push_back(r);
                }
            }
        }

        if (returns.empty()) return 0.0;

        double mean = 0;
        for (double r : returns) mean += r;
        mean /= returns.size();

        if (negative_returns.empty()) {
            return mean > 0 ? 999.0 : 0.0;  // No downside risk
        }

        double downside_variance = 0;
        for (double r : negative_returns) {
            downside_variance += r * r;
        }
        downside_variance /= negative_returns.size();

        double downside_dev = std::sqrt(downside_variance);
        if (downside_dev == 0) return 0.0;

        return (mean / downside_dev) * std::sqrt(365.0);
    }
};

// Implementation of print
inline void BacktestStats::print() const {
    std::cout << "\n=== Backtest Results ===\n";
    std::cout << "Period: " << start_time << " - " << end_time << "\n";
    std::cout << "\n--- Capital ---\n";
    std::cout << "Initial: $" << initial_capital << "\n";
    std::cout << "Final:   $" << final_capital << "\n";
    std::cout << "Return:  " << total_return_pct << "%\n";
    std::cout << "Fees:    $" << total_fees << "\n";

    std::cout << "\n--- Risk ---\n";
    std::cout << "Max Drawdown: " << max_drawdown_pct << "%\n";
    std::cout << "Sharpe Ratio: " << sharpe_ratio << "\n";
    std::cout << "Sortino Ratio: " << sortino_ratio << "\n";

    std::cout << "\n--- Trades ---\n";
    std::cout << "Total:   " << total_trades << "\n";
    std::cout << "Winning: " << winning_trades << " (" << win_rate << "%)\n";
    std::cout << "Losing:  " << losing_trades << "\n";
    std::cout << "Profit Factor: " << profit_factor << "\n";

    std::cout << "\n--- Average Trade ---\n";
    std::cout << "Avg Win:  $" << avg_win << "\n";
    std::cout << "Avg Loss: $" << avg_loss << "\n";
    std::cout << "Largest Win:  $" << largest_win << "\n";
    std::cout << "Largest Loss: $" << largest_loss << "\n";
}

}  // namespace backtest
}  // namespace hft
