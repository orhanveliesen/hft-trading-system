/**
 * Backtest Runner
 *
 * Runs strategies on historical kline data.
 *
 * Usage:
 *   ./run_backtest data.csv [strategy] [params...]
 *
 * Strategies (new):
 *   sma [fast] [slow]           - SMA crossover (default: 10, 30)
 *   rsi [period] [os] [ob]      - RSI (default: 14, 30, 70)
 *   mr [lookback] [std]         - Mean reversion (default: 20, 2.0)
 *   breakout [lookback]         - Breakout (default: 20)
 *   macd [fast] [slow] [signal] - MACD (default: 12, 26, 9)
 *
 * Strategies (existing HFT):
 *   simple_mr                   - Simple Mean Reversion (HFT)
 *   momentum [lookback] [bps]   - Momentum (default: 10, 10)
 *
 *   all                         - Run all strategies
 */

#include "../include/backtest/kline_backtest.hpp"
#include "../include/backtest/strategies.hpp"
#include "../include/backtest/strategy_adapter.hpp"
#include "../include/strategy/simple_adaptive.hpp"
#include <iostream>
#include <iomanip>
#include <memory>
#include <ctime>

using namespace hft;
using namespace hft::backtest;
using namespace hft::exchange;

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " DATA_FILE [STRATEGY] [PARAMS...]\n"
              << "\n"
              << "Strategies (Technical Analysis):\n"
              << "  sma [fast] [slow]           SMA crossover (default: 10, 30)\n"
              << "  rsi [period] [os] [ob]      RSI (default: 14, 30, 70)\n"
              << "  mr [lookback] [std]         Mean reversion (default: 20, 2.0)\n"
              << "  breakout [lookback]         Breakout (default: 20)\n"
              << "  macd [fast] [slow] [signal] MACD (default: 12, 26, 9)\n"
              << "\n"
              << "Strategies (HFT - from include/strategy/):\n"
              << "  simple_mr                   Simple Mean Reversion\n"
              << "  momentum [lookback] [bps]   Momentum (default: 10, 10)\n"
              << "\n"
              << "Adaptive Strategy:\n"
              << "  adaptive                    Auto-switches strategy based on regime\n"
              << "\n"
              << "  all                         Run all strategies\n"
              << "\n"
              << "Examples:\n"
              << "  " << prog << " btc_1h.csv sma 10 50\n"
              << "  " << prog << " btc_1h.csv simple_mr\n"
              << "  " << prog << " btc_1h.csv momentum 20 15\n"
              << "  " << prog << " btc_1h.csv all\n";
}

std::string format_time(Timestamp ts) {
    time_t t = ts / 1000;
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

void run_strategy(const std::string& name, IStrategy& strategy,
                  const std::vector<Kline>& klines, const BacktestConfig& config) {
    std::cout << "\n========================================\n";
    std::cout << "Strategy: " << name << "\n";
    std::cout << "========================================\n";

    KlineBacktester bt(config);
    bt.set_klines(klines);

    BacktestStats stats = bt.run(strategy);
    stats.print();

    // Print first few trades
    const auto& trades = bt.trades();
    if (!trades.empty()) {
        std::cout << "\n--- Sample Trades ---\n";
        int count = std::min(5, static_cast<int>(trades.size()));
        for (int i = 0; i < count; ++i) {
            const auto& t = trades[i];
            std::cout << (t.side == Side::Buy ? "LONG " : "SHORT ")
                      << format_time(t.entry_time) << " -> " << format_time(t.exit_time)
                      << " | Entry: $" << std::fixed << std::setprecision(2) << (t.entry_price / 10000.0)
                      << " Exit: $" << (t.exit_price / 10000.0)
                      << " | P&L: $" << t.pnl << "\n";
        }
        if (trades.size() > 5) {
            std::cout << "... and " << (trades.size() - 5) << " more trades\n";
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string data_file = argv[1];
    std::string strategy_name = (argc >= 3) ? argv[2] : "all";

    // Load data
    std::cout << "Loading data from " << data_file << "...\n";
    auto klines = load_klines_csv(data_file);

    if (klines.empty()) {
        std::cerr << "Error: No data loaded from " << data_file << "\n";
        return 1;
    }

    std::cout << "Loaded " << klines.size() << " klines\n";
    std::cout << "Period: " << format_time(klines.front().open_time)
              << " to " << format_time(klines.back().open_time) << "\n";

    // Price range
    Price min_price = klines[0].low;
    Price max_price = klines[0].high;
    for (const auto& k : klines) {
        if (k.low < min_price) min_price = k.low;
        if (k.high > max_price) max_price = k.high;
    }
    std::cout << "Price range: $" << std::fixed << std::setprecision(2)
              << (min_price / 10000.0) << " - $" << (max_price / 10000.0) << "\n";

    // Backtest config
    BacktestConfig config;
    config.initial_capital = 10000.0;
    config.fee_rate = 0.001;       // 0.1%
    config.slippage = 0.0005;      // 0.05%
    config.max_position_pct = 0.5; // 50% per trade
    config.use_stops = true;
    config.stop_loss_pct = 0.03;   // 3% stop loss
    config.take_profit_pct = 0.06; // 6% take profit

    if (strategy_name == "all") {
        // Run all strategies
        std::cout << "\n*** Technical Analysis Strategies ***\n";
        {
            SMACrossover sma(10, 30);
            run_strategy("SMA Crossover (10/30)", sma, klines, config);
        }
        {
            RSIStrategy rsi(14, 30, 70);
            run_strategy("RSI (14, 30/70)", rsi, klines, config);
        }
        {
            MeanReversion mr(20, 2.0);
            run_strategy("Mean Reversion (20, 2.0)", mr, klines, config);
        }
        {
            BreakoutStrategy bo(20);
            run_strategy("Breakout (20)", bo, klines, config);
        }
        {
            MACDStrategy macd(12, 26, 9);
            run_strategy("MACD (12/26/9)", macd, klines, config);
        }
        std::cout << "\n*** HFT Strategies (Adapted for Kline Data) ***\n";
        {
            SimpleMRAdapter simple_mr;
            run_strategy("Simple Mean Reversion (HFT)", simple_mr, klines, config);
        }
        {
            strategy::MomentumConfig cfg;
            cfg.lookback_ticks = 10;
            cfg.threshold_bps = 10;
            MomentumAdapter momentum(cfg);
            run_strategy("Momentum (HFT)", momentum, klines, config);
        }
    }
    else if (strategy_name == "sma") {
        int fast = (argc >= 4) ? std::stoi(argv[3]) : 10;
        int slow = (argc >= 5) ? std::stoi(argv[4]) : 30;
        SMACrossover sma(fast, slow);
        run_strategy("SMA Crossover (" + std::to_string(fast) + "/" + std::to_string(slow) + ")",
                     sma, klines, config);
    }
    else if (strategy_name == "rsi") {
        int period = (argc >= 4) ? std::stoi(argv[3]) : 14;
        double os = (argc >= 5) ? std::stod(argv[4]) : 30;
        double ob = (argc >= 6) ? std::stod(argv[5]) : 70;
        RSIStrategy rsi(period, os, ob);
        run_strategy("RSI (" + std::to_string(period) + ", " +
                     std::to_string(static_cast<int>(os)) + "/" +
                     std::to_string(static_cast<int>(ob)) + ")",
                     rsi, klines, config);
    }
    else if (strategy_name == "mr") {
        int lookback = (argc >= 4) ? std::stoi(argv[3]) : 20;
        double std_mult = (argc >= 5) ? std::stod(argv[4]) : 2.0;
        MeanReversion mr(lookback, std_mult);
        run_strategy("Mean Reversion (" + std::to_string(lookback) + ", " +
                     std::to_string(std_mult) + ")",
                     mr, klines, config);
    }
    else if (strategy_name == "breakout") {
        int lookback = (argc >= 4) ? std::stoi(argv[3]) : 20;
        BreakoutStrategy bo(lookback);
        run_strategy("Breakout (" + std::to_string(lookback) + ")",
                     bo, klines, config);
    }
    else if (strategy_name == "macd") {
        int fast = (argc >= 4) ? std::stoi(argv[3]) : 12;
        int slow = (argc >= 5) ? std::stoi(argv[4]) : 26;
        int signal = (argc >= 6) ? std::stoi(argv[5]) : 9;
        MACDStrategy macd(fast, slow, signal);
        run_strategy("MACD (" + std::to_string(fast) + "/" +
                     std::to_string(slow) + "/" + std::to_string(signal) + ")",
                     macd, klines, config);
    }
    else if (strategy_name == "simple_mr") {
        // HFT Simple Mean Reversion strategy
        SimpleMRAdapter simple_mr;
        run_strategy("Simple Mean Reversion (HFT)", simple_mr, klines, config);
    }
    else if (strategy_name == "momentum") {
        // HFT Momentum strategy
        int lookback = (argc >= 4) ? std::stoi(argv[3]) : 10;
        int threshold_bps = (argc >= 5) ? std::stoi(argv[4]) : 10;
        strategy::MomentumConfig cfg;
        cfg.lookback_ticks = lookback;
        cfg.threshold_bps = threshold_bps;
        MomentumAdapter momentum(cfg);
        run_strategy("Momentum (HFT, lookback=" + std::to_string(lookback) +
                     ", bps=" + std::to_string(threshold_bps) + ")",
                     momentum, klines, config);
    }
    else if (strategy_name == "adaptive") {
        // Simple adaptive strategy - switches between MeanReversion and Breakout
        strategy::SimpleAdaptive::Config adaptive_config;
        adaptive_config.verbose = true;  // Show regime changes
        adaptive_config.min_bars_before_switch = 10;
        adaptive_config.regime_lookback = 20;

        strategy::SimpleAdaptive adaptive(adaptive_config);
        run_strategy("Adaptive (MR/Breakout)", adaptive, klines, config);

        std::cout << "\n--- Regime Stats ---\n";
        std::cout << "Final Regime: " << strategy::regime_to_string(adaptive.current_regime()) << "\n";
        std::cout << "Total Switches: " << adaptive.switch_count() << "\n";
        std::cout << "Active Strategy: " << adaptive.active_strategy_name() << "\n";
    }
    else {
        std::cerr << "Unknown strategy: " << strategy_name << "\n";
        print_usage(argv[0]);
        return 1;
    }

    return 0;
}
