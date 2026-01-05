#include <iostream>
#include <iomanip>
#include "../include/backtester.hpp"

using namespace hft;

int main(int argc, char* argv[]) {
    std::string data_file = "data/binance/BTCUSDT-backtest.csv";
    if (argc > 1) {
        data_file = argv[1];
    }

    std::cout << "=== HFT Market Maker - Real Data Backtest ===\n\n";
    std::cout << "Loading data from: " << data_file << "\n";

    // Optimal parameters found from parameter sweep
    SimulatorConfig config;
    config.spread_bps = 10;      // 10 bps spread (profitable config)
    config.quote_size = 5;       // 5 BTC per quote
    config.max_position = 10;    // Max 10 BTC position (conservative)
    config.skew_factor = 0.5;
    config.max_loss = 100000000; // 10M max loss

    Backtester bt(config, FillMode::Aggressive);

    if (!bt.load_csv(data_file)) {
        std::cerr << "Failed to load data file: " << data_file << "\n";
        return 1;
    }

    std::cout << "Loaded " << bt.ticks().size() << " ticks\n";
    std::cout << "Running backtest...\n\n";

    auto result = bt.run();
    Backtester::print_result(result);

    // Calculate additional metrics
    double total_volume = result.total_trades * config.quote_size;
    double pnl_per_trade = result.total_trades > 0 ?
        static_cast<double>(result.total_pnl) / result.total_trades : 0;

    std::cout << "\n=== Additional Metrics (USD) ===\n";
    std::cout << "Total P&L (USD):     $" << std::fixed << std::setprecision(2)
              << (result.total_pnl / 10000.0) << "\n";
    std::cout << "Realized P&L (USD):  $" << (result.realized_pnl / 10000.0) << "\n";
    std::cout << "Max Drawdown (USD):  $" << (result.max_drawdown / 10000.0) << "\n";
    std::cout << "P&L per Trade (USD): $" << (pnl_per_trade / 10000.0) << "\n";
    std::cout << "Total Volume:        " << total_volume << " BTC\n";

    // Parameter sweep for optimization
    std::cout << "\n=== Parameter Sweep (P&L in USD) ===\n\n";
    std::cout << std::setw(8) << "Spread"
              << std::setw(8) << "Size"
              << std::setw(8) << "MaxPos"
              << std::setw(15) << "P&L ($)"
              << std::setw(10) << "Sharpe"
              << std::setw(8) << "Trades"
              << std::setw(15) << "MaxDD ($)"
              << "\n";
    std::cout << std::string(72, '-') << "\n";

    std::vector<uint32_t> spreads = {5, 10, 15, 20};
    std::vector<Quantity> sizes = {1, 5, 10};
    std::vector<int64_t> positions = {5, 10, 20};

    for (auto spread : spreads) {
        for (auto size : sizes) {
            for (auto max_pos : positions) {
                SimulatorConfig cfg;
                cfg.spread_bps = spread;
                cfg.quote_size = size;
                cfg.max_position = max_pos;
                cfg.skew_factor = 0.5;
                cfg.max_loss = 100000000;

                Backtester sweep_bt(cfg, FillMode::Aggressive);
                sweep_bt.load_csv(data_file);
                auto r = sweep_bt.run();

                std::cout << std::setw(8) << spread
                          << std::setw(8) << size
                          << std::setw(8) << max_pos
                          << std::setw(15) << std::fixed << std::setprecision(0) << (r.total_pnl / 10000.0)
                          << std::setw(10) << std::setprecision(4) << r.sharpe_ratio
                          << std::setw(8) << r.total_trades
                          << std::setw(15) << std::setprecision(0) << (r.max_drawdown / 10000.0)
                          << "\n";
            }
        }
    }

    return 0;
}
