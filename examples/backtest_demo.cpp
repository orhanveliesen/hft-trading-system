#include <iostream>
#include <random>
#include <iomanip>
#include "../include/backtester.hpp"

using namespace hft;

// Generate realistic tick data simulating a trading day
// Uses larger price movements to simulate volatile market
std::vector<TickData> generate_market_data(size_t num_ticks, Price start_price) {
    std::vector<TickData> ticks;
    std::mt19937 rng(42);  // Fixed seed for reproducibility

    // Price movement parameters - more volatile for market making
    std::normal_distribution<> price_move(0, 50);    // Larger moves
    std::uniform_real_distribution<> spread_var(0.8, 1.2);
    std::uniform_int_distribution<> size_dist(100, 1000);

    Price current_price = start_price;
    Price base_spread = 20;  // 20 ticks market spread

    for (size_t i = 0; i < num_ticks; ++i) {
        // Random walk with occasional larger moves
        double move = price_move(rng);

        // Mean reversion toward start price (prevents runaway)
        double reversion = (static_cast<double>(start_price) - current_price) * 0.002;
        move += reversion;

        current_price = static_cast<Price>(std::max(
            static_cast<double>(start_price) * 0.9,
            std::min(static_cast<double>(start_price) * 1.1, current_price + move)
        ));

        // Variable spread
        Price spread = static_cast<Price>(base_spread * spread_var(rng));
        Price half_spread = spread / 2;

        TickData tick;
        tick.timestamp = i;
        tick.bid = current_price - half_spread;
        tick.ask = current_price + half_spread;
        tick.bid_size = size_dist(rng);
        tick.ask_size = size_dist(rng);

        ticks.push_back(tick);
    }

    return ticks;
}

void run_parameter_sweep() {
    std::cout << "\n=== Parameter Sweep ===\n\n";
    std::cout << std::setw(12) << "Spread(bps)"
              << std::setw(12) << "QuoteSize"
              << std::setw(12) << "MaxPos"
              << std::setw(12) << "P&L"
              << std::setw(12) << "Sharpe"
              << std::setw(12) << "Trades"
              << std::setw(12) << "MaxDD"
              << "\n";
    std::cout << std::string(84, '-') << "\n";

    // Generate market data once
    auto ticks = generate_market_data(10000, 100000);  // 10k ticks around $10.00

    std::vector<uint32_t> spreads = {2, 5, 10, 20};
    std::vector<Quantity> sizes = {10, 50, 100};
    std::vector<int64_t> positions = {100, 500, 1000};

    for (auto spread : spreads) {
        for (auto size : sizes) {
            for (auto max_pos : positions) {
                SimulatorConfig config;
                config.spread_bps = spread;
                config.quote_size = size;
                config.max_position = max_pos;
                config.skew_factor = 0.5;

                Backtester bt(config, FillMode::Aggressive);

                for (const auto& tick : ticks) {
                    bt.add_tick(tick);
                }

                auto result = bt.run();

                std::cout << std::setw(12) << spread
                          << std::setw(12) << size
                          << std::setw(12) << max_pos
                          << std::setw(12) << result.total_pnl
                          << std::setw(12) << std::fixed << std::setprecision(3) << result.sharpe_ratio
                          << std::setw(12) << result.total_trades
                          << std::setw(12) << result.max_drawdown
                          << "\n";
            }
        }
    }
}

int main() {
    std::cout << "=== HFT Market Maker Backtest Demo ===\n\n";

    // Basic configuration
    // Note: At price 100000, 10 bps = 100000 * 10 / 10000 = 100 ticks half-spread
    SimulatorConfig config;
    config.spread_bps = 5;       // 5 bps = 0.05% spread (tighter for more fills)
    config.quote_size = 50;      // Quote 50 units each side
    config.max_position = 500;   // Max 500 unit position
    config.skew_factor = 0.5;    // Moderate inventory skew
    config.max_loss = 1000000;   // Stop loss at 1M

    // Generate synthetic market data
    std::cout << "Generating market data...\n";
    auto ticks = generate_market_data(50000, 100000);  // 50k ticks, starting at 100000 ($10.00)

    std::cout << "Running backtest with " << ticks.size() << " ticks...\n\n";

    Backtester bt(config, FillMode::Aggressive);
    for (const auto& tick : ticks) {
        bt.add_tick(tick);
    }

    auto result = bt.run();
    Backtester::print_result(result);

    // Run parameter sweep
    run_parameter_sweep();

    return 0;
}
