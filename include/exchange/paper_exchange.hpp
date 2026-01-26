#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include "../ipc/execution_report.hpp"
#include "../ipc/shared_config.hpp"
#include "../ipc/shared_paper_config.hpp"

namespace hft {
namespace exchange {

/**
 * PaperExchange - Simulated exchange for paper trading
 *
 * Produces ExecutionReport messages identical to real exchange format.
 * TradingEngine processes these without knowing the source (paper vs real).
 *
 * Features:
 * - Market orders: Instant fill at current bid/ask + slippage
 * - Limit orders: Pending until price crosses limit (pessimistic fill + slippage)
 * - Commission: Read from SharedConfig (configurable)
 * - Slippage: Read from SharedConfig (default 5 bps for paper trading realism)
 * - Pre-allocated arrays: No dynamic allocation on hot path
 *
 * Fill logic (pessimistic):
 * - BUY limit at P fills when: ask < P (price crossed below our limit)
 * - SELL limit at P fills when: bid > P (price crossed above our limit)
 * - Slippage always adverse: BUY pays more, SELL receives less
 */
class PaperExchange {
public:
    static constexpr size_t MAX_PENDING_ORDERS = 256;
    static constexpr size_t MAX_SYMBOL_LEN = 16;
    static constexpr double DEFAULT_SLIPPAGE_BPS = 5.0;  // 5 bps = 0.05% (pessimistic default)

    using ExecutionCallback = std::function<void(const ipc::ExecutionReport&)>;
    using SlippageCallback = std::function<void(double)>;

    struct PendingLimitOrder {
        char symbol[MAX_SYMBOL_LEN];
        uint64_t order_id;
        hft::Side side;
        double quantity;
        double limit_price;
        uint64_t submit_time_ns;
        bool active;

        void clear() {
            std::memset(this, 0, sizeof(PendingLimitOrder));
            active = false;
        }
    };

    PaperExchange()
        : next_order_id_(1)
        , pending_count_(0)
        , config_(nullptr)
        , paper_config_(nullptr)
        , total_slippage_(0)
    {
        for (auto& order : pending_orders_) {
            order.clear();
        }
    }

    // Set config pointer for reading commission rate
    void set_config(const ipc::SharedConfig* config) {
        config_ = config;
    }

    // Set paper config pointer for paper-trading specific settings (slippage, etc.)
    void set_paper_config(const ipc::SharedPaperConfig* paper_config) {
        paper_config_ = paper_config;
    }

    // Set callback for execution reports
    void set_execution_callback(ExecutionCallback callback) {
        on_execution_ = std::move(callback);
    }

    // Set callback for slippage events (for tracking in portfolio state)
    void set_slippage_callback(SlippageCallback callback) {
        on_slippage_ = std::move(callback);
    }

    /**
     * Send a market order - fills immediately
     *
     * @param symbol Symbol to trade
     * @param side Buy or Sell
     * @param quantity Amount to trade
     * @param bid Current bid price (used for sell)
     * @param ask Current ask price (used for buy)
     * @param timestamp Current timestamp
     * @return ExecutionReport with fill details
     */
    ipc::ExecutionReport send_market_order(
        const char* symbol,
        hft::Side side,
        double quantity,
        double bid,
        double ask,
        uint64_t timestamp
    ) {
        uint64_t order_id = next_order_id_++;

        // Market order fills at: ask for buy, bid for sell
        double base_price = (side == hft::Side::Buy) ? ask : bid;

        // Apply slippage (always adverse)
        double slippage_rate = get_slippage_bps() / 10000.0;
        double slippage_amount = base_price * slippage_rate;
        double fill_price;
        if (side == hft::Side::Buy) {
            fill_price = base_price + slippage_amount;  // BUY: pay MORE
        } else {
            fill_price = base_price - slippage_amount;  // SELL: receive LESS
        }

        // Track slippage cost
        double slippage_cost = slippage_amount * quantity;
        total_slippage_ += slippage_cost;
        if (on_slippage_) on_slippage_(slippage_cost);

        double commission = calculate_commission(quantity * fill_price);

        auto report = ipc::ExecutionReport::market_fill(
            symbol, order_id, side, quantity, fill_price, commission, timestamp
        );

        // Notify callback
        if (on_execution_) {
            on_execution_(report);
        }

        return report;
    }

    /**
     * Send a limit order - goes to pending list
     *
     * @param symbol Symbol to trade
     * @param side Buy or Sell
     * @param quantity Amount to trade
     * @param limit_price Limit price
     * @param timestamp Current timestamp
     * @return ExecutionReport with order accepted status (or rejected if full)
     */
    ipc::ExecutionReport send_limit_order(
        const char* symbol,
        hft::Side side,
        double quantity,
        double limit_price,
        uint64_t timestamp
    ) {
        uint64_t order_id = next_order_id_++;

        // Find free slot
        int slot = find_free_slot();
        if (slot < 0) {
            // No room for more pending orders
            auto report = ipc::ExecutionReport::rejected(
                symbol, order_id, side, ipc::OrderType::Limit,
                "MAX_PENDING_EXCEEDED", timestamp
            );
            if (on_execution_) {
                on_execution_(report);
            }
            return report;
        }

        // Add to pending list
        auto& order = pending_orders_[slot];
        std::strncpy(order.symbol, symbol, MAX_SYMBOL_LEN - 1);
        order.symbol[MAX_SYMBOL_LEN - 1] = '\0';
        order.order_id = order_id;
        order.side = side;
        order.quantity = quantity;
        order.limit_price = limit_price;
        order.submit_time_ns = timestamp;
        order.active = true;
        pending_count_++;

        // Return "New" report
        auto report = ipc::ExecutionReport::limit_accepted(
            symbol, order_id, side, timestamp
        );
        if (on_execution_) {
            on_execution_(report);
        }
        return report;
    }

    /**
     * Cancel a pending limit order
     *
     * @param order_id Order to cancel
     * @param timestamp Current timestamp
     * @return true if cancelled, false if not found
     */
    bool cancel_order(uint64_t order_id, uint64_t timestamp) {
        for (auto& order : pending_orders_) {
            if (order.active && order.order_id == order_id) {
                ipc::ExecutionReport report;
                report.clear();
                std::strncpy(report.symbol, order.symbol, sizeof(report.symbol) - 1);
                report.order_id = order_id;
                report.side = order.side;
                report.order_type = ipc::OrderType::Limit;
                report.exec_type = ipc::ExecType::Cancelled;
                report.status = ipc::OrderStatus::Cancelled;
                report.order_timestamp_ns = order.submit_time_ns;
                report.exec_timestamp_ns = timestamp;

                order.clear();
                pending_count_--;

                if (on_execution_) {
                    on_execution_(report);
                }
                return true;
            }
        }
        return false;
    }

    /**
     * Check pending orders for fills - call on each price update
     *
     * Uses pessimistic fill logic:
     * - BUY limit at P fills when ask < P (price crossed below)
     * - SELL limit at P fills when bid > P (price crossed above)
     *
     * @param symbol Symbol to check (only orders for this symbol)
     * @param bid Current bid price
     * @param ask Current ask price
     * @param timestamp Current timestamp
     */
    void on_price_update(
        const char* symbol,
        double bid,
        double ask,
        uint64_t timestamp
    ) {
        if (pending_count_ == 0) return;

        for (auto& order : pending_orders_) {
            if (!order.active) continue;
            if (std::strcmp(order.symbol, symbol) != 0) continue;

            bool should_fill = false;
            double fill_price = 0;

            if (order.side == hft::Side::Buy) {
                // Buy limit: fill when ask drops BELOW our limit (pessimistic)
                if (ask < order.limit_price) {
                    should_fill = true;
                    fill_price = ask;  // Fill at current ask, not our limit
                }
            } else {
                // Sell limit: fill when bid rises ABOVE our limit (pessimistic)
                if (bid > order.limit_price) {
                    should_fill = true;
                    fill_price = bid;  // Fill at current bid, not our limit
                }
            }

            if (should_fill) {
                // Apply slippage (always adverse)
                double slippage_rate = get_slippage_bps() / 10000.0;
                double slippage_amount = fill_price * slippage_rate;
                if (order.side == hft::Side::Buy) {
                    fill_price += slippage_amount;  // BUY: pay MORE
                } else {
                    fill_price -= slippage_amount;  // SELL: receive LESS
                }

                // Track slippage cost
                double slippage_cost = slippage_amount * order.quantity;
                total_slippage_ += slippage_cost;
                if (on_slippage_) on_slippage_(slippage_cost);

                double commission = calculate_commission(order.quantity * fill_price);

                auto report = ipc::ExecutionReport::limit_fill(
                    order.symbol,
                    order.order_id,
                    order.side,
                    order.quantity,
                    fill_price,
                    commission,
                    order.submit_time_ns,
                    timestamp
                );

                if (on_execution_) {
                    on_execution_(report);
                }

                order.clear();
                pending_count_--;
            }
        }
    }

    // Accessors
    size_t pending_count() const { return pending_count_; }
    uint64_t next_order_id() const { return next_order_id_; }
    double total_slippage() const { return total_slippage_; }

    // Get effective slippage in basis points
    // Priority: SharedPaperConfig > SharedConfig (deprecated) > default
    double get_slippage_bps() const {
        if (paper_config_) {
            return paper_config_->slippage_bps();
        }
        // Fallback to SharedConfig for backward compatibility (deprecated)
        if (config_) {
            return config_->slippage_bps();
        }
        return DEFAULT_SLIPPAGE_BPS;  // Fallback only if no config attached
    }

    // Get pending order by ID (for inspection)
    const PendingLimitOrder* find_order(uint64_t order_id) const {
        for (const auto& order : pending_orders_) {
            if (order.active && order.order_id == order_id) {
                return &order;
            }
        }
        return nullptr;
    }

private:
    int find_free_slot() {
        for (size_t i = 0; i < MAX_PENDING_ORDERS; ++i) {
            if (!pending_orders_[i].active) {
                return static_cast<int>(i);
            }
        }
        return -1;  // No free slot
    }

    double calculate_commission(double notional) const {
        double rate = 0.001;  // Default 0.1%
        if (config_) {
            rate = config_->commission_rate();
        }
        return notional * rate;
    }

    std::array<PendingLimitOrder, MAX_PENDING_ORDERS> pending_orders_;
    uint64_t next_order_id_;
    size_t pending_count_;
    const ipc::SharedConfig* config_;
    const ipc::SharedPaperConfig* paper_config_;
    double total_slippage_;
    ExecutionCallback on_execution_;
    SlippageCallback on_slippage_;
};

}  // namespace exchange
}  // namespace hft
