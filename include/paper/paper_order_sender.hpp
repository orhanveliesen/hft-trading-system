#pragma once

/**
 * PaperOrderSender - Simulates exchange for paper trading
 *
 * Generates fake exchange signals for all order events.
 * Pessimistic fills: Buy at ask + slippage, Sell at bid - slippage.
 *
 * Slippage simulation:
 * - Reads slippage_bps from SharedConfig (default: 5 bps = 0.05%)
 * - Applies adverse slippage to every fill
 * - Makes paper trading more realistic
 *
 * Queue simulation:
 * - Optional realistic queue position tracking for limit orders
 * - Uses QueueFillDetector to determine when orders would fill
 */

#include "../types.hpp"
#include "../ipc/shared_config.hpp"
#include "../risk/enhanced_risk_manager.hpp"
#include "queue_fill_detector.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

namespace hft {
namespace paper {

class PaperOrderSender {
public:
    static constexpr OrderId PAPER_ID_MASK = 0x8000000000000000ULL;
    static constexpr double DEFAULT_SLIPPAGE_BPS = 5.0;  // 5 bps = 0.05%

    enum class Event { Accepted, Filled, Cancelled, Rejected };

    // NOTE: qty is double (not Quantity/uint32_t) because crypto trades use
    // fractional quantities (e.g., 0.01 BTC). Using uint32_t would truncate to 0.
    using FillCallback = std::function<void(Symbol, OrderId, Side, double qty, Price)>;
    using SlippageCallback = std::function<void(double)>;  // Track slippage cost

    PaperOrderSender() : next_id_(1), total_orders_(0), total_fills_(0),
                         config_(nullptr), total_slippage_(0),
                         use_queue_sim_(false), default_queue_depth_(0) {}

    // Set config for reading slippage_bps
    void set_config(const ipc::SharedConfig* config) { config_ = config; }

    // Queue simulation configuration
    void enable_queue_simulation(bool enable) {
        use_queue_sim_ = enable;
    }

    void set_default_queue_depth(Quantity depth) {
        default_queue_depth_ = depth;
    }

    // Feed trade data to queue detector (for queue position updates)
    void on_trade(Symbol symbol, Price price, Quantity qty, Side aggressor_side, uint64_t timestamp_ns) {
        if (use_queue_sim_) {
            queue_detector_.on_trade(symbol, price, qty, aggressor_side, timestamp_ns);
        }
    }

    // 5-param version with expected_price for slippage tracking
    // is_market: true = market order (immediate fill with slippage)
    //            false = limit order (no slippage, only fills if price is favorable)
    // NOTE: qty is double to support fractional crypto quantities (e.g., 0.01 BTC)
    bool send_order(Symbol symbol, Side side, double qty, Price expected_price, bool is_market) {
        OrderId id = PAPER_ID_MASK | next_id_++;
        total_orders_++;

        // Register limit orders with QueueFillDetector if queue simulation enabled
        if (!is_market && use_queue_sim_) {
            queue_detector_.register_order(id, symbol, side, expected_price,
                                          static_cast<Quantity>(qty), current_time_ns());

            if (default_queue_depth_ > 0) {
                queue_detector_.set_initial_queue_depth(symbol, side, expected_price,
                                                       default_queue_depth_);
            }
        }

        pending_.push_back({symbol, id, side, qty, expected_price, is_market});
        return true;
    }

    // 4-param backward-compatible version (satisfies OrderSender concept)
    bool send_order(Symbol symbol, Side side, double qty, bool is_market) {
        return send_order(symbol, side, qty, 0, is_market);  // No expected_price tracking
    }

    bool cancel_order(Symbol /*symbol*/, OrderId id) {
        auto it = std::find_if(pending_.begin(), pending_.end(),
            [id](const Order& o) { return o.id == id; });
        if (it != pending_.end()) {
            if (use_queue_sim_) {
                queue_detector_.cancel_order(id);
            }
            pending_.erase(it);
            return true;
        }
        return false;
    }

    void process_fills(Symbol symbol, Price bid, Price ask) {
        // Get slippage in basis points from config (for market orders only)
        double slippage_bps = DEFAULT_SLIPPAGE_BPS;
        if (config_) {
            double cfg_slippage = config_->slippage_bps();
            if (cfg_slippage > 0) {
                slippage_bps = cfg_slippage;
            }
        }
        double slippage_rate = slippage_bps / 10000.0;  // Convert bps to decimal

        std::vector<Order> remaining;
        for (auto& o : pending_) {
            if (o.symbol != symbol) {
                remaining.push_back(o);
                continue;
            }

            Price fill_price;
            double slippage_cost = 0;

            if (o.is_market) {
                // MARKET ORDER: Fill immediately with slippage
                Price base_price = o.expected_price;
                if (base_price == 0) {
                    base_price = (o.side == Side::Buy) ? ask : bid;
                }

                // Apply slippage (always adverse) - branchless
                // Side::Buy (0): sign = 1 - 2*0 = 1, adds slippage (buyer pays more)
                // Side::Sell (1): sign = 1 - 2*1 = -1, subtracts slippage (seller gets less)
                double slippage_amount = static_cast<double>(base_price) * slippage_rate;
                int64_t base = static_cast<int64_t>(base_price);
                int64_t slippage = static_cast<int64_t>(slippage_amount);
                int side_sign = 1 - 2 * static_cast<int>(o.side);
                fill_price = static_cast<Price>(base + side_sign * slippage);

                slippage_cost = slippage_amount * o.qty / risk::PRICE_SCALE;
                total_slippage_ += slippage_cost;
                if (on_slippage_) on_slippage_(slippage_cost);

                if (on_fill_) on_fill_(o.symbol, o.id, o.side, o.qty, fill_price);
                total_fills_++;
            } else if (use_queue_sim_) {
                // LIMIT ORDER WITH QUEUE SIMULATION: Check if queue cleared
                auto result = queue_detector_.get_fill_estimate(o.id);
                if (result.filled && result.confidence == FillConfidence::Confirmed) {
                    // Queue cleared, fill at limit price
                    fill_price = o.expected_price;
                    if (on_fill_) on_fill_(o.symbol, o.id, o.side, o.qty, fill_price);
                    total_fills_++;
                } else {
                    // Still in queue, keep pending
                    remaining.push_back(o);
                }
            } else {
                // LIMIT ORDER WITHOUT QUEUE: Original behavior (immediate if price favorable)
                Price limit_price = o.expected_price;
                if (limit_price == 0) {
                    // Fallback: use current mid as limit
                    limit_price = (bid + ask) / 2;
                }

                bool can_fill = false;
                if (o.side == Side::Buy) {
                    // Buy limit: fills when ask <= limit_price
                    if (ask <= limit_price) {
                        fill_price = limit_price;  // Fill at limit price (no slippage)
                        can_fill = true;
                    }
                } else {
                    // Sell limit: fills when bid >= limit_price
                    if (bid >= limit_price) {
                        fill_price = limit_price;  // Fill at limit price (no slippage)
                        can_fill = true;
                    }
                }

                if (can_fill) {
                    if (on_fill_) on_fill_(o.symbol, o.id, o.side, o.qty, fill_price);
                    total_fills_++;
                } else {
                    // Limit order not yet fillable, keep pending
                    remaining.push_back(o);
                }
            }
        }
        pending_ = std::move(remaining);
    }

    void set_fill_callback(FillCallback cb) { on_fill_ = std::move(cb); }
    void set_slippage_callback(SlippageCallback cb) { on_slippage_ = std::move(cb); }
    uint64_t total_orders() const { return total_orders_; }
    uint64_t total_fills() const { return total_fills_; }
    double total_slippage() const { return total_slippage_; }

private:
    struct Order {
        Symbol symbol;
        OrderId id;
        Side side;
        double qty;            // double to support fractional crypto quantities (e.g., 0.01 BTC)
        Price expected_price;  // For limit: limit price, for market: expected fill
        bool is_market;        // true = market (slippage), false = limit (no slippage)
    };

    uint64_t current_time_ns() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
    }

    OrderId next_id_;
    uint64_t total_orders_;
    uint64_t total_fills_;
    const ipc::SharedConfig* config_;
    double total_slippage_;
    std::vector<Order> pending_;
    FillCallback on_fill_;
    SlippageCallback on_slippage_;

    // Queue simulation
    bool use_queue_sim_;
    Quantity default_queue_depth_;
    QueueFillDetector queue_detector_;
};

}  // namespace paper
}  // namespace hft
