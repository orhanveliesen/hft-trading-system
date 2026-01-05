#pragma once

#include "types.hpp"
#include "orderbook.hpp"
#include "matching_engine.hpp"
#include "strategy/market_maker.hpp"
#include "strategy/position.hpp"
#include "strategy/risk_manager.hpp"
#include "top_of_book.hpp"
#include <memory>
#include <string>
#include <optional>
#include <unordered_map>

namespace hft {

// Our open order - orders we sent to exchange
struct OurOrder {
    OrderId id;
    Side side;
    Price price;
    Quantity qty;
    Quantity filled_qty = 0;

    Quantity remaining() const { return qty - filled_qty; }
    bool is_filled() const { return filled_qty >= qty; }
};

/**
 * SymbolWorld - All trading context for a single symbol
 *
 * Aggregates related components that were previously scattered across
 * multiple maps. Provides a cohesive interface for symbol-specific operations.
 *
 * Usage:
 *   auto& world = engine.get_symbol_world(symbol);
 *   world.book().add_order(...);
 *   world.config().base_price;
 *   world.position().position();
 */
class SymbolWorld {
public:
    SymbolWorld(Symbol id, const std::string& ticker, const SymbolConfig& config)
        : id_(id)
        , ticker_(ticker)
        , config_(config)
        , book_(std::make_unique<OrderBook>(config.base_price, config.price_range))
        , matching_engine_(std::make_unique<MatchingEngine>(config.base_price, config.price_range))
        , position_(std::make_unique<strategy::PositionTracker>())
    {
        // Initialize market maker if configured
        if (config.enable_market_making) {
            strategy::MarketMakerConfig mm_config;
            mm_config.spread_bps = config.spread_bps;
            mm_config.quote_size = config.quote_size;
            mm_config.max_position = config.max_position;
            market_maker_ = std::make_unique<strategy::MarketMaker>(mm_config);
        }

        // Initialize risk manager
        strategy::RiskConfig risk_config;
        risk_config.max_position = config.max_position;
        risk_config.max_loss = config.max_loss;
        risk_manager_ = std::make_unique<strategy::RiskManager>(risk_config);
    }

    // Non-copyable, movable
    SymbolWorld(const SymbolWorld&) = delete;
    SymbolWorld& operator=(const SymbolWorld&) = delete;
    SymbolWorld(SymbolWorld&&) = default;
    SymbolWorld& operator=(SymbolWorld&&) = default;

    // ========================================
    // Accessors - Direct access to components
    // ========================================

    Symbol id() const { return id_; }
    const std::string& ticker() const { return ticker_; }
    const SymbolConfig& config() const { return config_; }

    // Order book access
    OrderBook& book() { return *book_; }
    const OrderBook& book() const { return *book_; }

    // Matching engine access
    MatchingEngine& matching() { return *matching_engine_; }
    const MatchingEngine& matching() const { return *matching_engine_; }

    // Position tracking
    strategy::PositionTracker& position() { return *position_; }
    const strategy::PositionTracker& position() const { return *position_; }

    // Market maker (may be null if not enabled)
    strategy::MarketMaker* market_maker() { return market_maker_.get(); }
    const strategy::MarketMaker* market_maker() const { return market_maker_.get(); }
    bool has_market_maker() const { return market_maker_ != nullptr; }

    // Risk manager
    strategy::RiskManager& risk() { return *risk_manager_; }
    const strategy::RiskManager& risk() const { return *risk_manager_; }

    // Top of book (L2 - lightweight price levels)
    TopOfBook& top() { return top_of_book_; }
    const TopOfBook& top() const { return top_of_book_; }

    // ========================================
    // Convenience Methods
    // ========================================

    Price best_bid() const { return book_->best_bid(); }
    Price best_ask() const { return book_->best_ask(); }

    Price mid_price() const {
        Price bid = best_bid();
        Price ask = best_ask();
        if (bid == INVALID_PRICE || ask == INVALID_PRICE) {
            return INVALID_PRICE;
        }
        return (bid + ask) / 2;
    }

    Price spread() const {
        Price bid = best_bid();
        Price ask = best_ask();
        if (bid == INVALID_PRICE || ask == INVALID_PRICE) {
            return INVALID_PRICE;
        }
        return ask - bid;
    }

    int64_t position_qty() const { return position_->position(); }
    bool is_flat() const { return position_->is_flat(); }

    // Check if trading is allowed (risk limits)
    bool can_trade(Side side, Quantity size) const {
        return risk_manager_->can_trade(side, size, position_->position());
    }

    bool is_halted() const { return risk_manager_->is_halted(); }

    // Book state (for snapshot sync)
    bool is_book_ready() const { return top_of_book_.is_ready(); }
    BookState book_state() const { return top_of_book_.state(); }

    // Apply snapshot to initialize book
    void apply_snapshot(const L1Snapshot& snap) { top_of_book_.apply_snapshot(snap); }

    template<size_t N>
    void apply_snapshot(const L2Snapshot<N>& snap) { top_of_book_.apply_snapshot(snap); }

    // ========================================
    // Trading Operations
    // ========================================

    // Submit order to matching engine
    void submit_order(OrderId id, Side side, Price price, Quantity qty, TraderId trader = NO_TRADER) {
        matching_engine_->add_order(id, side, price, qty, trader);
    }

    // Cancel order
    bool cancel_order(OrderId id) {
        return matching_engine_->cancel_order(id);
    }

    // Record a fill
    void on_fill(Side side, Quantity qty, Price price) {
        position_->on_fill(side, qty, price);
        // Update P&L in risk manager
        Price mid = mid_price();
        if (mid != INVALID_PRICE) {
            risk_manager_->update_pnl(position_->total_pnl(mid));
        }
    }

    // Get market maker quote (if enabled)
    std::optional<strategy::Quote> get_quote() const {
        if (!market_maker_) return std::nullopt;

        Price mid = mid_price();
        if (mid == INVALID_PRICE) return std::nullopt;

        return market_maker_->generate_quotes(mid, position_->position());
    }

    // ========================================
    // Our Order Tracking
    // ========================================

    // Track an order we sent to exchange
    void track_order(OrderId id, Side side, Price price, Quantity qty) {
        our_orders_[id] = OurOrder{id, side, price, qty, 0};
    }

    // Update fill on our order
    void on_our_fill(OrderId id, Quantity fill_qty) {
        auto it = our_orders_.find(id);
        if (it != our_orders_.end()) {
            it->second.filled_qty += fill_qty;
            if (it->second.is_filled()) {
                our_orders_.erase(it);
            }
        }
    }

    // Remove order (cancelled or fully filled)
    void untrack_order(OrderId id) {
        our_orders_.erase(id);
    }

    // Get all our open orders
    const std::unordered_map<OrderId, OurOrder>& our_orders() const {
        return our_orders_;
    }

    size_t our_order_count() const { return our_orders_.size(); }

private:
    // Identity
    Symbol id_;
    std::string ticker_;
    SymbolConfig config_;

    // Core components
    std::unique_ptr<OrderBook> book_;
    std::unique_ptr<MatchingEngine> matching_engine_;

    // Strategy components
    std::unique_ptr<strategy::PositionTracker> position_;
    std::unique_ptr<strategy::MarketMaker> market_maker_;  // Optional
    std::unique_ptr<strategy::RiskManager> risk_manager_;

    // Our open orders (sent to exchange, awaiting fill/cancel)
    std::unordered_map<OrderId, OurOrder> our_orders_;

    // L2 order book (top 5 levels - lightweight)
    TopOfBook top_of_book_;
};

}  // namespace hft
