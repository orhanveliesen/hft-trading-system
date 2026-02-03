#include "../../include/trading/portfolio.hpp"
#include "../../include/ipc/shared_config.hpp"
#include "../../include/ipc/symbol_config.hpp"

#include <cmath>
#include <chrono>
#include <algorithm>

namespace hft::trading {

// =============================================================================
// OpenPosition Implementation
// =============================================================================

void OpenPosition::clear() {
    entry_price = 0;
    quantity = 0;
    target_price = 0;
    stop_loss_price = 0;
    peak_price = 0;
    timestamp = 0;
    active = false;
}

bool OpenPosition::update_peak_and_check_trend_exit(double current_price, double pullback_pct) {
    if (current_price > peak_price) {
        peak_price = current_price;
    }

    bool in_profit = current_price > entry_price;
    double pullback = (peak_price - current_price) / peak_price;
    return in_profit && pullback >= pullback_pct;
}

// =============================================================================
// SymbolPositions Implementation
// =============================================================================

bool SymbolPositions::add(double entry, double qty, double target, double stop_loss) {
    if (count >= MAX_POSITIONS_PER_SYMBOL) return false;

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

double SymbolPositions::total_quantity() const {
    double total = 0;
    for (const auto& slot : slots) {
        if (slot.active) total += slot.quantity;
    }
    return total;
}

double SymbolPositions::avg_entry() const {
    double total_cost = 0, total_qty = 0;
    for (const auto& slot : slots) {
        if (slot.active) {
            total_cost += slot.entry_price * slot.quantity;
            total_qty += slot.quantity;
        }
    }
    return total_qty > 0 ? total_cost / total_qty : 0;
}

void SymbolPositions::clear_all() {
    for (auto& slot : slots) slot.clear();
    count = 0;
}

// =============================================================================
// Portfolio Implementation
// =============================================================================

// === Config Accessors ===

double Portfolio::target_pct() const {
    return config_ ? config_->target_pct() / 100.0 : config::targets::TARGET_PCT;
}

double Portfolio::stop_pct() const {
    return config_ ? config_->stop_pct() / 100.0 : config::targets::STOP_PCT;
}

double Portfolio::commission_rate() const {
    return config_ ? config_->commission_rate() : config::costs::COMMISSION_PCT;
}

double Portfolio::pullback_pct() const {
    return config_ ? config_->pullback_pct() / 100.0 : config::targets::PULLBACK_PCT;
}

double Portfolio::base_position_pct(const char* symbol) const {
    if (symbol && symbol_configs_) {
        const auto* sym_cfg = symbol_configs_->find(symbol);
        if (sym_cfg && !sym_cfg->use_global_position()) {
            return sym_cfg->base_position_x100 / 10000.0;
        }
    }
    return config_ ? config_->base_position_pct() / 100.0 : config::position::BASE_PCT;
}

double Portfolio::max_position_pct(const char* symbol) const {
    if (symbol && symbol_configs_) {
        const auto* sym_cfg = symbol_configs_->find(symbol);
        if (sym_cfg && !sym_cfg->use_global_position()) {
            return sym_cfg->max_position_x100 / 10000.0;
        }
    }
    return config_ ? config_->max_position_pct() / 100.0 : config::position::MAX_PCT;
}

// === Setters ===

void Portfolio::set_config(const ipc::SharedConfig* cfg) {
    config_ = cfg;
}

void Portfolio::set_symbol_configs(const ipc::SharedSymbolConfigs* cfgs) {
    symbol_configs_ = cfgs;
}

// === Initialization ===

void Portfolio::init(double capital) {
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

double Portfolio::calculate_qty(double price, double available_cash, const char* symbol) const {
    if (price <= 0) return 0;

    double position_value = available_cash * base_position_pct(symbol);
    double max_value = available_cash * max_position_pct(symbol);
    position_value = std::min(position_value, max_value);

    double qty = position_value / price;
    qty = std::floor(qty * 1e8) / 1e8;  // Binance precision

    if (qty * price < 10.0) return 0;  // Minimum order size
    return qty;
}

// === Position Queries ===

double Portfolio::get_holding(Symbol s) const {
    if (s >= MAX_SYMBOLS) return 0;
    return positions[s].total_quantity();
}

bool Portfolio::can_buy(double price, double qty) const {
    double available = cash - pending_cash;
    return available >= price * qty;
}

bool Portfolio::can_sell(Symbol s, double qty) const {
    return get_holding(s) >= qty;
}

double Portfolio::avg_entry_price(Symbol s) const {
    if (s >= MAX_SYMBOLS) return 0;
    return positions[s].avg_entry();
}

// === Cash Reservation ===

void Portfolio::reserve_cash(double amount) {
    pending_cash += amount;
}

void Portfolio::release_reserved_cash(double amount) {
    pending_cash -= amount;
    if (pending_cash < 0) pending_cash = 0;
}

// === Trading Operations ===

double Portfolio::buy(Symbol s, double price, double qty, double spread_cost, double commission) {
    if (qty <= 0 || price <= 0) return 0;
    if (s >= MAX_SYMBOLS) return 0;

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

double Portfolio::sell(Symbol s, double price, double qty, double spread_cost, double commission) {
    if (qty <= 0 || price <= 0) return 0;
    if (s >= MAX_SYMBOLS) return 0;

    double remaining = qty;
    auto& sym_pos = positions[s];
    double actual_sold = 0;

    for (auto& slot : sym_pos.slots) {
        if (!slot.active || remaining <= 0) continue;

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

// === Portfolio Value ===

double Portfolio::total_value(const std::array<double, MAX_SYMBOLS>& prices) const {
    double value = cash;
    for (size_t s = 0; s < MAX_SYMBOLS; s++) {
        if (symbol_active[s] && prices[s] > 0) {
            value += positions[s].total_quantity() * prices[s];
        }
    }
    return value;
}

double Portfolio::total_value(const std::map<Symbol, double>& prices) const {
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

int Portfolio::position_count() const {
    int count = 0;
    for (size_t s = 0; s < MAX_SYMBOLS; s++) {
        if (symbol_active[s] && positions[s].count > 0) count++;
    }
    return count;
}

int Portfolio::total_position_slots() const {
    int count = 0;
    for (size_t s = 0; s < MAX_SYMBOLS; s++) {
        count += positions[s].count;
    }
    return count;
}

} // namespace hft::trading
