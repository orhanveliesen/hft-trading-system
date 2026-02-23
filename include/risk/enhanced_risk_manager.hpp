#pragma once

#include "../types.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft {
namespace risk {

// Symbol index type for hot path
using SymbolIndex = uint32_t;
constexpr SymbolIndex INVALID_SYMBOL_INDEX = std::numeric_limits<SymbolIndex>::max();

// Price scaling factor (prices are in basis points: 1 dollar = 10000)
constexpr int64_t PRICE_SCALE = 10000;

/**
 * EnhancedRiskConfig - Complete risk configuration
 *
 * All monetary limits are expressed as percentages of initial_capital.
 * This ensures consistent scaling regardless of capital size.
 */
struct EnhancedRiskConfig {
    // Initial capital (required - all percentage limits are calculated from this)
    Capital initial_capital = 0; // Must be set!

    // Daily loss limit as percentage of initial capital
    double daily_loss_limit_pct = 0.02; // 2% daily loss limit (0.02 = 2%)

    // Max drawdown from peak as percentage
    double max_drawdown_pct = 0.10; // 10% max drawdown (0.10 = 10%)

    // Max total notional exposure as percentage of initial capital
    double max_notional_pct = 1.0; // 100% of capital (1.0 = 100%)

    // Order limits (quantity-based, not percentage)
    Quantity max_order_size = 10000; // Max single order size

    // Position limits (quantity-based, not percentage)
    Position max_total_position = 100000; // Max total absolute position
};

/**
 * Per-symbol risk limits
 */
struct SymbolRiskLimit {
    Position max_position = 0; // 0 = no limit
    Notional max_notional = 0; // 0 = no limit
};

/**
 * Per-symbol risk state (updated on each fill)
 */
struct SymbolRiskState {
    Position position = 0; // Current net position (negative = short)
    Notional notional = 0; // Current notional (abs(position) * last_price)
    Price last_price = 0;  // Last fill price (for notional calc)

    void reset() {
        position = 0;
        notional = 0;
        last_price = 0;
    }
};

/**
 * Global risk state snapshot
 */
struct RiskState {
    PnL current_pnl;
    PnL daily_pnl;
    Capital peak_equity;
    Notional total_notional;
    double current_drawdown_pct;
    bool can_trade;
    bool daily_limit_breached;
    bool drawdown_breached;
};

/**
 * EnhancedRiskManager - Production-grade risk management (Hybrid Design)
 *
 * Features:
 * - Daily P&L limit with automatic halt
 * - Max drawdown from peak with automatic halt
 * - Per-symbol position and notional limits
 * - Global notional exposure limit
 * - Max order size limit
 *
 * Design (Hybrid):
 * - Config/Cold path: String-based symbol names for readability
 * - Hot path: Dense array indexing for O(1) with minimal cycles
 * - register_symbol() returns SymbolIndex for caller to cache
 * - check_order(SymbolIndex, ...) for hot path (~4-5 cycles)
 * - check_order(string, ...) convenience overload for non-critical paths
 */
class EnhancedRiskManager {
public:
    explicit EnhancedRiskManager(const EnhancedRiskConfig& config = EnhancedRiskConfig{})
        : config_(config), initial_capital_(config.initial_capital), current_pnl_(0),
          peak_equity_(config.initial_capital), daily_start_pnl_(0), total_notional_(0), daily_limit_breached_(false),
          drawdown_breached_(false), halted_(false) {}

    // ========================================
    // Symbol Registration (Cold Path)
    // ========================================

    /**
     * Reserve capacity for expected number of symbols
     * Call once at startup to avoid reallocations
     */
    void reserve_symbols(size_t count) {
        limits_.reserve(count);
        states_.reserve(count);
        index_to_symbol_.reserve(count);
    }

    /**
     * Register a symbol and get its index for hot path usage
     * Returns SymbolIndex that caller should cache
     *
     * @param symbol String symbol name (e.g., "AAPL", "BTCUSDT")
     * @param max_position Max position limit (0 = no limit)
     * @param max_notional Max notional limit (0 = no limit)
     * @return SymbolIndex for hot path operations
     */
    SymbolIndex register_symbol(const std::string& symbol, Position max_position = 0, Notional max_notional = 0) {
        // Check if already registered
        auto it = symbol_to_index_.find(symbol);
        if (it != symbol_to_index_.end()) {
            // Update limits for existing symbol
            limits_[it->second] = {max_position, max_notional};
            return it->second;
        }

        // Register new symbol
        SymbolIndex index = static_cast<SymbolIndex>(states_.size());
        symbol_to_index_[symbol] = index;
        index_to_symbol_.push_back(symbol);
        states_.push_back(SymbolRiskState{});
        limits_.push_back({max_position, max_notional});

        return index;
    }

    /**
     * Get symbol index by name (cold path lookup)
     * Returns INVALID_SYMBOL_INDEX if not found
     */
    SymbolIndex get_symbol_index(const std::string& symbol) const {
        auto it = symbol_to_index_.find(symbol);
        if (it == symbol_to_index_.end()) {
            return INVALID_SYMBOL_INDEX;
        }
        return it->second;
    }

    /**
     * Get symbol name by index (for logging/debug)
     */
    const std::string& get_symbol_name(SymbolIndex index) const {
        static const std::string empty;
        if (index >= index_to_symbol_.size())
            return empty;
        return index_to_symbol_[index];
    }

    /**
     * Update limits for existing symbol (cold path)
     */
    void set_symbol_limit(const std::string& symbol, Position max_position, Notional max_notional) {
        register_symbol(symbol, max_position, max_notional);
    }

    // ========================================
    // P&L Updates
    // ========================================

    /**
     * Update current P&L and check limits
     * Called on every fill or periodically with mark-to-market
     */
    void update_pnl(PnL pnl) {
        current_pnl_ = pnl;

        Capital current_equity = initial_capital_ + current_pnl_;

        // Update peak equity (only goes up)
        if (current_equity > peak_equity_) {
            peak_equity_ = current_equity;
        }

        // Check daily loss limit (percentage of initial capital)
        PnL daily_pnl = current_pnl_ - daily_start_pnl_;
        PnL daily_loss_limit = static_cast<PnL>(initial_capital_ * config_.daily_loss_limit_pct);
        if (daily_pnl < -daily_loss_limit) {
            daily_limit_breached_ = true;
            halted_ = true;
        }

        // Check drawdown from peak
        if (peak_equity_ > 0) {
            double drawdown = static_cast<double>(peak_equity_ - current_equity) / static_cast<double>(peak_equity_);
            if (drawdown > config_.max_drawdown_pct) {
                drawdown_breached_ = true;
                halted_ = true;
            }
        }
    }

    /**
     * Call at start of new trading day
     * Resets daily P&L tracking but keeps drawdown state
     */
    void new_trading_day() {
        daily_start_pnl_ = current_pnl_;
        daily_limit_breached_ = false;

        // Only reset halt if drawdown is not breached
        if (!drawdown_breached_) {
            halted_ = false;
        }
    }

    // ========================================
    // Pre-Trade Risk Checks - HOT PATH
    // ========================================

    /**
     * Check if an order is allowed (HOT PATH - use this!)
     *
     * @param symbol_index Index from register_symbol() - caller must cache this
     * @return true if order passes all risk checks
     *
     * Performance: ~4-5 cycles (array indexing only)
     */
    __attribute__((always_inline)) bool check_order(SymbolIndex symbol_index, Side side, Quantity qty,
                                                    Price price) const {
        // Global halt check
        if (halted_) [[unlikely]] {
            return false;
        }

        // Order size check
        if (qty > config_.max_order_size) [[unlikely]] {
            return false;
        }

        // Symbol-specific checks (direct array access)
        if (symbol_index < limits_.size()) [[likely]] {
            const auto& limit = limits_[symbol_index];
            const auto& state = states_[symbol_index];

            // Position limit check
            if (limit.max_position > 0) {
                Position new_position = state.position;
                if (side == Side::Buy) {
                    new_position += static_cast<Position>(qty);
                } else {
                    new_position -= static_cast<Position>(qty);
                }

                if (std::abs(new_position) > limit.max_position) [[unlikely]] {
                    return false;
                }
            }

            // Notional limit check
            if (limit.max_notional > 0 && price > 0) {
                Notional order_notional = static_cast<Notional>(qty) * price / PRICE_SCALE;
                Notional new_notional = state.notional + order_notional;

                if (new_notional > limit.max_notional) [[unlikely]] {
                    return false;
                }
            }
        }

        // Global notional check (percentage of initial capital)
        if (config_.max_notional_pct > 0) {
            Notional max_notional = static_cast<Notional>(initial_capital_ * config_.max_notional_pct);
            Notional order_notional = static_cast<Notional>(qty) * price / PRICE_SCALE;
            if (total_notional_ + order_notional > max_notional) [[unlikely]] {
                return false;
            }
        }

        return true;
    }

    /**
     * Check order by symbol name (convenience, NOT for hot path)
     * Use check_order(SymbolIndex, ...) for performance-critical code
     */
    bool check_order(const std::string& symbol, Side side, Quantity qty, Price price) const {
        SymbolIndex idx = get_symbol_index(symbol);
        if (idx == INVALID_SYMBOL_INDEX) {
            // Unknown symbol - allow if no specific limits
            return !halted_ && qty <= config_.max_order_size;
        }
        return check_order(idx, side, qty, price);
    }

    /**
     * Quick check if trading is allowed at all
     */
    __attribute__((always_inline)) bool can_trade() const { return !halted_; }

    // ========================================
    // Fill Updates - HOT PATH
    // ========================================

    /**
     * Update state after a fill (HOT PATH)
     */
    __attribute__((always_inline)) void on_fill(SymbolIndex symbol_index, Side side, Quantity qty, Price price) {
        if (symbol_index >= states_.size()) [[unlikely]]
            return;

        auto& state = states_[symbol_index];

        // Update position
        Position signed_qty = static_cast<Position>(qty);
        if (side == Side::Buy) {
            state.position += signed_qty;
        } else {
            state.position -= signed_qty;
        }

        // Update notional
        state.last_price = price;
        state.notional = std::abs(state.position) * price / PRICE_SCALE;

        // Update total notional (could be optimized with delta tracking)
        recalculate_total_notional();
    }

    /**
     * Update state by symbol name (convenience, NOT for hot path)
     */
    void on_fill(const std::string& symbol, Side side, Quantity qty, Price price) {
        SymbolIndex idx = get_symbol_index(symbol);
        if (idx == INVALID_SYMBOL_INDEX) {
            idx = register_symbol(symbol); // Auto-register
        }
        on_fill(idx, side, qty, price);
    }

    // ========================================
    // State Queries
    // ========================================

    bool is_halted() const { return halted_; }
    bool is_daily_limit_breached() const { return daily_limit_breached_; }
    bool is_drawdown_breached() const { return drawdown_breached_; }

    PnL current_pnl() const { return current_pnl_; }
    Capital peak_equity() const { return peak_equity_; }
    Notional total_notional() const { return total_notional_; }

    PnL daily_pnl() const { return current_pnl_ - daily_start_pnl_; }

    double current_drawdown_pct() const {
        if (peak_equity_ <= 0)
            return 0.0;
        Capital current_equity = initial_capital_ + current_pnl_;
        return static_cast<double>(peak_equity_ - current_equity) / static_cast<double>(peak_equity_);
    }

    Position symbol_position(SymbolIndex index) const {
        if (index >= states_.size())
            return 0;
        return states_[index].position;
    }

    Position symbol_position(const std::string& symbol) const {
        SymbolIndex idx = get_symbol_index(symbol);
        if (idx == INVALID_SYMBOL_INDEX)
            return 0;
        return states_[idx].position;
    }

    Notional symbol_notional(SymbolIndex index) const {
        if (index >= states_.size())
            return 0;
        return states_[index].notional;
    }

    RiskState build_state() const {
        RiskState state;
        state.current_pnl = current_pnl_;
        state.daily_pnl = daily_pnl();
        state.peak_equity = peak_equity_;
        state.total_notional = total_notional_;
        state.current_drawdown_pct = current_drawdown_pct();
        state.can_trade = !halted_;
        state.daily_limit_breached = daily_limit_breached_;
        state.drawdown_breached = drawdown_breached_;
        return state;
    }

    // ========================================
    // Control
    // ========================================

    void halt() { halted_ = true; }

    void reset_halt() {
        halted_ = false;
        daily_limit_breached_ = false;
        drawdown_breached_ = false;
    }

    void reset_all() {
        current_pnl_ = 0;
        peak_equity_ = initial_capital_;
        daily_start_pnl_ = 0;
        total_notional_ = 0;
        daily_limit_breached_ = false;
        drawdown_breached_ = false;
        halted_ = false;

        for (auto& state : states_) {
            state.reset();
        }
    }

    const EnhancedRiskConfig& config() const { return config_; }

    // ========================================
    // Symbol enumeration
    // ========================================

    size_t symbol_count() const { return states_.size(); }

    const std::vector<std::string>& symbols() const { return index_to_symbol_; }

private:
    void recalculate_total_notional() {
        Notional total = 0;
        for (const auto& state : states_) {
            total += state.notional;
        }
        total_notional_ = total;
    }

    EnhancedRiskConfig config_;

    // Capital and P&L tracking
    Capital initial_capital_;
    PnL current_pnl_;
    Capital peak_equity_;
    PnL daily_start_pnl_;
    Notional total_notional_;

    // Risk flags
    bool daily_limit_breached_;
    bool drawdown_breached_;
    bool halted_;

    // Per-symbol data - HOT PATH (dense arrays for cache efficiency)
    std::vector<SymbolRiskLimit> limits_;
    std::vector<SymbolRiskState> states_;

    // Symbol mapping - COLD PATH (string lookups)
    std::unordered_map<std::string, SymbolIndex> symbol_to_index_;
    std::vector<std::string> index_to_symbol_;
};

} // namespace risk
} // namespace hft
