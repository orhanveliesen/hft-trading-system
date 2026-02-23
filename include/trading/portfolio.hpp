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
 *
 * NOTE: Header-only by design for HFT performance.
 * All methods inline to eliminate function call overhead.
 */

#include "../config/defaults.hpp"
#include "../ipc/shared_config.hpp"
#include "../ipc/symbol_config.hpp"
#include "../types.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>

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

    inline void clear() {
        entry_price = 0;
        quantity = 0;
        target_price = 0;
        stop_loss_price = 0;
        peak_price = 0;
        timestamp = 0;
        active = false;
    }

    inline bool update_peak_and_check_trend_exit(double current_price, double pullback_pct = 0.005) {
        if (current_price > peak_price) {
            peak_price = current_price;
        }

        bool in_profit = current_price > entry_price;
        double pullback = (peak_price - current_price) / peak_price;
        return in_profit && pullback >= pullback_pct;
    }
};

// =============================================================================
// SymbolPositions - Pre-allocated position storage for one symbol
// =============================================================================
struct SymbolPositions {
    std::array<OpenPosition, MAX_POSITIONS_PER_SYMBOL> slots;
    size_t count = 0;

    inline bool add(double entry, double qty, double target, double stop_loss) {
        if (count >= MAX_POSITIONS_PER_SYMBOL)
            return false;

        for (auto& slot : slots) {
            if (!slot.active) {
                slot.entry_price = entry;
                slot.quantity = qty;
                slot.target_price = target;
                slot.stop_loss_price = stop_loss;
                slot.peak_price = entry;
                slot.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
                slot.active = true;
                count++;
                return true;
            }
        }
        return false;
    }

    inline double total_quantity() const {
        double total = 0;
        for (const auto& slot : slots) {
            if (slot.active)
                total += slot.quantity;
        }
        return total;
    }

    inline double avg_entry() const {
        double total_cost = 0, total_qty = 0;
        for (const auto& slot : slots) {
            if (slot.active) {
                total_cost += slot.entry_price * slot.quantity;
                total_qty += slot.quantity;
            }
        }
        return total_qty > 0 ? total_cost / total_qty : 0;
    }

    inline void clear_all() {
        for (auto& slot : slots)
            slot.clear();
        count = 0;
    }
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
    inline double target_pct() const { return config_ ? config_->target_pct() / 100.0 : config::targets::TARGET_PCT; }

    inline double stop_pct() const { return config_ ? config_->stop_pct() / 100.0 : config::targets::STOP_PCT; }

    inline double commission_rate() const {
        return config_ ? config_->commission_rate() : config::costs::COMMISSION_PCT;
    }

    inline double pullback_pct() const {
        return config_ ? config_->pullback_pct() / 100.0 : config::targets::PULLBACK_PCT;
    }

    inline double base_position_pct(const char* symbol = nullptr) const {
        if (symbol && symbol_configs_) {
            const auto* sym_cfg = symbol_configs_->find(symbol);
            if (sym_cfg && !sym_cfg->use_global_position()) {
                return sym_cfg->base_position_x100 / 10000.0;
            }
        }
        return config_ ? config_->base_position_pct() / 100.0 : config::position::BASE_PCT;
    }

    inline double max_position_pct(const char* symbol = nullptr) const {
        if (symbol && symbol_configs_) {
            const auto* sym_cfg = symbol_configs_->find(symbol);
            if (sym_cfg && !sym_cfg->use_global_position()) {
                return sym_cfg->max_position_x100 / 10000.0;
            }
        }
        return config_ ? config_->max_position_pct() / 100.0 : config::position::MAX_PCT;
    }

    // === Setters ===
    inline void set_config(const ipc::SharedConfig* cfg) { config_ = cfg; }

    inline void set_symbol_configs(const ipc::SharedSymbolConfigs* cfgs) { symbol_configs_ = cfgs; }

    // === Initialization ===
    inline void init(double capital) {
        cash = capital;
        total_commissions = 0;
        total_spread_cost = 0;
        total_volume = 0;
        pending_cash = 0;
        for (size_t i = 0; i < MAX_SYMBOLS; i++) {
            positions[i].clear_all();
            symbol_active[i] = false;
        }
    }

    // === Position Sizing ===
    inline double calculate_qty(double price, double available_cash, const char* symbol = nullptr) const {
        if (price <= 0)
            return 0;

        double position_value = available_cash * base_position_pct(symbol);
        double max_value = available_cash * max_position_pct(symbol);
        position_value = std::min(position_value, max_value);

        double qty = position_value / price;
        qty = std::floor(qty * 1e8) / 1e8; // Binance precision

        if (qty * price < 10.0)
            return 0; // Minimum order size
        return qty;
    }

    // === Position Queries ===
    inline double get_holding(Symbol s) const {
        if (s >= MAX_SYMBOLS)
            return 0;
        return positions[s].total_quantity();
    }

    inline bool can_buy(double price, double qty) const {
        double available = cash - pending_cash;
        return available >= price * qty;
    }

    inline bool can_sell(Symbol s, double qty) const { return get_holding(s) >= qty; }

    /**
     * Check if we can add more positions for a symbol.
     * Returns false when MAX_POSITIONS_PER_SYMBOL is reached.
     * IMPORTANT: Call this BEFORE sending orders to avoid sending
     * orders that will be rejected by the portfolio.
     */
    inline bool can_add_position(Symbol s) const {
        if (s >= MAX_SYMBOLS)
            return false;
        return positions[s].count < MAX_POSITIONS_PER_SYMBOL;
    }

    inline double avg_entry_price(Symbol s) const {
        if (s >= MAX_SYMBOLS)
            return 0;
        return positions[s].avg_entry();
    }

    // === Cash Reservation ===
    inline void reserve_cash(double amount) { pending_cash += amount; }

    inline void release_reserved_cash(double amount) {
        pending_cash -= amount;
        if (pending_cash < 0)
            pending_cash = 0;
    }

    // === Trading Operations ===
    inline double buy(Symbol s, double price, double qty, double spread_cost = 0, double commission = 0) {
        if (qty <= 0 || price <= 0)
            return 0;
        if (s >= MAX_SYMBOLS)
            return 0;

        double target = price * (1.0 + target_pct());
        double stop_loss = price * (1.0 - stop_pct());

        if (positions[s].add(price, qty, target, stop_loss)) {
            double trade_value = price * qty;
            double actual_commission = commission > 0 ? commission : trade_value * commission_rate();
            cash -= trade_value + actual_commission;
            total_commissions += actual_commission;
            total_spread_cost += spread_cost;
            total_volume += trade_value;
            symbol_active[s] = true;
            return actual_commission;
        }
        return 0;
    }

    inline double sell(Symbol s, double price, double qty, double spread_cost = 0, double commission = 0) {
        if (qty <= 0 || price <= 0)
            return 0;
        if (s >= MAX_SYMBOLS)
            return 0;

        double remaining = qty;
        auto& sym_pos = positions[s];
        double actual_sold = 0;

        for (auto& slot : sym_pos.slots) {
            if (!slot.active || remaining <= 0)
                continue;

            double sell_qty = std::min(remaining, slot.quantity);
            slot.quantity -= sell_qty;
            remaining -= sell_qty;
            actual_sold += sell_qty;

            if (slot.quantity <= 0.0001) {
                slot.clear();
                sym_pos.count--;
            }
        }

        double trade_value = price * actual_sold;
        double actual_commission = commission > 0 ? commission : trade_value * commission_rate();
        if (commission > 0 && actual_sold < qty && qty > 0) {
            actual_commission = commission * (actual_sold / qty);
        }

        cash += trade_value - actual_commission;
        total_commissions += actual_commission;
        total_volume += trade_value;
        total_spread_cost += spread_cost;

        if (sym_pos.count == 0) {
            symbol_active[s] = false;
        }
        return actual_commission;
    }

    // === Target/Stop Checking (template - must be in header) ===
    template <typename OnTargetHit, typename OnStopHit, typename OnTrendExit>
    inline int check_and_close(Symbol s, double current_price, OnTargetHit on_target, OnStopHit on_stop,
                               OnTrendExit on_trend_exit, double pullback_threshold = 0.005) {
        if (s >= MAX_SYMBOLS)
            return 0;

        int closed = 0;
        auto& sym_pos = positions[s];

        for (auto& slot : sym_pos.slots) {
            if (!slot.active)
                continue;

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

    // === Portfolio Value ===
    inline double total_value(const std::array<double, MAX_SYMBOLS>& prices) const {
        double value = cash;
        for (size_t s = 0; s < MAX_SYMBOLS; s++) {
            if (symbol_active[s] && prices[s] > 0) {
                value += positions[s].total_quantity() * prices[s];
            }
        }
        return value;
    }

    inline double total_value(const std::map<Symbol, double>& prices) const {
        double value = cash;
        for (size_t s = 0; s < MAX_SYMBOLS; s++) {
            if (symbol_active[s]) {
                auto it = prices.find(static_cast<Symbol>(s));
                if (it != prices.end()) {
                    value += positions[s].total_quantity() * it->second;
                }
            }
        }
        return value;
    }

    inline int position_count() const {
        int count = 0;
        for (size_t s = 0; s < MAX_SYMBOLS; s++) {
            if (symbol_active[s] && positions[s].count > 0)
                count++;
        }
        return count;
    }

    inline int total_position_slots() const {
        int count = 0;
        for (size_t s = 0; s < MAX_SYMBOLS; s++) {
            count += positions[s].count;
        }
        return count;
    }
};

} // namespace hft::trading
