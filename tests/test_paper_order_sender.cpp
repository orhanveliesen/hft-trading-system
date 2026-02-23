#include "../include/ipc/shared_config.hpp"
#include "../include/paper/queue_fill_detector.hpp"
#include "../include/risk/enhanced_risk_manager.hpp"
#include "../include/types.hpp"

#include <algorithm>
#include <cassert>
#include <functional>
#include <iostream>
#include <vector>

using namespace hft;
using namespace hft::paper;

#define TEST(name) void name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "  " << #name << "... ";                                                                          \
        name();                                                                                                        \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

#define ASSERT_EQ(a, b) assert((a) == (b))
#define ASSERT_TRUE(x) assert(x)
#define ASSERT_FALSE(x) assert(!(x))
#define ASSERT_GT(a, b) assert((a) > (b))
#define ASSERT_GE(a, b) assert((a) >= (b))
#define ASSERT_LE(a, b) assert((a) <= (b))
#define ASSERT_NE(a, b) assert((a) != (b))

// ============================================
// PaperOrderSender - Testable version with QueueFillDetector integration
// This mirrors the class in trader.cpp but adds queue simulation
// ============================================

class PaperOrderSender {
public:
    static constexpr OrderId PAPER_ID_MASK = 0x8000000000000000ULL;
    static constexpr double DEFAULT_SLIPPAGE_BPS = 5.0;

    enum class Event { Accepted, Filled, Cancelled, Rejected };

    using FillCallback = std::function<void(Symbol, OrderId, Side, double qty, Price)>;
    using SlippageCallback = std::function<void(double)>;

    PaperOrderSender()
        : next_id_(1), total_orders_(0), total_fills_(0), config_(nullptr), total_slippage_(0), use_queue_sim_(false),
          default_queue_depth_(0) {}

    void set_config(const ipc::SharedConfig* config) { config_ = config; }

    // Queue simulation configuration
    void enable_queue_simulation(bool enable) { use_queue_sim_ = enable; }

    void set_default_queue_depth(Quantity depth) { default_queue_depth_ = depth; }

    // Feed trade data to queue detector (for queue position updates)
    void on_trade(Symbol symbol, Price price, Quantity qty, Side aggressor_side, uint64_t timestamp_ns) {
        if (use_queue_sim_) {
            queue_detector_.on_trade(symbol, price, qty, aggressor_side, timestamp_ns);
        }
    }

    // 5-param version with expected_price for slippage tracking
    bool send_order(Symbol symbol, Side side, double qty, Price expected_price, bool is_market) {
        OrderId id = PAPER_ID_MASK | next_id_++;
        total_orders_++;

        // Register limit orders with QueueFillDetector if queue simulation enabled
        if (!is_market && use_queue_sim_) {
            queue_detector_.register_order(id, symbol, side, expected_price, static_cast<Quantity>(qty),
                                           current_time_ns());

            if (default_queue_depth_ > 0) {
                queue_detector_.set_initial_queue_depth(symbol, side, expected_price, default_queue_depth_);
            }
        }

        pending_.push_back({symbol, id, side, qty, expected_price, is_market});
        return true;
    }

    // 4-param backward-compatible version
    bool send_order(Symbol symbol, Side side, double qty, bool is_market) {
        return send_order(symbol, side, qty, 0, is_market);
    }

    bool cancel_order(Symbol /*symbol*/, OrderId id) {
        auto it = std::find_if(pending_.begin(), pending_.end(), [id](const Order& o) { return o.id == id; });
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
        double slippage_bps = DEFAULT_SLIPPAGE_BPS;
        if (config_) {
            double cfg_slippage = config_->slippage_bps();
            if (cfg_slippage > 0) {
                slippage_bps = cfg_slippage;
            }
        }
        double slippage_rate = slippage_bps / 10000.0;

        std::vector<Order> remaining;
        for (auto& o : pending_) {
            if (o.symbol != symbol) {
                remaining.push_back(o);
                continue;
            }

            Price fill_price;

            if (o.is_market) {
                // MARKET ORDER: Fill immediately with slippage (unchanged behavior)
                Price base_price = o.expected_price;
                if (base_price == 0) {
                    base_price = (o.side == Side::Buy) ? ask : bid;
                }

                double slippage_amount = static_cast<double>(base_price) * slippage_rate;
                if (o.side == Side::Buy) {
                    fill_price = base_price + static_cast<Price>(slippage_amount);
                } else {
                    fill_price = base_price - static_cast<Price>(slippage_amount);
                }

                double slippage_cost = slippage_amount * o.qty / risk::PRICE_SCALE;
                total_slippage_ += slippage_cost;
                if (on_slippage_)
                    on_slippage_(slippage_cost);

                if (on_fill_)
                    on_fill_(o.symbol, o.id, o.side, o.qty, fill_price);
                total_fills_++;

            } else if (use_queue_sim_) {
                // LIMIT ORDER WITH QUEUE SIMULATION: Check if queue cleared
                auto result = queue_detector_.get_fill_estimate(o.id);
                if (result.filled && result.confidence == FillConfidence::Confirmed) {
                    // Queue cleared, fill at limit price
                    fill_price = o.expected_price;
                    if (on_fill_)
                        on_fill_(o.symbol, o.id, o.side, o.qty, fill_price);
                    total_fills_++;
                } else {
                    // Still in queue, keep pending
                    remaining.push_back(o);
                }

            } else {
                // LIMIT ORDER WITHOUT QUEUE: Original behavior (immediate if price favorable)
                Price limit_price = o.expected_price;
                if (limit_price == 0) {
                    limit_price = (bid + ask) / 2;
                }

                bool can_fill = false;
                if (o.side == Side::Buy) {
                    if (ask <= limit_price) {
                        fill_price = limit_price;
                        can_fill = true;
                    }
                } else {
                    if (bid >= limit_price) {
                        fill_price = limit_price;
                        can_fill = true;
                    }
                }

                if (can_fill) {
                    if (on_fill_)
                        on_fill_(o.symbol, o.id, o.side, o.qty, fill_price);
                    total_fills_++;
                } else {
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
    size_t pending_count() const { return pending_.size(); }

    // For testing: set current time
    void set_current_time(uint64_t ns) { current_time_ = ns; }

private:
    struct Order {
        Symbol symbol;
        OrderId id;
        Side side;
        double qty;
        Price expected_price;
        bool is_market;
    };

    uint64_t current_time_ns() const { return current_time_; }

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
    uint64_t current_time_ = 1000;
};


// ============================================
// Test: Queue simulation disabled = original behavior (immediate fill)
// ============================================
TEST(test_queue_sim_disabled_fills_immediately) {
    PaperOrderSender sender;
    // Queue sim OFF by default

    int fill_count = 0;
    Price fill_price = 0;
    sender.set_fill_callback([&](Symbol, OrderId, Side, double, Price p) {
        fill_count++;
        fill_price = p;
    });

    // Limit buy at ask price (ask = 100000000, which equals limit)
    constexpr Price ASK = 100000000;
    constexpr Price BID = 99000000;
    sender.send_order(0, Side::Buy, 1.0, ASK, false); // Limit order at ask
    sender.process_fills(0, BID, ASK);

    // Should fill immediately (old behavior) since ask <= limit_price
    ASSERT_EQ(fill_count, 1);
    ASSERT_EQ(fill_price, ASK);
}

// ============================================
// Test: Queue simulation enabled = limit order waits in queue
// ============================================
TEST(test_queue_sim_enabled_waits_in_queue) {
    PaperOrderSender sender;
    sender.enable_queue_simulation(true);
    sender.set_default_queue_depth(1000); // 1000 units ahead

    int fill_count = 0;
    sender.set_fill_callback([&](Symbol, OrderId, Side, double, Price) { fill_count++; });

    constexpr Price ASK = 100000000;
    constexpr Price BID = 99000000;
    sender.send_order(0, Side::Buy, 1.0, ASK, false);
    sender.process_fills(0, BID, ASK);

    // Should NOT fill - queue not cleared
    ASSERT_EQ(fill_count, 0);
    ASSERT_EQ(sender.pending_count(), 1u);
}

// ============================================
// Test: Queue simulation + trades = fill after queue clears
// ============================================
TEST(test_queue_sim_fills_after_trades) {
    PaperOrderSender sender;
    sender.enable_queue_simulation(true);
    sender.set_default_queue_depth(1000);

    int fill_count = 0;
    sender.set_fill_callback([&](Symbol, OrderId, Side, double, Price) { fill_count++; });

    constexpr Price LIMIT_PRICE = 100000000;
    constexpr Price BID = 99000000;
    constexpr Price ASK = 100000000;

    sender.send_order(0, Side::Buy, 1.0, LIMIT_PRICE, false);

    // Initially no fill
    sender.process_fills(0, BID, ASK);
    ASSERT_EQ(fill_count, 0);

    // Feed trades to clear queue (1000 queue ahead + 1 for our order = 1001)
    // aggressor = Sell means they're hitting bids, we're passive on buy side
    sender.on_trade(0, LIMIT_PRICE, 1001, Side::Sell, 2000);

    sender.process_fills(0, BID, ASK);

    // Now should be filled (queue cleared)
    ASSERT_EQ(fill_count, 1);
    ASSERT_EQ(sender.pending_count(), 0u);
}

// ============================================
// Test: Market orders always bypass queue
// ============================================
TEST(test_market_order_bypasses_queue) {
    PaperOrderSender sender;
    sender.enable_queue_simulation(true);
    sender.set_default_queue_depth(10000); // Big queue

    int fill_count = 0;
    sender.set_fill_callback([&](Symbol, OrderId, Side, double, Price) { fill_count++; });

    constexpr Price ASK = 100000000;
    constexpr Price BID = 99000000;
    sender.send_order(0, Side::Buy, 1.0, ASK, true); // MARKET order
    sender.process_fills(0, BID, ASK);

    // Market order ignores queue, fills immediately
    ASSERT_EQ(fill_count, 1);
}

// ============================================
// Test: Slippage still applied to market orders with queue sim
// ============================================
TEST(test_market_order_slippage_with_queue_sim) {
    PaperOrderSender sender;
    sender.enable_queue_simulation(true);

    Price filled_price = 0;
    sender.set_fill_callback([&](Symbol, OrderId, Side, double, Price p) { filled_price = p; });

    constexpr Price ASK = 100000000;
    constexpr Price BID = 99000000;
    sender.send_order(0, Side::Buy, 1.0, ASK, true); // MARKET order
    sender.process_fills(0, BID, ASK);

    // Should have slippage (fill > expected for buy)
    // Default slippage is 5 bps = 0.05% = ASK * 0.0005 = 50000
    ASSERT_GT(filled_price, ASK);
}

// ============================================
// Test: Sell side limit order with queue simulation
// ============================================
TEST(test_sell_limit_queue_sim) {
    PaperOrderSender sender;
    sender.enable_queue_simulation(true);
    sender.set_default_queue_depth(500);

    int fill_count = 0;
    Price fill_price = 0;
    sender.set_fill_callback([&](Symbol, OrderId, Side, double, Price p) {
        fill_count++;
        fill_price = p;
    });

    constexpr Price LIMIT_PRICE = 99000000; // Sell at bid
    constexpr Price BID = 99000000;
    constexpr Price ASK = 100000000;

    sender.send_order(0, Side::Sell, 1.0, LIMIT_PRICE, false);

    // Initially no fill
    sender.process_fills(0, BID, ASK);
    ASSERT_EQ(fill_count, 0);

    // Feed trades: 500 queue ahead + 1 for our order = 501
    // aggressor = Buy means they're lifting offers, we're passive on sell side
    sender.on_trade(0, LIMIT_PRICE, 501, Side::Buy, 2000);

    sender.process_fills(0, BID, ASK);

    // Now should be filled
    ASSERT_EQ(fill_count, 1);
    ASSERT_EQ(fill_price, LIMIT_PRICE);
}

// ============================================
// Test: Cancel removes order from queue
// ============================================
TEST(test_cancel_removes_from_queue) {
    PaperOrderSender sender;
    sender.enable_queue_simulation(true);
    sender.set_default_queue_depth(1000);

    int fill_count = 0;
    sender.set_fill_callback([&](Symbol, OrderId, Side, double, Price) { fill_count++; });

    constexpr Price LIMIT_PRICE = 100000000;
    sender.send_order(0, Side::Buy, 1.0, LIMIT_PRICE, false);
    ASSERT_EQ(sender.pending_count(), 1u);

    // Cancel the order
    OrderId order_id = PaperOrderSender::PAPER_ID_MASK | 1;
    bool cancelled = sender.cancel_order(0, order_id);
    ASSERT_TRUE(cancelled);
    ASSERT_EQ(sender.pending_count(), 0u);

    // Feed trades that would have filled it
    sender.on_trade(0, LIMIT_PRICE, 1000, Side::Sell, 2000);

    // Process fills - should not fill cancelled order
    sender.process_fills(0, 99000000, 100000000);
    ASSERT_EQ(fill_count, 0);
}

// ============================================
// Test: Multiple orders at same price level
// ============================================
TEST(test_multiple_orders_queue_fifo) {
    PaperOrderSender sender;
    sender.enable_queue_simulation(true);
    sender.set_default_queue_depth(0); // No depth ahead, first order at front

    std::vector<OrderId> filled_ids;
    sender.set_fill_callback([&](Symbol, OrderId id, Side, double, Price) { filled_ids.push_back(id); });

    constexpr Price LIMIT_PRICE = 100000000;

    // Submit two orders at same price
    sender.send_order(0, Side::Buy, 1.0, LIMIT_PRICE, false); // Order 1
    sender.send_order(0, Side::Buy, 1.0, LIMIT_PRICE, false); // Order 2

    // Trade for only first order quantity
    sender.on_trade(0, LIMIT_PRICE, 1, Side::Sell, 2000);

    sender.process_fills(0, 99000000, 100000000);

    // First order should be filled
    ASSERT_EQ(filled_ids.size(), 1u);
    ASSERT_EQ(filled_ids[0], PaperOrderSender::PAPER_ID_MASK | 1);

    // Second order still pending
    ASSERT_EQ(sender.pending_count(), 1u);
}

// ============================================
// Test: Stats tracking with queue simulation
// ============================================
TEST(test_stats_with_queue_sim) {
    PaperOrderSender sender;
    sender.enable_queue_simulation(true);
    sender.set_default_queue_depth(100);

    double total_slippage = 0;
    sender.set_slippage_callback([&](double slip) { total_slippage += slip; });

    sender.set_fill_callback([](Symbol, OrderId, Side, double, Price) {});

    constexpr Price LIMIT_PRICE = 100000000;

    // Submit market order (has slippage)
    sender.send_order(0, Side::Buy, 1.0, LIMIT_PRICE, true);
    sender.process_fills(0, 99000000, 100000000);

    // Submit limit order (no slippage when filled)
    // Trade 101 = 100 queue ahead + 1 for our order
    sender.send_order(0, Side::Buy, 1.0, LIMIT_PRICE, false);
    sender.on_trade(0, LIMIT_PRICE, 101, Side::Sell, 2000);
    sender.process_fills(0, 99000000, 100000000);

    ASSERT_EQ(sender.total_orders(), 2u);
    ASSERT_EQ(sender.total_fills(), 2u);
    ASSERT_GT(total_slippage, 0); // Only market order had slippage
}

// ============================================
// Test: Queue depth estimation affects fill behavior
// ============================================
TEST(test_queue_depth_affects_fill_time) {
    PaperOrderSender sender1;
    sender1.enable_queue_simulation(true);
    sender1.set_default_queue_depth(100);

    PaperOrderSender sender2;
    sender2.enable_queue_simulation(true);
    sender2.set_default_queue_depth(1000);

    int fills1 = 0, fills2 = 0;
    sender1.set_fill_callback([&](Symbol, OrderId, Side, double, Price) { fills1++; });
    sender2.set_fill_callback([&](Symbol, OrderId, Side, double, Price) { fills2++; });

    constexpr Price LIMIT_PRICE = 100000000;

    sender1.send_order(0, Side::Buy, 1.0, LIMIT_PRICE, false);
    sender2.send_order(0, Side::Buy, 1.0, LIMIT_PRICE, false);

    // Trade 500 units
    sender1.on_trade(0, LIMIT_PRICE, 500, Side::Sell, 2000);
    sender2.on_trade(0, LIMIT_PRICE, 500, Side::Sell, 2000);

    sender1.process_fills(0, 99000000, 100000000);
    sender2.process_fills(0, 99000000, 100000000);

    // Sender1 should be filled (queue was 100, traded 500)
    ASSERT_EQ(fills1, 1);
    // Sender2 should NOT be filled (queue was 1000, only traded 500)
    ASSERT_EQ(fills2, 0);
}


// ============================================
// Main
// ============================================

int main() {
    std::cout << "\n=== PaperOrderSender Queue Integration Tests ===\n\n";

    std::cout << "Basic Behavior:\n";
    RUN_TEST(test_queue_sim_disabled_fills_immediately);
    RUN_TEST(test_queue_sim_enabled_waits_in_queue);
    RUN_TEST(test_queue_sim_fills_after_trades);

    std::cout << "\nMarket Orders:\n";
    RUN_TEST(test_market_order_bypasses_queue);
    RUN_TEST(test_market_order_slippage_with_queue_sim);

    std::cout << "\nSell Side:\n";
    RUN_TEST(test_sell_limit_queue_sim);

    std::cout << "\nOrder Management:\n";
    RUN_TEST(test_cancel_removes_from_queue);
    RUN_TEST(test_multiple_orders_queue_fifo);

    std::cout << "\nStatistics:\n";
    RUN_TEST(test_stats_with_queue_sim);
    RUN_TEST(test_queue_depth_affects_fill_time);

    std::cout << "\n=== All PaperOrderSender Tests Passed! ===\n";
    return 0;
}
