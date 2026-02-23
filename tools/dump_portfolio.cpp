/**
 * Dump shared portfolio state for debugging
 */
#include "../include/ipc/shared_portfolio_state.hpp"

#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <sys/mman.h>
#include <unistd.h>

int main() {
    // Try direct mmap first for debugging
    int fd = open("/dev/shm/trader_portfolio", O_RDONLY);
    if (fd < 0) {
        std::cerr << "Cannot open /dev/shm/trader_portfolio: " << strerror(errno) << "\n";
        return 1;
    }

    void* ptr = mmap(nullptr, sizeof(hft::ipc::SharedPortfolioState), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);

    if (ptr == MAP_FAILED) {
        std::cerr << "mmap failed: " << strerror(errno) << "\n";
        return 1;
    }

    auto* state = static_cast<hft::ipc::SharedPortfolioState*>(ptr);

    std::cout << "[ Validation ]\n";
    std::cout << "  Expected magic: 0x" << std::hex << hft::ipc::SharedPortfolioState::MAGIC << "\n";
    std::cout << "  Actual magic:   0x" << state->magic << std::dec << "\n";
    std::cout << "  Expected ver:   " << hft::ipc::SharedPortfolioState::VERSION << "\n";
    std::cout << "  Actual ver:     " << state->version << "\n";
    std::cout << "  is_valid():     " << (state->is_valid() ? "YES" : "NO") << "\n";
    std::cout << "\n";

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n=== SHARED PORTFOLIO STATE DUMP ===\n\n";

    std::cout << "[ Raw Values ]\n";
    std::cout << "  magic:           0x" << std::hex << state->magic << std::dec << "\n";
    std::cout << "  version:         " << state->version << "\n";
    std::cout << "  session_id:      " << state->session_id << "\n";
    std::cout << "  sequence:        " << state->sequence.load() << "\n";
    std::cout << "\n";

    std::cout << "[ Cash & P&L (raw x8) ]\n";
    std::cout << "  cash_x8:              " << state->cash_x8.load() << "\n";
    std::cout << "  initial_cash_x8:      " << state->initial_cash_x8.load() << "\n";
    std::cout << "  total_realized_pnl_x8:" << state->total_realized_pnl_x8.load() << "\n";
    std::cout << "\n";

    std::cout << "[ Cash & P&L (converted) ]\n";
    std::cout << "  cash:             $" << state->cash() << "\n";
    std::cout << "  initial_cash:     $" << state->initial_cash() << "\n";
    std::cout << "  realized_pnl:     $" << state->total_realized_pnl() << "\n";
    std::cout << "  unrealized_pnl:   $" << state->total_unrealized_pnl() << "\n";
    std::cout << "\n";

    std::cout << "[ Calculated Values ]\n";
    double market_value = state->total_market_value();
    double equity = state->total_equity(); // cash + market_value
    double pnl = state->total_pnl();       // equity - initial_cash
    double pnl_pct = (pnl / state->initial_cash()) * 100.0;

    std::cout << "  market_value:     $" << market_value << "\n";
    std::cout << "  equity:           $" << equity << " (cash + market_value)\n";
    std::cout << "  P&L:              $" << pnl << "\n";
    std::cout << "  P&L %:            " << pnl_pct << "%\n";
    std::cout << "\n";

    std::cout << "[ P&L Reconciliation ]\n";
    double realized = state->total_realized_pnl();
    double unrealized = state->total_unrealized_pnl();
    double commission = state->total_commissions();
    double slippage = state->total_slippage();
    double component_pnl = realized + unrealized - commission;
    double difference = pnl - component_pnl;

    std::cout << "  Equity-based P&L: $" << pnl << " (cash + mkt_val - initial)\n";
    std::cout << "  Component P&L:    $" << component_pnl << " (R + U - C)\n";
    std::cout << "    Realized:       $" << realized << "\n";
    std::cout << "    Unrealized:     $" << unrealized << "\n";
    std::cout << "    Commission:     $" << commission << "\n";
    std::cout << "  DIFFERENCE:       $" << difference << "\n";
    std::cout << "  (Slippage $" << slippage << " already in R/U - not subtracted)\n";
    std::cout << "\n";

    std::cout << "[ Trade Stats ]\n";
    std::cout << "  total_events:     " << state->total_events.load() << "\n";
    std::cout << "  total_fills:      " << state->total_fills.load() << "\n";
    std::cout << "  winning_trades:   " << state->winning_trades.load() << "\n";
    std::cout << "  losing_trades:    " << state->losing_trades.load() << "\n";
    std::cout << "  total_targets:    " << state->total_targets.load() << "\n";
    std::cout << "  total_stops:      " << state->total_stops.load() << "\n";
    std::cout << "  win_rate:         " << state->win_rate() << "%\n";
    std::cout << "\n";

    std::cout << "[ Trading Costs (raw x8) ]\n";
    std::cout << "  commissions_x8:   " << state->total_commissions_x8.load() << "\n";
    std::cout << "  spread_cost_x8:   " << state->total_spread_cost_x8.load() << "\n";
    std::cout << "  slippage_x8:      " << state->total_slippage_x8.load() << "\n";
    std::cout << "  volume_x8:        " << state->total_volume_x8.load() << "\n";
    std::cout << "\n";

    std::cout << "[ Trading Costs (converted) ]\n";
    std::cout << "  commissions:      $" << state->total_commissions() << "\n";
    std::cout << "  spread_cost:      $" << state->total_spread_cost() << "\n";
    std::cout << "  slippage:         $" << state->total_slippage() << "\n";
    std::cout << "  total_costs:      $" << state->total_costs() << "\n";
    std::cout << "  total_volume:     $" << state->total_volume() << "\n";
    std::cout << "\n";

    std::cout << "[ Active Positions ]\n";
    int active_count = 0;
    for (size_t i = 0; i < hft::ipc::MAX_PORTFOLIO_SYMBOLS; ++i) {
        const auto& pos = state->positions[i];
        if (pos.active.load()) {
            double qty = pos.quantity();
            if (qty > 0.0001) {
                std::cout << "  [" << i << "] " << pos.symbol << ": qty=" << std::setprecision(6) << qty
                          << " avg=" << std::setprecision(2) << pos.avg_price() << " last=" << pos.last_price()
                          << " unreal=" << pos.unrealized_pnl() << "\n";
                active_count++;
            }
        }
    }
    if (active_count == 0) {
        std::cout << "  (no active positions with qty > 0)\n";
    }
    std::cout << "\n";

    std::cout << "=== END DUMP ===\n\n";
    return 0;
}
