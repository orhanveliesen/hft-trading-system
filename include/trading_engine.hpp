#pragma once

#include "account/account.hpp"
#include "concepts.hpp"
#include "order_sender.hpp"
#include "strategy/halt_manager.hpp"
#include "symbol_config.hpp"
#include "symbol_world.hpp"
#include "types.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft {

/**
 * Trading Engine - Manages multiple symbols via SymbolWorld
 *
 * Template parameter Sender must satisfy concepts::OrderSender.
 * See concepts.hpp for the full concept definition.
 *
 * Before (scattered data):
 *   orderbooks_[symbol]->add_order(...);
 *   configs_[symbol].base_price;
 *
 * After (unified access):
 *   auto& world = get_symbol_world(symbol);
 *   world.book().add_order(...);
 *   world.config().base_price;
 */
template <concepts::OrderSender Sender>
class TradingEngine {
public:
    explicit TradingEngine(Sender& sender) : sender_(sender), next_symbol_id_(1) { setup_halt_manager(); }

    // ========================================
    // Symbol Management
    // ========================================

    // Add a symbol with configuration
    Symbol add_symbol(const SymbolConfig& config) {
        Symbol id = next_symbol_id_++;

        auto world = std::make_unique<SymbolWorld>(id, config.symbol, config);
        worlds_[id] = std::move(world);
        ticker_to_id_[config.symbol] = id;

        return id;
    }

    // Check if symbol exists
    bool has_symbol(Symbol id) const { return worlds_.find(id) != worlds_.end(); }

    bool has_symbol(const std::string& ticker) const { return ticker_to_id_.find(ticker) != ticker_to_id_.end(); }

    size_t symbol_count() const { return worlds_.size(); }

    // ========================================
    // SymbolWorld Access - The Clean API
    // ========================================

    // Get SymbolWorld by ID (preferred - O(1))
    SymbolWorld* get_symbol_world(Symbol id) {
        auto it = worlds_.find(id);
        return it != worlds_.end() ? it->second.get() : nullptr;
    }

    const SymbolWorld* get_symbol_world(Symbol id) const {
        auto it = worlds_.find(id);
        return it != worlds_.end() ? it->second.get() : nullptr;
    }

    // Get SymbolWorld by ticker (convenience - O(1) amortized)
    SymbolWorld* get_symbol_world(const std::string& ticker) {
        auto it = ticker_to_id_.find(ticker);
        if (it == ticker_to_id_.end())
            return nullptr;
        return get_symbol_world(it->second);
    }

    const SymbolWorld* get_symbol_world(const std::string& ticker) const {
        auto it = ticker_to_id_.find(ticker);
        if (it == ticker_to_id_.end())
            return nullptr;
        return get_symbol_world(it->second);
    }

    // Lookup symbol ID from ticker
    std::optional<Symbol> lookup_symbol(const std::string& ticker) const {
        auto it = ticker_to_id_.find(ticker);
        if (it == ticker_to_id_.end())
            return std::nullopt;
        return it->second;
    }

    // ========================================
    // Legacy API (for backward compatibility)
    // ========================================

    OrderBook* get_orderbook(Symbol id) {
        auto* world = get_symbol_world(id);
        return world ? &world->book() : nullptr;
    }

    OrderBook* get_orderbook(const std::string& ticker) {
        auto* world = get_symbol_world(ticker);
        return world ? &world->book() : nullptr;
    }

    // ========================================
    // Iteration
    // ========================================

    // Iterate over all symbols
    template <typename Func>
    void for_each_symbol(Func&& func) {
        for (auto& [id, world] : worlds_) {
            func(*world);
        }
    }

    template <typename Func>
    void for_each_symbol(Func&& func) const {
        for (const auto& [id, world] : worlds_) {
            func(*world);
        }
    }

    // ========================================
    // Message Handlers (filter by symbol)
    // ========================================

    void on_add_order(Symbol symbol, OrderId id, Side side, Price price, Quantity quantity,
                      TraderId /*trader*/ = NO_TRADER) {
        if (auto* world = get_symbol_world(symbol)) {
            world->book().add_order(id, side, price, quantity);
        }
    }

    void on_add_order(const std::string& ticker, OrderId id, Side side, Price price, Quantity quantity,
                      TraderId /*trader*/ = NO_TRADER) {
        if (auto* world = get_symbol_world(ticker)) {
            world->book().add_order(id, side, price, quantity);
        }
    }

    void on_cancel_order(Symbol symbol, OrderId id) {
        if (auto* world = get_symbol_world(symbol)) {
            world->book().cancel_order(id);
        }
    }

    void on_cancel_order(const std::string& ticker, OrderId id) {
        if (auto* world = get_symbol_world(ticker)) {
            world->book().cancel_order(id);
        }
    }

    void on_execute_order(Symbol symbol, OrderId id, Quantity quantity) {
        if (auto* world = get_symbol_world(symbol)) {
            world->book().execute_order(id, quantity);
        }
    }

    void on_execute_order(const std::string& ticker, OrderId id, Quantity quantity) {
        if (auto* world = get_symbol_world(ticker)) {
            world->book().execute_order(id, quantity);
        }
    }

    // ========================================
    // Halt Management - HaltManager owns everything
    // ========================================

    // Check if trading is allowed (hot path)
    __attribute__((always_inline)) bool can_trade() const { return halt_manager_.can_trade(); }

    // Get halt manager for configuration or direct control
    strategy::HaltManager& halt_manager() { return halt_manager_; }
    const strategy::HaltManager& halt_manager() const { return halt_manager_; }

    // Get account manager
    account::AccountManager& account() { return account_manager_; }
    const account::AccountManager& account() const { return account_manager_; }

    // Pre-trade check: can we afford this order?
    account::OrderCost check_order(Symbol symbol, Side side, Quantity qty) {
        auto* world = get_symbol_world(symbol);
        if (!world) {
            account::OrderCost cost;
            cost.can_afford = false;
            cost.reject_reason = "Unknown symbol";
            return cost;
        }

        // Use mid price for cost estimate
        Price price = world->top().mid_price();
        if (price == 0) {
            // Fall back to best bid/ask
            price = (side == Side::Buy) ? world->top().best_ask() : world->top().best_bid();
        }

        return account_manager_.calculate_order_cost(side, qty, price);
    }

    // ========================================
    // Order Sending (direct, zero-cost)
    // ========================================

    // Send order via OrderSender (hot path)
    __attribute__((always_inline)) bool send_order(Symbol symbol, Side side, Quantity qty, bool is_market) {
        return sender_.send_order(symbol, side, qty, is_market);
    }

    // Cancel order via OrderSender (hot path)
    __attribute__((always_inline)) bool cancel_order(Symbol symbol, OrderId order_id) {
        return sender_.cancel_order(symbol, order_id);
    }

    // Get the underlying sender (for advanced use)
    Sender& sender() { return sender_; }
    const Sender& sender() const { return sender_; }

    // Convenience method to trigger halt
    bool halt(strategy::HaltReason reason, const std::string& message = "") {
        return halt_manager_.halt(reason, message);
    }

    // Get all positions for monitoring
    std::vector<strategy::PositionInfo> get_all_positions() const {
        std::vector<strategy::PositionInfo> positions;
        positions.reserve(worlds_.size());

        for (const auto& [id, world] : worlds_) {
            int64_t pos = world->position().position();
            if (pos != 0) {
                strategy::PositionInfo info;
                info.symbol = id;
                info.ticker = world->ticker();
                info.position = pos;
                info.last_price = world->book().best_bid();
                positions.push_back(info);
            }
        }
        return positions;
    }

private:
    void setup_halt_manager() {
        // Wire up HaltManager callbacks to TradingEngine

        // 1. Get positions callback
        halt_manager_.set_get_positions_callback([this]() { return get_all_positions(); });

        // 2. Cancel all orders callback
        halt_manager_.set_cancel_all_callback([this]() { cancel_all_orders(); });

        // 3. Send order callback - uses template sender (no std::function on hot path)
        halt_manager_.set_send_order_callback([this](Symbol symbol, Side side, Quantity qty, bool is_market) {
            return sender_.send_order(symbol, side, qty, is_market);
        });
    }

    // Cancel all open orders across all symbols
    void cancel_all_orders() {
        for (auto& [id, world] : worlds_) {
            for (const auto& [order_id, order] : world->our_orders()) {
                sender_.cancel_order(id, order_id);
            }
        }
    }

    // Order sender (template-based, zero overhead)
    Sender& sender_;

    // Single map: Symbol ID -> SymbolWorld (all data in one place)
    std::unordered_map<Symbol, std::unique_ptr<SymbolWorld>> worlds_;

    // Ticker -> Symbol ID mapping (for ticker-based lookups)
    std::unordered_map<std::string, Symbol> ticker_to_id_;

    // Auto-incrementing symbol ID
    Symbol next_symbol_id_;

    // Halt management - HaltManager owns the halt logic
    strategy::HaltManager halt_manager_;

    // Account management
    account::AccountManager account_manager_;
};

// Type alias for convenience
using DefaultTradingEngine = TradingEngine<NullOrderSender>;

} // namespace hft
