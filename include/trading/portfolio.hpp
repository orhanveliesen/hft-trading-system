#pragma once

/**
 * Portfolio - Pre-allocated position tracking with zero allocation on hot path
 *
 * Features:
 * - Pre-allocated storage for positions (no std::vector, no new/delete)
 * - Symbol-aware config: uses symbol-specific or global settings
 * - Target/stop-loss tracking with callback-based exit
 * - FIFO position management
 *
 * Usage:
 *   Portfolio portfolio;
 *   portfolio.set_config(shared_config);
 *   portfolio.set_symbol_configs(symbol_configs);
 *   portfolio.init(100000.0);  // $100k starting capital
 *   portfolio.buy(symbol_id, price, qty);
 */

#include <array>
#include <map>
#include <cstdint>

#include "../types.hpp"
#include "../config/defaults.hpp"

// Forward declarations
namespace hft::ipc {
    struct SharedConfig;
    struct SharedSymbolConfigs;
}

namespace hft::trading {

// =============================================================================
// Constants
// =============================================================================
constexpr size_t MAX_SYMBOLS = 64;              // Max symbols we can track
constexpr size_t MAX_POSITIONS_PER_SYMBOL = 32; // Max open positions per symbol

// =============================================================================
// OpenPosition - Single position slot with entry/target/stop
// =============================================================================
struct OpenPosition {
    double entry_price = 0;
    double quantity = 0;
    double target_price = 0;
    double stop_loss_price = 0;
    double peak_price = 0;
    uint64_t timestamp = 0;
    bool active = false;

    void clear();
    bool update_peak_and_check_trend_exit(double current_price, double pullback_pct = 0.005);
};

// =============================================================================
// SymbolPositions - Pre-allocated position storage for one symbol
// =============================================================================
struct SymbolPositions {
    std::array<OpenPosition, MAX_POSITIONS_PER_SYMBOL> slots;
    size_t count = 0;

    bool add(double entry, double qty, double target, double stop_loss);
    double total_quantity() const;
    double avg_entry() const;
    void clear_all();
};

// =============================================================================
// Portfolio - Main portfolio tracker
// =============================================================================
struct Portfolio {
    double cash = 0;
    std::array<SymbolPositions, MAX_SYMBOLS> positions;
    std::array<bool, MAX_SYMBOLS> symbol_active;

    // Config pointers (optional, fall back to defaults if null)
    const ipc::SharedConfig* config_ = nullptr;
    const ipc::SharedSymbolConfigs* symbol_configs_ = nullptr;

    // Trading costs tracking
    double total_commissions = 0;
    double total_spread_cost = 0;
    double total_volume = 0;
    double pending_cash = 0;

    // === Config Accessors ===
    double target_pct() const;
    double stop_pct() const;
    double commission_rate() const;
    double pullback_pct() const;
    double base_position_pct(const char* symbol = nullptr) const;
    double max_position_pct(const char* symbol = nullptr) const;

    // === Setters ===
    void set_config(const ipc::SharedConfig* cfg);
    void set_symbol_configs(const ipc::SharedSymbolConfigs* cfgs);

    // === Initialization ===
    void init(double capital);

    // === Position Sizing ===
    double calculate_qty(double price, double available_cash, const char* symbol = nullptr) const;

    // === Position Queries ===
    double get_holding(Symbol s) const;
    bool can_buy(double price, double qty) const;
    bool can_sell(Symbol s, double qty) const;
    double avg_entry_price(Symbol s) const;

    // === Cash Reservation ===
    void reserve_cash(double amount);
    void release_reserved_cash(double amount);

    // === Trading Operations ===
    double buy(Symbol s, double price, double qty, double spread_cost = 0, double commission = 0);
    double sell(Symbol s, double price, double qty, double spread_cost = 0, double commission = 0);

    // === Target/Stop Checking (template - must be in header) ===
    template<typename OnTargetHit, typename OnStopHit, typename OnTrendExit>
    int check_and_close(Symbol s, double current_price,
                        OnTargetHit on_target, OnStopHit on_stop, OnTrendExit on_trend_exit,
                        double pullback_threshold = 0.005);

    // === Portfolio Value ===
    double total_value(const std::array<double, MAX_SYMBOLS>& prices) const;
    double total_value(const std::map<Symbol, double>& prices) const;

    int position_count() const;
    int total_position_slots() const;
};

// =============================================================================
// Template Implementation (must be in header)
// =============================================================================
template<typename OnTargetHit, typename OnStopHit, typename OnTrendExit>
int Portfolio::check_and_close(Symbol s, double current_price,
                               OnTargetHit on_target, OnStopHit on_stop, OnTrendExit on_trend_exit,
                               double pullback_threshold) {
    if (s >= MAX_SYMBOLS) return 0;

    int closed = 0;
    auto& sym_pos = positions[s];

    for (auto& slot : sym_pos.slots) {
        if (!slot.active) continue;

        // TARGET HIT
        if (current_price >= slot.target_price) {
            double qty = slot.quantity;
            double trade_value = current_price * qty;
            double commission = trade_value * commission_rate();
            cash += trade_value - commission;
            total_commissions += commission;
            on_target(qty, slot.entry_price, current_price);
            slot.clear();
            sym_pos.count--;
            closed++;
            continue;
        }

        // TREND EXIT
        if (slot.update_peak_and_check_trend_exit(current_price, pullback_threshold)) {
            double qty = slot.quantity;
            double trade_value = current_price * qty;
            double commission = trade_value * commission_rate();
            cash += trade_value - commission;
            total_commissions += commission;
            on_trend_exit(qty, slot.entry_price, current_price, slot.peak_price);
            slot.clear();
            sym_pos.count--;
            closed++;
            continue;
        }

        // STOP-LOSS HIT
        if (current_price <= slot.stop_loss_price) {
            double qty = slot.quantity;
            double trade_value = current_price * qty;
            double commission = trade_value * commission_rate();
            cash += trade_value - commission;
            total_commissions += commission;
            on_stop(qty, slot.entry_price, current_price);
            slot.clear();
            sym_pos.count--;
            closed++;
        }
    }

    if (sym_pos.count == 0) {
        symbol_active[s] = false;
    }
    return closed;
}

} // namespace hft::trading
