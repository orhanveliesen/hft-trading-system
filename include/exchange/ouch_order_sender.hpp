#pragma once

#include "../order_sender.hpp"
#include "../ouch/ouch_session.hpp"
#include "../ouch/ouch_messages.hpp"
#include <unordered_map>
#include <string>
#include <cstring>

namespace hft {
namespace exchange {

/**
 * OuchOrderSender - OrderSender implementation for OUCH protocol
 *
 * Implements the OrderSender concept for NASDAQ, BIST, and other
 * exchanges using OUCH 4.2 protocol.
 *
 * Features:
 *   - Zero-cost abstraction (template-based polymorphism)
 *   - Symbol ID -> stock ticker mapping
 *   - Order token -> OrderId tracking
 *   - Price conversion (4 decimal places)
 *   - Configurable TIF (default: IOC for HFT)
 *
 * Usage:
 *   ouch::OuchSessionConfig config;
 *   config.host = "nasdaq-ouch.example.com";
 *   config.port = 15000;
 *   config.username = "MYUSER";
 *   config.password = "MYPASS";
 *   config.firm = "MYFIRM";
 *
 *   ouch::OuchSession session(config);
 *   OuchOrderSender sender(session);
 *
 *   sender.register_symbol(1, "AAPL");
 *   sender.connect();
 *
 *   TradingEngine<OuchOrderSender> engine(sender);
 */
class OuchOrderSender {
public:
    // Order tracking info
    struct OrderInfo {
        Symbol symbol;
        Side side;
        Quantity quantity;
        Price price;
        uint64_t exchange_ref;  // Exchange order reference (from Accepted)
        bool is_live;
    };

    explicit OuchOrderSender(ouch::OuchSession& session)
        : session_(session)
        , default_tif_(ouch::TIF_IOC)
        , orders_sent_(0)
        , orders_filled_(0)
        , orders_canceled_(0)
        , orders_rejected_(0)
    {
        // Set up response callbacks
        session_.set_accepted_callback([this](const ouch::Accepted& msg) {
            on_accepted(msg);
        });

        session_.set_executed_callback([this](const ouch::Executed& msg) {
            on_executed(msg);
        });

        session_.set_canceled_callback([this](const ouch::Canceled& msg) {
            on_canceled(msg);
        });

        session_.set_rejected_callback([this](const ouch::Rejected& msg) {
            on_rejected(msg);
        });

        session_.set_replaced_callback([this](const ouch::Replaced& msg) {
            on_replaced(msg);
        });
    }

    // Register symbol mapping (Symbol ID -> stock ticker)
    void register_symbol(Symbol id, const std::string& ticker) {
        symbol_to_ticker_[id] = ticker;
        ticker_to_symbol_[ticker] = id;
    }

    // Connect to exchange
    bool connect() {
        return session_.connect();
    }

    // Disconnect from exchange
    void disconnect() {
        session_.disconnect();
    }

    // Check if connected
    bool is_connected() const {
        return session_.is_connected();
    }

    // Process incoming messages (call in event loop)
    int process() {
        return session_.process_incoming();
    }

    // Set default Time-in-Force
    void set_default_tif(uint32_t tif) {
        default_tif_ = tif;
    }

    // ============================================
    // OrderSender Interface
    // ============================================

    /**
     * Send order to exchange
     *
     * @param symbol Symbol ID
     * @param side Buy or Sell
     * @param qty Quantity
     * @param is_market If true, send as IOC with aggressive price
     * @return true if sent successfully
     */
    bool send_order(Symbol symbol, Side side, Quantity qty, bool is_market) {
        return send_order_with_price(symbol, side, qty, 0, is_market);
    }

    /**
     * Send limit order with specific price
     */
    bool send_order_with_price(Symbol symbol, Side side, Quantity qty, Price price, bool is_market = false) {
        // Check connection
        if (!session_.is_connected()) {
            return false;
        }

        // Look up ticker
        auto it = symbol_to_ticker_.find(symbol);
        if (it == symbol_to_ticker_.end()) {
            return false;  // Unknown symbol
        }

        // Build order message
        ouch::EnterOrder order;
        order.init();

        // Generate and set token
        char token[15];
        session_.generate_token(token);
        order.set_token(token);

        // Set order details
        order.side = (side == Side::Buy) ? ouch::SIDE_BUY : ouch::SIDE_SELL;
        order.set_quantity(static_cast<uint32_t>(qty));
        order.set_stock(it->second.c_str());

        // Price: OUCH uses 4 decimal places
        // If market order, use 0 (exchange will fill at market)
        uint32_t ouch_price = is_market ? 0 : static_cast<uint32_t>(price);
        order.set_price(ouch_price);

        // Time in force
        order.set_time_in_force(is_market ? ouch::TIF_IOC : default_tif_);

        // Set firm
        order.set_firm(session_.config().firm);

        // Display type
        order.display = ouch::DISPLAY_VISIBLE;

        // Send order
        if (!session_.send_enter_order(order)) {
            return false;
        }

        // Track pending order
        std::string token_str(token, 14);
        pending_orders_[token_str] = OrderInfo{
            symbol, side, qty, price, 0, false
        };

        ++orders_sent_;
        return true;
    }

    /**
     * Cancel order
     *
     * @param symbol Symbol ID (used for validation)
     * @param order_id Order ID (maps to token)
     * @return true if cancel request sent successfully
     */
    bool cancel_order(Symbol /*symbol*/, OrderId order_id) {
        if (!session_.is_connected()) {
            return false;
        }

        // Look up order by ID
        auto it = order_id_to_token_.find(order_id);
        if (it == order_id_to_token_.end()) {
            return false;  // Unknown order
        }

        // Build cancel message
        ouch::CancelOrder cancel;
        cancel.init();
        cancel.set_token(it->second.c_str());
        cancel.set_quantity(0);  // Full cancel

        return session_.send_cancel_order(cancel);
    }

    /**
     * Cancel order by token (internal use)
     */
    bool cancel_by_token(const std::string& token) {
        if (!session_.is_connected()) {
            return false;
        }

        ouch::CancelOrder cancel;
        cancel.init();
        cancel.set_token(token.c_str());
        cancel.set_quantity(0);

        return session_.send_cancel_order(cancel);
    }

    /**
     * Replace order with new price/quantity
     */
    bool replace_order(const std::string& existing_token, Quantity new_qty, Price new_price) {
        if (!session_.is_connected()) {
            return false;
        }

        // Generate new token
        char new_token[15];
        session_.generate_token(new_token);

        ouch::ReplaceOrder replace;
        replace.init();
        replace.set_existing_token(existing_token.c_str());
        replace.set_replacement_token(new_token);
        replace.set_quantity(static_cast<uint32_t>(new_qty));
        replace.set_price(static_cast<uint32_t>(new_price));
        replace.set_time_in_force(default_tif_);

        return session_.send_replace_order(replace);
    }

    // ============================================
    // Order State Queries
    // ============================================

    const OrderInfo* get_order(const std::string& token) const {
        auto it = pending_orders_.find(token);
        if (it != pending_orders_.end()) {
            return &it->second;
        }
        auto it2 = live_orders_.find(token);
        if (it2 != live_orders_.end()) {
            return &it2->second;
        }
        return nullptr;
    }

    // ============================================
    // Statistics
    // ============================================

    uint64_t orders_sent() const { return orders_sent_; }
    uint64_t orders_filled() const { return orders_filled_; }
    uint64_t orders_canceled() const { return orders_canceled_; }
    uint64_t orders_rejected() const { return orders_rejected_; }

    size_t pending_count() const { return pending_orders_.size(); }
    size_t live_count() const { return live_orders_.size(); }

    // ============================================
    // Callbacks for order events
    // ============================================

    using OrderCallback = std::function<void(const std::string& token, const OrderInfo& info)>;
    using ExecutionCallback = std::function<void(const std::string& token, Quantity filled_qty, Price fill_price)>;

    void set_on_order_accepted(OrderCallback cb) { on_order_accepted_ = std::move(cb); }
    void set_on_order_rejected(OrderCallback cb) { on_order_rejected_ = std::move(cb); }
    void set_on_order_canceled(OrderCallback cb) { on_order_canceled_ = std::move(cb); }
    void set_on_order_executed(ExecutionCallback cb) { on_order_executed_ = std::move(cb); }

private:
    // Response handlers
    void on_accepted(const ouch::Accepted& msg) {
        std::string token(msg.token, 14);

        auto it = pending_orders_.find(token);
        if (it != pending_orders_.end()) {
            it->second.exchange_ref = msg.get_order_ref();
            it->second.is_live = true;

            // Move to live orders
            live_orders_[token] = it->second;
            pending_orders_.erase(it);

            // Generate internal OrderId from token
            OrderId oid = generate_order_id(token);
            order_id_to_token_[oid] = token;
            token_to_order_id_[token] = oid;

            if (on_order_accepted_) {
                on_order_accepted_(token, live_orders_[token]);
            }
        }
    }

    void on_executed(const ouch::Executed& msg) {
        std::string token(msg.token, 14);

        auto it = live_orders_.find(token);
        if (it != live_orders_.end()) {
            Quantity fill_qty = msg.get_executed_quantity();
            Price fill_price = msg.get_execution_price();

            ++orders_filled_;

            if (on_order_executed_) {
                on_order_executed_(token, fill_qty, fill_price);
            }

            // Update remaining quantity
            if (fill_qty >= it->second.quantity) {
                // Fully filled - remove from live orders
                live_orders_.erase(it);
            } else {
                it->second.quantity -= fill_qty;
            }
        }
    }

    void on_canceled(const ouch::Canceled& msg) {
        std::string token(msg.token, 14);

        auto it = live_orders_.find(token);
        if (it != live_orders_.end()) {
            ++orders_canceled_;

            if (on_order_canceled_) {
                on_order_canceled_(token, it->second);
            }

            live_orders_.erase(it);
        }
    }

    void on_rejected(const ouch::Rejected& msg) {
        std::string token(msg.token, 14);

        auto it = pending_orders_.find(token);
        if (it != pending_orders_.end()) {
            ++orders_rejected_;

            if (on_order_rejected_) {
                on_order_rejected_(token, it->second);
            }

            pending_orders_.erase(it);
        }
    }

    void on_replaced(const ouch::Replaced& msg) {
        std::string old_token(msg.previous_token, 14);
        std::string new_token(msg.replacement_token, 14);

        auto it = live_orders_.find(old_token);
        if (it != live_orders_.end()) {
            // Update order info
            OrderInfo info = it->second;
            info.quantity = msg.get_quantity();
            info.price = msg.get_price();
            info.exchange_ref = msg.get_order_ref();

            // Remove old, add new
            live_orders_.erase(it);
            live_orders_[new_token] = info;

            // Update token mappings
            auto oid_it = token_to_order_id_.find(old_token);
            if (oid_it != token_to_order_id_.end()) {
                OrderId oid = oid_it->second;
                token_to_order_id_.erase(oid_it);
                token_to_order_id_[new_token] = oid;
                order_id_to_token_[oid] = new_token;
            }
        }
    }

    // Generate internal OrderId from token string
    static OrderId generate_order_id(const std::string& token) {
        // Simple hash of token
        uint64_t hash = 0;
        for (char c : token) {
            hash = hash * 31 + static_cast<uint64_t>(c);
        }
        return hash;
    }

    ouch::OuchSession& session_;
    uint32_t default_tif_;

    // Symbol mappings
    std::unordered_map<Symbol, std::string> symbol_to_ticker_;
    std::unordered_map<std::string, Symbol> ticker_to_symbol_;

    // Order tracking
    std::unordered_map<std::string, OrderInfo> pending_orders_;  // Token -> OrderInfo
    std::unordered_map<std::string, OrderInfo> live_orders_;     // Token -> OrderInfo
    std::unordered_map<OrderId, std::string> order_id_to_token_;
    std::unordered_map<std::string, OrderId> token_to_order_id_;

    // Statistics
    uint64_t orders_sent_;
    uint64_t orders_filled_;
    uint64_t orders_canceled_;
    uint64_t orders_rejected_;

    // Event callbacks
    OrderCallback on_order_accepted_;
    OrderCallback on_order_rejected_;
    OrderCallback on_order_canceled_;
    ExecutionCallback on_order_executed_;
};

// Verify concept satisfaction
static_assert(concepts::OrderSender<OuchOrderSender>,
              "OuchOrderSender must satisfy OrderSender concept");

}  // namespace exchange
}  // namespace hft
