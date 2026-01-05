/**
 * Strategy Optimizer
 *
 * Tests all strategies on multiple symbols and generates optimal config.
 *
 * Usage:
 *   ./optimize_strategies [options] symbol1 symbol2 ...
 *
 * Options:
 *   -o, --output FILE     Output config file (default: trading_config.json)
 *   -d, --days N          Days of historical data (default: 90)
 *   -i, --interval INT    Kline interval: 1h, 4h, 1d (default: 1h)
 *   --download            Download fresh data from Binance
 *
 * Examples:
 *   ./optimize_strategies BTCUSDT ETHUSDT SOLUSDT
 *   ./optimize_strategies --download -d 180 BTCUSDT ETHUSDT
 */

#include "../include/config/strategy_config.hpp"
#include "../include/config/strategy_factory.hpp"
#include "../include/backtest/kline_backtest.hpp"
#include "../include/exchange/market_data.hpp"
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>

using namespace hft;
using namespace hft::config;
using namespace hft::backtest;
using namespace hft::exchange;

struct OptimizationResult {
    StrategyType strategy;
    StrategyParams params;
    BacktestStats stats;
    double score;  // Combined score for ranking
};

// Calculate a combined score from backtest stats
// Higher is better
double calculate_score(const BacktestStats& stats) {
    // Weighted combination:
    // - Return is important but not everything
    // - Sharpe ratio captures risk-adjusted return
    // - Win rate gives consistency
    // - Low drawdown is critical

    double return_score = stats.total_return_pct;  // Can be negative
    double sharpe_score = stats.sharpe_ratio * 10; // Scale up
    double win_rate_score = (stats.win_rate - 50) * 0.5;  // Bonus for >50%
    double drawdown_penalty = -stats.max_drawdown_pct * 2;  // Penalize drawdown
    double profit_factor_bonus = (stats.profit_factor > 1.0) ?
        (stats.profit_factor - 1.0) * 20 : (stats.profit_factor - 1.0) * 40;

    return return_score + sharpe_score + win_rate_score +
           drawdown_penalty + profit_factor_bonus;
}

// Test a single strategy on data
OptimizationResult test_strategy(StrategyType type, const StrategyParams& params,
                                  const std::vector<Kline>& klines,
                                  const BacktestConfig& bt_config) {
    OptimizationResult result;
    result.strategy = type;
    result.params = params;

    auto strategy = StrategyFactory::create(type, params);
    KlineBacktester bt(bt_config);
    bt.set_klines(klines);
    result.stats = bt.run(*strategy);
    result.score = calculate_score(result.stats);

    return result;
}

// Test all strategies and find the best one
OptimizationResult find_best_strategy(const std::string& symbol,
                                       const std::vector<Kline>& klines,
                                       const BacktestConfig& bt_config) {
    std::vector<OptimizationResult> results;

    auto strategy_types = StrategyFactory::get_all_types();

    std::cout << "\n  Testing strategies for " << symbol << ":\n";
    std::cout << "  " << std::string(60, '-') << "\n";
    std::cout << "  " << std::left << std::setw(20) << "Strategy"
              << std::right << std::setw(10) << "Return"
              << std::setw(10) << "Sharpe"
              << std::setw(10) << "WinRate"
              << std::setw(10) << "MaxDD"
              << std::setw(10) << "Score" << "\n";
    std::cout << "  " << std::string(60, '-') << "\n";

    for (auto type : strategy_types) {
        StrategyParams params = StrategyFactory::get_default_params(type);
        auto result = test_strategy(type, params, klines, bt_config);
        results.push_back(result);

        std::string name = StrategyFactory::get_name(type, params);
        std::cout << "  " << std::left << std::setw(20) << name
                  << std::right << std::fixed << std::setprecision(2)
                  << std::setw(9) << result.stats.total_return_pct << "%"
                  << std::setw(10) << result.stats.sharpe_ratio
                  << std::setw(9) << result.stats.win_rate << "%"
                  << std::setw(9) << result.stats.max_drawdown_pct << "%"
                  << std::setw(10) << result.score << "\n";
    }

    // Find best by score
    auto best = std::max_element(results.begin(), results.end(),
        [](const OptimizationResult& a, const OptimizationResult& b) {
            return a.score < b.score;
        });

    std::cout << "  " << std::string(60, '-') << "\n";
    std::cout << "  Best: " << StrategyFactory::get_name(best->strategy, best->params)
              << " (score: " << std::fixed << std::setprecision(2) << best->score << ")\n";

    return *best;
}

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options] SYMBOL1 [SYMBOL2 ...]\n\n"
              << "Options:\n"
              << "  -o, --output FILE     Output config file (default: trading_config.json)\n"
              << "  -d, --data-dir DIR    Data directory (default: current dir)\n"
              << "  -c, --capital N       Initial capital (default: 10000)\n"
              << "\n"
              << "Examples:\n"
              << "  " << prog << " -o config.json BTCUSDT ETHUSDT\n"
              << "  " << prog << " -d ../data BTCUSDT\n\n"
              << "Note: Data files should be named like: btcusdt_1h.csv\n";
}

int main(int argc, char* argv[]) {
    std::string output_file = "trading_config.json";
    std::string data_dir = ".";
    double initial_capital = 10000.0;
    std::vector<std::string> symbols;

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            output_file = argv[++i];
        }
        else if ((arg == "-d" || arg == "--data-dir") && i + 1 < argc) {
            data_dir = argv[++i];
        }
        else if ((arg == "-c" || arg == "--capital") && i + 1 < argc) {
            initial_capital = std::stod(argv[++i]);
        }
        else if (arg[0] != '-') {
            // Assume it's a symbol
            symbols.push_back(arg);
        }
    }

    if (symbols.empty()) {
        std::cerr << "Error: No symbols specified\n\n";
        print_usage(argv[0]);
        return 1;
    }

    std::cout << "=== Strategy Optimizer ===\n";
    std::cout << "Symbols: ";
    for (const auto& s : symbols) std::cout << s << " ";
    std::cout << "\n";
    std::cout << "Initial Capital: $" << initial_capital << "\n";
    std::cout << "Output: " << output_file << "\n";

    // Backtest config
    BacktestConfig bt_config;
    bt_config.initial_capital = initial_capital;
    bt_config.fee_rate = 0.001;
    bt_config.slippage = 0.0005;
    bt_config.max_position_pct = 0.5;
    bt_config.use_stops = true;
    bt_config.stop_loss_pct = 0.03;
    bt_config.take_profit_pct = 0.06;

    // Trading config to generate
    TradingConfig trading_config;
    trading_config.initial_capital = initial_capital;
    trading_config.fee_rate = bt_config.fee_rate;
    trading_config.slippage = bt_config.slippage;

    // Process each symbol
    for (const auto& symbol : symbols) {
        // Try to find data file
        std::string lower_symbol = symbol;
        std::transform(lower_symbol.begin(), lower_symbol.end(),
                       lower_symbol.begin(), ::tolower);

        std::vector<std::string> possible_files = {
            data_dir + "/" + lower_symbol + "_1h.csv",
            data_dir + "/" + lower_symbol + "_hourly.csv",
            data_dir + "/" + lower_symbol + "_3m_hourly.csv",
            data_dir + "/" + symbol + "_1h.csv",
            data_dir + "/" + symbol + ".csv"
        };

        std::string data_file;
        for (const auto& f : possible_files) {
            std::ifstream test(f);
            if (test.good()) {
                data_file = f;
                break;
            }
        }

        if (data_file.empty()) {
            std::cerr << "\nWarning: No data file found for " << symbol << "\n";
            std::cerr << "Tried: ";
            for (const auto& f : possible_files) std::cerr << f << " ";
            std::cerr << "\n";
            continue;
        }

        std::cout << "\n========================================\n";
        std::cout << "Optimizing: " << symbol << "\n";
        std::cout << "Data file: " << data_file << "\n";
        std::cout << "========================================\n";

        // Load data
        auto klines = load_klines_csv(data_file);
        if (klines.empty()) {
            std::cerr << "Error: No data loaded from " << data_file << "\n";
            continue;
        }

        std::cout << "Loaded " << klines.size() << " klines\n";

        // Find best strategy
        auto best = find_best_strategy(symbol, klines, bt_config);

        // Create symbol config
        SymbolConfig sym_config;
        sym_config.symbol = symbol;
        sym_config.strategy = best.strategy;
        sym_config.params = best.params;
        sym_config.max_position_pct = bt_config.max_position_pct;
        sym_config.stop_loss_pct = bt_config.stop_loss_pct;
        sym_config.take_profit_pct = bt_config.take_profit_pct;
        sym_config.expected_return = best.stats.total_return_pct;
        sym_config.win_rate = best.stats.win_rate;
        sym_config.profit_factor = best.stats.profit_factor;
        sym_config.max_drawdown = best.stats.max_drawdown_pct;
        sym_config.sharpe_ratio = best.stats.sharpe_ratio;

        trading_config.symbols.push_back(sym_config);
    }

    if (trading_config.symbols.empty()) {
        std::cerr << "\nError: No symbols were successfully optimized\n";
        return 1;
    }

    // Save config
    std::cout << "\n========================================\n";
    std::cout << "Saving configuration to " << output_file << "\n";
    std::cout << "========================================\n";

    ConfigParser::save(output_file, trading_config);

    std::cout << "\nOptimization complete!\n";
    std::cout << "\nSummary:\n";
    std::cout << std::left << std::setw(12) << "Symbol"
              << std::setw(18) << "Strategy"
              << std::right << std::setw(10) << "Return"
              << std::setw(10) << "WinRate"
              << std::setw(10) << "PF" << "\n";
    std::cout << std::string(60, '-') << "\n";

    for (const auto& sym : trading_config.symbols) {
        std::string name = StrategyFactory::get_name(sym.strategy, sym.params);
        std::cout << std::left << std::setw(12) << sym.symbol
                  << std::setw(18) << name
                  << std::right << std::fixed << std::setprecision(2)
                  << std::setw(9) << sym.expected_return << "%"
                  << std::setw(9) << sym.win_rate << "%"
                  << std::setw(10) << sym.profit_factor << "\n";
    }

    std::cout << "\nConfig file saved: " << output_file << "\n";
    std::cout << "Use with: ./run_trading --config " << output_file << "\n";

    return 0;
}
