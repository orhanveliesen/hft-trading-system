#pragma once

#include "../types.hpp"
#include "../order_sender.hpp"
#include "../strategy/regime_detector.hpp"
#include "../strategy/adaptive_strategy.hpp"
#include "../logging/async_logger.hpp"
#include <vector>
#include <unordered_map>
#include <queue>
#include <functional>
#include <chrono>
#include <random>
#include <cmath>

namespace hft {
namespace paper {

/**
 * Paper Trade Status
 */
enum class FillStatus : uint8_t {
    Pending,
    PartialFill,
    Filled,
    Cancelled,
    Rejected
};

/**
 * Paper Order
 */
struct PaperOrder {
    OrderId id;
    Symbol symbol;
    Side side;
    Quantity quantity;
    Quantity filled_qty;
    Price price;            // 0 for market orders
    bool is_market;
    uint64_t submit_time_ns;
    uint64_t fill_time_ns;
    FillStatus status;
    Price avg_fill_price;
};

/**
 * Fill Event
 */
struct FillEvent {
    OrderId order_id;
    Symbol symbol;
    Side side;
    Quantity quantity;
    Price price;
    uint64_t timestamp_ns;
    bool is_maker;          // True if added liquidity
};

/**
 * Position State
 */
struct PaperPosition {
    Symbol symbol;
    int64_t quantity;       // Signed: positive = long, negative = short
    Price avg_entry_price;
    double unrealized_pnl;
    double realized_pnl;
    uint64_t last_update_ns;
};

/**
 * Fill Simulation Configuration
 */
struct FillSimConfig {
    // Latency simulation
    uint64_t min_latency_ns = 500'000;      // 500us minimum
    uint64_t max_latency_ns = 2'000'000;    // 2ms maximum
    uint64_t jitter_ns = 100'000;           // 100us jitter

    // Slippage simulation
    double slippage_bps = 0.5;              // 0.5 bps average slippage
    double slippage_variance = 0.3;         // Variance factor

    // Fill probability (for limit orders)
    double fill_probability = 0.8;          // 80% chance of fill at price

    // Partial fill simulation
    bool enable_partial_fills = true;
    double partial_fill_rate = 0.3;         // 30% chance of partial fill

    // Market impact
    double market_impact_bps = 1.0;         // 1 bps per 1000 shares

    // Random seed (0 = use time)
    uint64_t random_seed = 0;
};

/**
 * Paper Trading Order Sender
 *
 * Simulates order execution with realistic:
 * - Latency (configurable)
 * - Slippage (based on order size and volatility)
 * - Partial fills
 * - Market impact
 */
class PaperOrderSender {
public:
    using FillCallback = std::function<void(const FillEvent&)>;

    explicit PaperOrderSender(const FillSimConfig& config = FillSimConfig())
        : config_(config)
        , next_order_id_(1)
        , total_orders_(0)
        , total_fills_(0)
        , rng_(config.random_seed != 0 ? config.random_seed :
               std::chrono::high_resolution_clock::now().time_since_epoch().count())
    {}

    // OrderSender interface
    bool send_order(Symbol symbol, Side side, Quantity qty, bool is_market) {
        return send_limit_order(symbol, side, qty, 0, is_market);
    }

    bool send_limit_order(Symbol symbol, Side side, Quantity qty, Price price, bool is_market = false) {
        PaperOrder order;
        order.id = next_order_id_++;
        order.symbol = symbol;
        order.side = side;
        order.quantity = qty;
        order.filled_qty = 0;
        order.price = price;
        order.is_market = is_market;
        order.submit_time_ns = now_ns();
        order.fill_time_ns = 0;
        order.status = FillStatus::Pending;
        order.avg_fill_price = 0;

        pending_orders_[order.id] = order;
        total_orders_++;

        // Schedule fill check
        schedule_fill_check(order.id);

        return true;
    }

    bool cancel_order(Symbol /*symbol*/, OrderId order_id) {
        auto it = pending_orders_.find(order_id);
        if (it == pending_orders_.end()) {
            return false;
        }

        it->second.status = FillStatus::Cancelled;
        pending_orders_.erase(it);
        return true;
    }

    /**
     * Process pending fills (call in event loop)
     * Pass current market price for fill simulation.
     */
    void process_fills(Symbol symbol, Price bid, Price ask) {
        auto now = now_ns();

        std::vector<OrderId> to_remove;

        for (auto& [id, order] : pending_orders_) {
            if (order.symbol != symbol) continue;
            if (order.status != FillStatus::Pending) continue;

            // Check latency
            uint64_t latency = simulate_latency();
            if (now - order.submit_time_ns < latency) continue;

            // Simulate fill
            Price fill_price = simulate_fill_price(order, bid, ask);

            if (fill_price == 0) {
                // Order not filled (limit order not marketable)
                continue;
            }

            // Simulate partial vs full fill
            Quantity fill_qty = simulate_fill_quantity(order);

            // Apply market impact
            fill_price = apply_market_impact(fill_price, order.side, fill_qty);

            // Execute fill
            order.filled_qty += fill_qty;
            order.fill_time_ns = now;

            // Update average fill price
            if (order.avg_fill_price == 0) {
                order.avg_fill_price = fill_price;
            } else {
                // Weighted average
                order.avg_fill_price = (order.avg_fill_price * (order.filled_qty - fill_qty) +
                                       fill_price * fill_qty) / order.filled_qty;
            }

            // Determine status
            if (order.filled_qty >= order.quantity) {
                order.status = FillStatus::Filled;
                to_remove.push_back(id);
            } else {
                order.status = FillStatus::PartialFill;
            }

            // Notify
            if (on_fill_) {
                FillEvent event;
                event.order_id = order.id;
                event.symbol = order.symbol;
                event.side = order.side;
                event.quantity = fill_qty;
                event.price = fill_price;
                event.timestamp_ns = now;
                event.is_maker = !order.is_market && order.price > 0;

                on_fill_(event);
            }

            total_fills_++;
        }

        // Remove filled orders
        for (auto id : to_remove) {
            auto it = pending_orders_.find(id);
            if (it != pending_orders_.end()) {
                filled_orders_.push_back(it->second);
                pending_orders_.erase(it);
            }
        }
    }

    void set_fill_callback(FillCallback cb) { on_fill_ = std::move(cb); }

    // Statistics
    uint64_t total_orders() const { return total_orders_; }
    uint64_t total_fills() const { return total_fills_; }
    size_t pending_count() const { return pending_orders_.size(); }

    const std::vector<PaperOrder>& filled_orders() const { return filled_orders_; }

private:
    FillSimConfig config_;
    OrderId next_order_id_;
    std::unordered_map<OrderId, PaperOrder> pending_orders_;
    std::vector<PaperOrder> filled_orders_;
    FillCallback on_fill_;

    uint64_t total_orders_;
    uint64_t total_fills_;

    std::mt19937_64 rng_;

    uint64_t now_ns() const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    }

    void schedule_fill_check(OrderId /*order_id*/) {
        // In a real system, this would schedule an async callback.
        // For paper trading, fills are processed in process_fills().
    }

    uint64_t simulate_latency() {
        std::uniform_int_distribution<uint64_t> dist(
            config_.min_latency_ns, config_.max_latency_ns);
        std::normal_distribution<double> jitter(0, config_.jitter_ns);

        uint64_t base_latency = dist(rng_);
        int64_t jitter_val = static_cast<int64_t>(jitter(rng_));

        return static_cast<uint64_t>(std::max(
            static_cast<int64_t>(config_.min_latency_ns),
            static_cast<int64_t>(base_latency) + jitter_val));
    }

    Price simulate_fill_price(const PaperOrder& order, Price bid, Price ask) {
        if (order.is_market || order.price == 0) {
            // Market order: fill at current market with slippage
            Price base_price = (order.side == Side::Buy) ? ask : bid;
            return apply_slippage(base_price, order.side);
        }

        // Limit order: check if marketable
        if (order.side == Side::Buy && order.price >= ask) {
            return apply_slippage(ask, order.side);
        }
        if (order.side == Side::Sell && order.price <= bid) {
            return apply_slippage(bid, order.side);
        }

        // Not marketable - check if would fill based on probability
        std::uniform_real_distribution<double> dist(0, 1);
        if (dist(rng_) < config_.fill_probability) {
            return order.price;  // Fill at limit price
        }

        return 0;  // No fill
    }

    Price apply_slippage(Price price, Side side) {
        std::normal_distribution<double> dist(config_.slippage_bps, config_.slippage_variance);
        double slippage = std::abs(dist(rng_)) / 10000.0;  // Convert bps to decimal

        if (side == Side::Buy) {
            return static_cast<Price>(price * (1.0 + slippage));
        } else {
            return static_cast<Price>(price * (1.0 - slippage));
        }
    }

    Price apply_market_impact(Price price, Side side, Quantity qty) {
        // Impact increases with order size
        double impact = (config_.market_impact_bps / 10000.0) * (qty / 1000.0);

        if (side == Side::Buy) {
            return static_cast<Price>(price * (1.0 + impact));
        } else {
            return static_cast<Price>(price * (1.0 - impact));
        }
    }

    Quantity simulate_fill_quantity(const PaperOrder& order) {
        Quantity remaining = order.quantity - order.filled_qty;

        if (!config_.enable_partial_fills) {
            return remaining;
        }

        std::uniform_real_distribution<double> dist(0, 1);
        if (dist(rng_) < config_.partial_fill_rate) {
            // Partial fill: 50-100% of remaining
            std::uniform_real_distribution<double> pct(0.5, 1.0);
            return static_cast<Quantity>(remaining * pct(rng_));
        }

        return remaining;  // Full fill
    }
};

// Verify concept satisfaction
static_assert(is_order_sender_v<PaperOrderSender>, "PaperOrderSender must satisfy OrderSender concept");

/**
 * Paper Trading Engine Configuration
 */
struct PaperTradingConfig {
    double initial_capital = 100000.0;
    FillSimConfig fill_config = {};
    strategy::RegimeConfig regime_config = {};

    // Risk limits
    double max_position_value = 10000.0;  // Max notional per symbol
    int64_t max_position_qty = 1000;      // Max shares per symbol
    double max_loss_pct = 0.02;           // 2% max drawdown

    // Logging
    bool enable_logging = true;
    logging::LogLevel log_level = logging::LogLevel::Info;
};

/**
 * Paper Trading Engine
 *
 * Complete paper trading system with:
 * - Order simulation
 * - Position tracking
 * - P&L calculation
 * - Regime detection
 * - Async logging
 */
class PaperTradingEngine {
public:
    using Config = PaperTradingConfig;

    explicit PaperTradingEngine(const Config& config = Config())
        : config_(config)
        , order_sender_(config.fill_config)
        , regime_detector_(config.regime_config)
        , capital_(config.initial_capital)
        , peak_equity_(config.initial_capital)
        , is_halted_(false)
    {
        // Set up fill callback
        order_sender_.set_fill_callback([this](const FillEvent& event) {
            on_fill(event);
        });

        // Start logger if enabled
        if (config_.enable_logging) {
            logger_.set_min_level(config_.log_level);
            logger_.start();
        }
    }

    ~PaperTradingEngine() {
        if (config_.enable_logging) {
            logger_.stop();
        }
    }

    /**
     * Process market data update
     */
    void on_market_data(Symbol symbol, Price bid, Price ask, uint64_t timestamp_ns) {
        if (is_halted_) return;

        // Update regime
        double mid = (bid + ask) / 2.0 / 10000.0;  // Convert to dollars
        regime_detector_.update(mid);

        // Update positions with mark-to-market
        update_position_pnl(symbol, bid, ask);

        // Process pending fills
        order_sender_.process_fills(symbol, bid, ask);

        // Check risk limits
        check_risk_limits();

        // Store last prices
        last_prices_[symbol] = {bid, ask, timestamp_ns};
    }

    /**
     * Submit order
     */
    bool submit_order(Symbol symbol, Side side, Quantity qty, bool is_market = true) {
        if (is_halted_) {
            if (config_.enable_logging) {
                LOG_WARN(logger_, "Order rejected: trading halted");
            }
            return false;
        }

        // Check position limits
        auto& pos = positions_[symbol];
        int64_t new_qty = pos.quantity + (side == Side::Buy ? qty : -static_cast<int64_t>(qty));

        if (std::abs(new_qty) > config_.max_position_qty) {
            if (config_.enable_logging) {
                LOGF_WARN(logger_, "Order rejected: pos limit %ld", new_qty);
            }
            return false;
        }

        bool result = order_sender_.send_order(symbol, side, qty, is_market);

        if (result && config_.enable_logging) {
            LOGF_INFO(logger_, "Order sent: %s %u @ mkt",
                     side == Side::Buy ? "BUY" : "SELL", qty);
        }

        return result;
    }

    /**
     * Get current position
     */
    const PaperPosition& get_position(Symbol symbol) {
        return positions_[symbol];
    }

    /**
     * Get total P&L
     */
    double total_pnl() const {
        double total = 0;
        for (const auto& [_, pos] : positions_) {
            total += pos.realized_pnl + pos.unrealized_pnl;
        }
        return total;
    }

    /**
     * Get current equity
     */
    double equity() const {
        return capital_ + total_pnl();
    }

    /**
     * Get current drawdown
     */
    double drawdown() const {
        double current = equity();
        if (current >= peak_equity_) {
            return 0.0;
        }
        return (peak_equity_ - current) / peak_equity_;
    }

    // Getters
    strategy::MarketRegime current_regime() const { return regime_detector_.current_regime(); }
    double regime_confidence() const { return regime_detector_.confidence(); }
    double volatility() const { return regime_detector_.volatility(); }
    double trend_strength() const { return regime_detector_.trend_strength(); }

    bool is_halted() const { return is_halted_; }
    void set_halted(bool halted) { is_halted_ = halted; }

    const Config& config() const { return config_; }
    logging::AsyncLogger& logger() { return logger_; }

    // Statistics
    uint64_t total_orders() const { return order_sender_.total_orders(); }
    uint64_t total_fills() const { return order_sender_.total_fills(); }

private:
    Config config_;
    PaperOrderSender order_sender_;
    strategy::RegimeDetector regime_detector_;
    logging::AsyncLogger logger_;

    double capital_;
    double peak_equity_;
    bool is_halted_;

    std::unordered_map<Symbol, PaperPosition> positions_;

    struct PriceInfo {
        Price bid;
        Price ask;
        uint64_t timestamp_ns;
    };
    std::unordered_map<Symbol, PriceInfo> last_prices_;

    void on_fill(const FillEvent& event) {
        auto& pos = positions_[event.symbol];

        // Update position
        int64_t sign = (event.side == Side::Buy) ? 1 : -1;
        int64_t old_qty = pos.quantity;
        int64_t fill_qty = static_cast<int64_t>(event.quantity) * sign;

        // Check if closing or opening
        bool is_closing = (old_qty != 0) && ((old_qty > 0) != (fill_qty > 0));

        if (is_closing) {
            // Calculate realized P&L for closed portion
            int64_t close_qty = std::min(std::abs(old_qty), std::abs(fill_qty));
            double price_diff = (static_cast<double>(event.price) - pos.avg_entry_price) / 10000.0;

            if (old_qty > 0) {
                pos.realized_pnl += price_diff * close_qty;
            } else {
                pos.realized_pnl -= price_diff * close_qty;
            }
        }

        // Update quantity
        pos.quantity += fill_qty;

        // Update average entry price
        if (pos.quantity == 0) {
            pos.avg_entry_price = 0;
        } else if (!is_closing || std::abs(fill_qty) > std::abs(old_qty)) {
            // Opening or reversing position
            pos.avg_entry_price = event.price;
        }

        pos.symbol = event.symbol;
        pos.last_update_ns = event.timestamp_ns;

        if (config_.enable_logging) {
            LOGF_INFO(logger_, "Fill: %s %u @ %.4f pos=%ld",
                     event.side == Side::Buy ? "BUY" : "SELL",
                     event.quantity,
                     event.price / 10000.0,
                     pos.quantity);
        }
    }

    void update_position_pnl(Symbol symbol, Price bid, Price ask) {
        auto it = positions_.find(symbol);
        if (it == positions_.end()) return;

        auto& pos = it->second;
        if (pos.quantity == 0) {
            pos.unrealized_pnl = 0;
            return;
        }

        // Mark to market
        Price mark_price = (pos.quantity > 0) ? bid : ask;
        double price_diff = (static_cast<double>(mark_price) - pos.avg_entry_price) / 10000.0;

        if (pos.quantity > 0) {
            pos.unrealized_pnl = price_diff * pos.quantity;
        } else {
            pos.unrealized_pnl = -price_diff * std::abs(pos.quantity);
        }
    }

    void check_risk_limits() {
        double current_dd = drawdown();

        if (current_dd >= config_.max_loss_pct && !is_halted_) {
            is_halted_ = true;

            if (config_.enable_logging) {
                LOGF_WARN(logger_, "HALT: drawdown %.2f%% >= limit %.2f%%",
                         current_dd * 100, config_.max_loss_pct * 100);
            }
        }

        // Update peak equity
        double current = equity();
        if (current > peak_equity_) {
            peak_equity_ = current;
        }
    }
};

}  // namespace paper
}  // namespace hft
