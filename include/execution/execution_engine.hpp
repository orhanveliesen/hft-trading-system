#pragma once

#include "../types.hpp"
#include "../strategy/istrategy.hpp"
#include <vector>
#include <functional>
#include <cstdint>

namespace hft {
namespace execution {

using namespace strategy;

// =============================================================================
// Order Types for Execution
// =============================================================================

enum class OrderType : uint8_t {
    Market,
    Limit
};

struct PendingOrder {
    uint64_t order_id = 0;
    Symbol symbol = 0;
    Side side = Side::Buy;
    double quantity = 0;  // double for fractional crypto quantities
    Price limit_price = 0;
    Price expected_fill_price = 0;  // For slippage tracking
    uint64_t submit_time_ns = 0;
    bool active = false;

    void clear() {
        order_id = 0;
        symbol = 0;
        side = Side::Buy;
        quantity = 0;
        limit_price = 0;
        expected_fill_price = 0;
        submit_time_ns = 0;
        active = false;
    }
};

// =============================================================================
// Execution Configuration
// =============================================================================

struct ExecutionConfig {
    // Order type decision thresholds
    double wide_spread_threshold_bps = 10.0;  // Above this, prefer limit
    double urgency_spread_threshold_bps = 5.0; // Below this, market is OK

    // Signal strength mapping
    bool strong_signal_uses_market = true;    // Strong signals → Market
    bool weak_signal_uses_limit = true;       // Weak signals → Limit

    // Regime-based overrides
    bool high_vol_uses_market = true;         // High vol → Market (spread might widen)
    bool ranging_prefers_limit = true;        // Ranging → Limit (save costs)

    // Limit order settings
    double limit_offset_bps = 2.0;            // How far inside spread to place limit
    uint64_t limit_timeout_ns = 5'000'000'000; // 5 seconds before cancel

    // Tracking
    size_t max_pending_orders = 64;
};

// =============================================================================
// Exchange Interface (abstract)
// =============================================================================

/**
 * IExchangeAdapter - Interface that ExecutionEngine uses to send orders
 *
 * This allows ExecutionEngine to work with both paper and real exchanges.
 *
 * NOTE: qty parameter is double (not Quantity/uint32_t) because crypto trading
 * uses fractional quantities (e.g., 0.01 BTC). Using uint32_t would truncate
 * these to 0.
 */
class IExchangeAdapter {
public:
    virtual ~IExchangeAdapter() = default;

    /// Send market order, returns order ID (0 on failure)
    virtual uint64_t send_market_order(
        Symbol symbol, Side side, double qty, Price expected_price
    ) = 0;

    /// Send limit order, returns order ID (0 on failure)
    virtual uint64_t send_limit_order(
        Symbol symbol, Side side, double qty, Price limit_price
    ) = 0;

    /// Cancel order by ID
    virtual bool cancel_order(uint64_t order_id) = 0;

    /// Check if order is still pending
    virtual bool is_order_pending(uint64_t order_id) const = 0;

    /// Is this a paper exchange?
    virtual bool is_paper() const = 0;
};

// =============================================================================
// Execution Engine
// =============================================================================

/**
 * ExecutionEngine - Converts strategy signals to exchange orders
 *
 * Responsibilities:
 * 1. Decide order type (Limit vs Market) based on:
 *    - Strategy preference
 *    - Signal strength
 *    - Market regime
 *    - Spread width
 *
 * 2. Calculate order parameters:
 *    - Quantity (from signal)
 *    - Price (for limits)
 *    - Expected fill price (for slippage tracking)
 *
 * 3. Track pending orders:
 *    - Store limit orders
 *    - Cancel stale orders
 *    - Match fills
 */
class ExecutionEngine {
public:
    // Callbacks use double qty for fractional crypto quantities
    using OrderCallback = std::function<void(uint64_t order_id, Symbol symbol, Side side, double qty, Price price, OrderType type)>;
    using FillCallback = std::function<void(uint64_t order_id, Symbol symbol, Side side, double qty, Price price, double slippage)>;
    using PositionCallback = std::function<double(Symbol symbol)>;  // Returns current position for symbol

    // Minimum position threshold - below this is considered zero (dust)
    static constexpr double MIN_POSITION_THRESHOLD = 0.0001;

    explicit ExecutionEngine(const ExecutionConfig& config = {})
        : config_(config)
    {
        pending_orders_.resize(config.max_pending_orders);
    }

    void set_exchange(IExchangeAdapter* exchange) {
        exchange_ = exchange;
    }

    void set_order_callback(OrderCallback cb) {
        on_order_ = std::move(cb);
    }

    void set_fill_callback(FillCallback cb) {
        on_fill_ = std::move(cb);
    }

    void set_position_callback(PositionCallback cb) {
        get_position_ = std::move(cb);
    }

    // =========================================================================
    // Main Execution Method
    // =========================================================================

    /**
     * Execute a signal - convert to order and send to exchange
     *
     * @param symbol Symbol to trade
     * @param signal Strategy signal
     * @param market Current market data
     * @param regime Current market regime
     * @return Order ID if sent, 0 if not executed
     */
    uint64_t execute(
        Symbol symbol,
        const Signal& signal,
        const MarketSnapshot& market,
        MarketRegime regime
    ) {
        if (!signal.is_actionable() || !exchange_) {
            return 0;
        }

        // Determine order type
        OrderType order_type = decide_order_type(signal, market, regime);

        // Determine side
        Side side = signal.is_buy() ? Side::Buy : Side::Sell;

        // Calculate prices
        Price expected_price = (side == Side::Buy) ? market.ask : market.bid;
        Price limit_price = calculate_limit_price(signal, market, side);

        // Use quantity directly (double, for fractional crypto quantities)
        double qty = signal.suggested_qty;

        // CRITICAL: For SELL orders, check position to prevent overselling
        if (side == Side::Sell && get_position_) {
            double current_position = get_position_(symbol);

            // If position is dust (below threshold), reject the order
            if (current_position < MIN_POSITION_THRESHOLD) {
                return 0;  // No position to sell
            }

            // Limit qty to available position
            if (qty > current_position) {
                qty = current_position;
            }
        }

        // Send order
        uint64_t order_id = 0;
        if (order_type == OrderType::Market) {
            order_id = exchange_->send_market_order(symbol, side, qty, expected_price);
        } else {
            order_id = exchange_->send_limit_order(symbol, side, qty, limit_price);
            if (order_id > 0) {
                track_pending_order(order_id, symbol, side, qty, limit_price, expected_price);
            }
        }

        // Notify callback
        if (order_id > 0 && on_order_) {
            Price order_price = (order_type == OrderType::Market) ? expected_price : limit_price;
            on_order_(order_id, symbol, side, qty, order_price, order_type);
        }

        return order_id;
    }

    // =========================================================================
    // Pending Order Management
    // =========================================================================

    /// Called when a fill occurs - calculate slippage and notify
    void on_fill(uint64_t order_id, Symbol symbol, Side side, double qty, Price fill_price) {
        double slippage = 0;

        // Find pending order to calculate slippage
        for (auto& po : pending_orders_) {
            if (po.active && po.order_id == order_id) {
                // Slippage = difference from expected
                // For buys: positive slippage = paid more = bad
                // For sells: negative slippage = received less = bad
                if (side == Side::Buy) {
                    slippage = static_cast<double>(fill_price - po.expected_fill_price);
                } else {
                    slippage = static_cast<double>(po.expected_fill_price - fill_price);
                }
                po.clear();  // Order filled, remove from pending
                break;
            }
        }

        if (on_fill_) {
            on_fill_(order_id, symbol, side, qty, fill_price, slippage);
        }
    }

    /// Cancel stale pending orders (call periodically)
    void cancel_stale_orders(uint64_t current_time_ns) {
        for (auto& po : pending_orders_) {
            if (po.active) {
                uint64_t age = current_time_ns - po.submit_time_ns;
                if (age > config_.limit_timeout_ns) {
                    if (exchange_->cancel_order(po.order_id)) {
                        po.clear();
                    }
                }
            }
        }
    }

    /// Get count of pending orders
    size_t pending_order_count() const {
        size_t count = 0;
        for (const auto& po : pending_orders_) {
            if (po.active) count++;
        }
        return count;
    }

    // =========================================================================
    // Configuration
    // =========================================================================

    const ExecutionConfig& config() const { return config_; }
    void set_config(const ExecutionConfig& config) { config_ = config; }

private:
    ExecutionConfig config_;
    IExchangeAdapter* exchange_ = nullptr;
    std::vector<PendingOrder> pending_orders_;
    OrderCallback on_order_;
    FillCallback on_fill_;
    PositionCallback get_position_;

    /**
     * Decide whether to use Market or Limit order
     */
    OrderType decide_order_type(
        const Signal& signal,
        const MarketSnapshot& market,
        MarketRegime regime
    ) {
        // 1. Check strategy preference
        if (signal.order_pref == OrderPreference::Market) {
            return OrderType::Market;
        }
        if (signal.order_pref == OrderPreference::Limit) {
            return OrderType::Limit;
        }

        // 2. Signal strength based decision
        if (config_.strong_signal_uses_market && signal.strength >= SignalStrength::Strong) {
            return OrderType::Market;  // Strong = urgent, don't miss
        }
        if (config_.weak_signal_uses_limit && signal.strength <= SignalStrength::Weak) {
            return OrderType::Limit;  // Weak = passive, save costs
        }

        // 3. Regime based decision
        if (config_.high_vol_uses_market && regime == MarketRegime::HighVolatility) {
            return OrderType::Market;  // Vol = spread might widen, execute now
        }
        if (config_.ranging_prefers_limit && regime == MarketRegime::Ranging) {
            return OrderType::Limit;  // Ranging = stable, save costs
        }

        // 4. Spread based decision
        double spread_bps = market.spread_bps();
        if (spread_bps > config_.wide_spread_threshold_bps) {
            return OrderType::Limit;  // Wide spread = use limit to save
        }
        if (spread_bps < config_.urgency_spread_threshold_bps) {
            return OrderType::Market;  // Tight spread = market is cheap
        }

        // 5. Default: Market for medium signals, regime unknown
        return OrderType::Market;
    }

    /**
     * Calculate limit price based on signal and market
     */
    Price calculate_limit_price(
        const Signal& signal,
        const MarketSnapshot& market,
        Side side
    ) {
        // If signal specifies a price, use it
        if (signal.limit_price > 0) {
            return signal.limit_price;
        }

        // Calculate offset into spread
        Price spread = market.spread();
        Price offset = static_cast<Price>(spread * config_.limit_offset_bps / 100.0);

        if (side == Side::Buy) {
            // Buy: place limit above bid, but below mid
            return market.bid + offset;
        } else {
            // Sell: place limit below ask, but above mid
            return market.ask - offset;
        }
    }

    /**
     * Track a pending limit order
     */
    void track_pending_order(
        uint64_t order_id,
        Symbol symbol,
        Side side,
        double qty,
        Price limit_price,
        Price expected_price
    ) {
        // Find free slot
        for (auto& po : pending_orders_) {
            if (!po.active) {
                po.order_id = order_id;
                po.symbol = symbol;
                po.side = side;
                po.quantity = qty;
                po.limit_price = limit_price;
                po.expected_fill_price = expected_price;
                po.submit_time_ns = now_ns();
                po.active = true;
                return;
            }
        }
        // No free slot - should not happen if max_pending_orders is set correctly
    }

    uint64_t now_ns() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()
        ).count();
    }
};

// =============================================================================
// Helper: Order type string conversion
// =============================================================================

inline const char* order_type_str(OrderType type) {
    switch (type) {
        case OrderType::Market: return "MARKET";
        case OrderType::Limit: return "LIMIT";
        default: return "UNKNOWN";
    }
}

}  // namespace execution
}  // namespace hft
