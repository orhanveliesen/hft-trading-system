#pragma once

#include "../types.hpp"
#include "../order_sender.hpp"
#include "../strategy/regime_detector.hpp"
#include "../strategy/adaptive_strategy.hpp"
#include "../risk/enhanced_risk_manager.hpp"
#include "../logging/async_logger.hpp"
#include <vector>
#include <unordered_map>
#include <queue>
#include <functional>
#include <chrono>
#include <random>
#include <cmath>
#include <string>

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
static_assert(concepts::OrderSender<PaperOrderSender>,
              "PaperOrderSender must satisfy OrderSender concept");

/**
 * Per-symbol risk configuration for paper trading
 */
struct SymbolRiskConfig {
    std::string symbol;
    Position max_position = 1000;      // Max shares per symbol
    Notional max_notional = 10000000;  // Max notional per symbol (in scaled units)
};

/**
 * Paper Trading Engine Configuration
 */
struct PaperTradingConfig {
    // Capital
    Capital initial_capital = 100000 * risk::PRICE_SCALE;  // Convert to scaled units

    // Fill simulation
    FillSimConfig fill_config = {};
    strategy::RegimeConfig regime_config = {};

    // Risk limits (maps to EnhancedRiskConfig - all as percentages)
    double daily_loss_limit_pct = 0.02;                      // 2% daily loss limit
    double max_drawdown_pct = 0.10;                          // 10% max drawdown
    Quantity max_order_size = 1000;                          // Max single order size
    double max_notional_pct = 1.0;                           // 100% of capital max exposure

    // Per-symbol defaults
    Position default_max_position = 1000;
    Notional default_max_notional = 100000 * risk::PRICE_SCALE;

    // Symbol configurations
    std::vector<SymbolRiskConfig> symbol_configs;

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
 * - Production-grade risk management (EnhancedRiskManager)
 */
class PaperTradingEngine {
public:
    using Config = PaperTradingConfig;

    explicit PaperTradingEngine(const Config& config = Config())
        : config_(config)
        , order_sender_(config.fill_config)
        , regime_detector_(config.regime_config)
        , risk_manager_(create_risk_config(config))
        , capital_(config.initial_capital)
        , peak_equity_(config.initial_capital)
    {
        // Register symbols from config
        for (const auto& sym_cfg : config.symbol_configs) {
            register_symbol(sym_cfg.symbol, sym_cfg.max_position, sym_cfg.max_notional);
        }

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

private:
    static risk::EnhancedRiskConfig create_risk_config(const Config& cfg) {
        risk::EnhancedRiskConfig risk_cfg;
        risk_cfg.initial_capital = cfg.initial_capital;
        risk_cfg.daily_loss_limit_pct = cfg.daily_loss_limit_pct;
        risk_cfg.max_drawdown_pct = cfg.max_drawdown_pct;
        risk_cfg.max_order_size = cfg.max_order_size;
        risk_cfg.max_notional_pct = cfg.max_notional_pct;
        return risk_cfg;
    }

public:

    ~PaperTradingEngine() {
        if (config_.enable_logging) {
            logger_.stop();
        }
    }

    /**
     * Register a symbol for trading (must be called before trading)
     * Returns SymbolIndex for hot path operations
     */
    risk::SymbolIndex register_symbol(const std::string& symbol_name,
                                           Position max_position = 0,
                                           Notional max_notional = 0) {
        // Use defaults if not specified
        if (max_position == 0) max_position = config_.default_max_position;
        if (max_notional == 0) max_notional = config_.default_max_notional;

        // Register with risk manager
        risk::SymbolIndex idx = risk_manager_.register_symbol(symbol_name, max_position, max_notional);

        // Create Symbol (numeric) from index for internal use
        Symbol symbol = static_cast<Symbol>(idx);
        symbol_index_map_[symbol] = idx;
        symbol_name_map_[symbol] = symbol_name;

        if (config_.enable_logging) {
            LOGF_INFO(logger_, "Symbol %s idx=%u pos=%ld",
                     symbol_name.c_str(), idx, max_position);
        }

        return idx;
    }

    /**
     * Process market data update
     */
    void on_market_data(Symbol symbol, Price bid, Price ask, uint64_t timestamp_ns) {
        if (!risk_manager_.can_trade()) return;

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
     * Submit order with risk checks
     */
    bool submit_order(Symbol symbol, Side side, Quantity qty, bool is_market = true) {
        return submit_order_with_price(symbol, side, qty, 0, is_market);
    }

    /**
     * Submit order with explicit price for risk calculation
     */
    bool submit_order_with_price(Symbol symbol, Side side, Quantity qty, Price price, bool is_market = true) {
        // Get or create symbol index
        risk::SymbolIndex idx = get_or_register_symbol(symbol);

        // Use last known price if not provided
        if (price == 0) {
            auto it = last_prices_.find(symbol);
            if (it != last_prices_.end()) {
                price = (side == Side::Buy) ? it->second.ask : it->second.bid;
            }
        }

        // Pre-trade risk check
        if (!risk_manager_.check_order(idx, side, qty, price)) {
            if (config_.enable_logging) {
                LOGF_WARN(logger_, "Risk reject: %s %u",
                         side == Side::Buy ? "BUY" : "SELL", qty);
            }
            return false;
        }

        bool result = order_sender_.send_order(symbol, side, qty, is_market);

        if (result && config_.enable_logging) {
            LOGF_INFO(logger_, "Order: %s %u",
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

    bool is_halted() const { return !risk_manager_.can_trade(); }
    void set_halted(bool halted) {
        if (halted) {
            risk_manager_.halt();
        } else {
            risk_manager_.reset_halt();
        }
    }

    const Config& config() const { return config_; }
    logging::AsyncLogger& logger() { return logger_; }

    // Risk manager access
    const risk::EnhancedRiskManager& risk_manager() const { return risk_manager_; }
    risk::RiskState risk_state() const { return risk_manager_.build_state(); }

    // Statistics
    uint64_t total_orders() const { return order_sender_.total_orders(); }
    uint64_t total_fills() const { return order_sender_.total_fills(); }

private:
    Config config_;
    PaperOrderSender order_sender_;
    strategy::RegimeDetector regime_detector_;
    risk::EnhancedRiskManager risk_manager_;
    logging::AsyncLogger logger_;

    double capital_;
    double peak_equity_;

    std::unordered_map<Symbol, PaperPosition> positions_;
    std::unordered_map<Symbol, risk::SymbolIndex> symbol_index_map_;
    std::unordered_map<Symbol, std::string> symbol_name_map_;

    struct PriceInfo {
        Price bid;
        Price ask;
        uint64_t timestamp_ns;
    };
    std::unordered_map<Symbol, PriceInfo> last_prices_;

    void on_fill(const FillEvent& event) {
        auto& pos = positions_[event.symbol];

        // Update risk manager position tracking
        risk::SymbolIndex idx = get_or_register_symbol(event.symbol);
        risk_manager_.on_fill(idx, event.side, event.quantity, event.price);

        // Update position
        int64_t sign = (event.side == Side::Buy) ? 1 : -1;
        int64_t old_qty = pos.quantity;
        int64_t fill_qty = static_cast<int64_t>(event.quantity) * sign;

        // Check if closing or opening
        bool is_closing = (old_qty != 0) && ((old_qty > 0) != (fill_qty > 0));

        if (is_closing) {
            // Calculate realized P&L for closed portion
            int64_t close_qty = std::min(std::abs(old_qty), std::abs(fill_qty));
            double price_diff = (static_cast<double>(event.price) - pos.avg_entry_price) / risk::PRICE_SCALE;

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

        // Update risk manager P&L
        PnL total_pnl_scaled = static_cast<PnL>(total_pnl() * risk::PRICE_SCALE);
        risk_manager_.update_pnl(total_pnl_scaled);

        if (config_.enable_logging) {
            LOGF_INFO(logger_, "Fill: %s %u @ %.4f pos=%ld",
                     event.side == Side::Buy ? "BUY" : "SELL",
                     event.quantity,
                     static_cast<double>(event.price) / risk::PRICE_SCALE,
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
        double price_diff = (static_cast<double>(mark_price) - pos.avg_entry_price) / risk::PRICE_SCALE;

        if (pos.quantity > 0) {
            pos.unrealized_pnl = price_diff * pos.quantity;
        } else {
            pos.unrealized_pnl = -price_diff * std::abs(pos.quantity);
        }
    }

    void check_risk_limits() {
        // Risk manager already handles limits
        // Just update P&L for mark-to-market
        PnL total_pnl_scaled = static_cast<PnL>(total_pnl() * risk::PRICE_SCALE);
        risk_manager_.update_pnl(total_pnl_scaled);

        // Update local peak equity tracking
        double current = equity();
        if (current > peak_equity_) {
            peak_equity_ = current;
        }
    }

    // Helper to get symbol index (non-const version for on_fill)
    risk::SymbolIndex get_or_register_symbol(Symbol symbol) {
        auto it = symbol_index_map_.find(symbol);
        if (it != symbol_index_map_.end()) {
            return it->second;
        }

        // Auto-register with default name and limits
        std::string name = "SYM" + std::to_string(symbol);
        risk::SymbolIndex idx = risk_manager_.register_symbol(
            name, config_.default_max_position, config_.default_max_notional);
        symbol_index_map_[symbol] = idx;
        symbol_name_map_[symbol] = name;
        return idx;
    }
};

}  // namespace paper
}  // namespace hft
