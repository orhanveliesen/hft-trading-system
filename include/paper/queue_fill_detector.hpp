#pragma once

#include "../types.hpp"
#include <unordered_map>
#include <map>
#include <vector>
#include <deque>
#include <functional>
#include <tuple>
#include <cmath>

namespace hft {
namespace paper {

/**
 * Fill Confidence Levels
 */
enum class FillConfidence : uint8_t {
    Confirmed,      // 100% - Order behind us got filled
    VeryLikely,     // 90%  - Most of queue ahead cleared
    Likely,         // 70%  - Significant volume traded
    Possible,       // 50%  - Price touched our level
    Unlikely        // 20%  - Still waiting in queue
};

inline const char* confidence_to_string(FillConfidence conf) {
    switch (conf) {
        case FillConfidence::Confirmed:  return "CONFIRMED";
        case FillConfidence::VeryLikely: return "VERY_LIKELY";
        case FillConfidence::Likely:     return "LIKELY";
        case FillConfidence::Possible:   return "POSSIBLE";
        case FillConfidence::Unlikely:   return "UNLIKELY";
        default: return "UNKNOWN";
    }
}

inline double confidence_weight(FillConfidence conf) {
    switch (conf) {
        case FillConfidence::Confirmed:  return 1.0;
        case FillConfidence::VeryLikely: return 0.85;
        case FillConfidence::Likely:     return 0.65;
        case FillConfidence::Possible:   return 0.40;
        case FillConfidence::Unlikely:   return 0.10;
        default: return 0.0;
    }
}

/**
 * Queue Entry - Represents an order in the queue
 */
struct QueueEntry {
    uint64_t sequence;      // Exchange sequence number (order arrival time)
    Quantity quantity;
    Quantity remaining;
    bool is_ours;
    OrderId our_order_id;   // Only valid if is_ours
};

/**
 * Price Level Queue State
 */
struct PriceLevelQueue {
    Price price;
    Side side;
    std::deque<QueueEntry> queue;

    // Our order info
    bool has_our_order = false;
    size_t our_position = 0;        // Index in queue
    uint64_t our_sequence = 0;
    Quantity our_original_qty = 0;
    Quantity our_remaining = 0;
    OrderId our_order_id = 0;

    // Tracking
    Quantity total_ahead_at_entry = 0;  // Queue depth when we joined
    Quantity volume_traded = 0;          // Total volume traded at this level

    Quantity queue_ahead() const {
        if (!has_our_order || our_position == 0) return 0;

        Quantity ahead = 0;
        for (size_t i = 0; i < our_position && i < queue.size(); ++i) {
            ahead += queue[i].remaining;
        }
        return ahead;
    }
};

/**
 * Fill Result
 */
struct FillResult {
    bool filled = false;
    FillConfidence confidence = FillConfidence::Unlikely;
    Quantity fill_quantity = 0;
    Price fill_price = 0;
    uint64_t fill_time_ns = 0;

    // For stats
    uint64_t queue_wait_ns = 0;     // Time spent in queue
    Quantity queue_ahead_at_fill = 0;
};

/**
 * Paper Order State
 */
struct PaperOrderState {
    OrderId id;
    Symbol symbol;
    Side side;
    Price price;
    Quantity quantity;
    Quantity filled = 0;
    uint64_t submit_time_ns = 0;
    uint64_t sequence = 0;
    bool is_active = true;
};

/**
 * Queue Fill Detector Configuration
 */
struct QueueFillDetectorConfig {
    bool pessimistic_mode = true;   // Only count confirmed fills
    bool track_probabilistic = true; // Also track likely fills for stats
    double partial_fill_threshold = 0.9;  // VeryLikely when 90% of queue traded
};

/**
 * Queue-Based Fill Detector (Pessimistic)
 *
 * Uses the pessimistic approach:
 * - Only confirms fill when we have PROOF
 * - Proof = an order AFTER us in queue got filled
 * - More conservative = more realistic results
 *
 * Also tracks probabilistic fills for comparison.
 */
class QueueFillDetector {
public:
    using FillCallback = std::function<void(OrderId, const FillResult&)>;
    using Config = QueueFillDetectorConfig;

    explicit QueueFillDetector(const Config& config = Config())
        : config_(config)
        , next_sequence_(1)
    {}

    /**
     * Register our order
     */
    void register_order(OrderId id, Symbol symbol, Side side,
                       Price price, Quantity qty, uint64_t timestamp_ns) {
        // Create order state
        PaperOrderState order{
            .id = id,
            .symbol = symbol,
            .side = side,
            .price = price,
            .quantity = qty,
            .filled = 0,
            .submit_time_ns = timestamp_ns,
            .sequence = next_sequence_++,
            .is_active = true
        };

        orders_[id] = order;

        // Add to price level queue
        auto key = make_key(symbol, price, side);
        auto& level = levels_[key];
        level.price = price;
        level.side = side;

        // Calculate queue position
        level.our_position = level.queue.size();
        level.our_sequence = order.sequence;
        level.our_original_qty = qty;
        level.our_remaining = qty;
        level.our_order_id = id;
        level.has_our_order = true;
        level.total_ahead_at_entry = calculate_total_remaining(level);

        // Add our entry to queue
        level.queue.push_back(QueueEntry{
            .sequence = order.sequence,
            .quantity = qty,
            .remaining = qty,
            .is_ours = true,
            .our_order_id = id
        });

        order_to_level_[id] = key;
    }

    /**
     * L2 Update - Track queue changes
     * Call this when order book level changes
     */
    void on_l2_update(Symbol symbol, Side side, Price price,
                     Quantity old_size, Quantity new_size,
                     uint64_t timestamp_ns) {
        auto key = make_key(symbol, price, side);
        auto it = levels_.find(key);
        if (it == levels_.end()) return;

        auto& level = it->second;
        int64_t delta = static_cast<int64_t>(new_size) - static_cast<int64_t>(old_size);

        if (delta > 0) {
            // New order(s) added - goes to back of queue
            level.queue.push_back(QueueEntry{
                .sequence = next_sequence_++,
                .quantity = static_cast<Quantity>(delta),
                .remaining = static_cast<Quantity>(delta),
                .is_ours = false,
                .our_order_id = 0
            });
        } else if (delta < 0) {
            // Order(s) removed - could be cancel or fill
            // We'll get more info from on_trade
            // For now, just track the reduction
            Quantity removed = static_cast<Quantity>(-delta);
            remove_from_front(level, removed, timestamp_ns);
        }
    }

    /**
     * Trade Event - Key for fill detection
     *
     * @param aggressor_side The side that initiated the trade (taker)
     * @param passive_sequence Sequence of the passive (maker) order if known
     */
    void on_trade(Symbol symbol, Price price, Quantity qty,
                 Side aggressor_side, uint64_t timestamp_ns,
                 uint64_t passive_sequence = 0) {

        // Passive side is opposite of aggressor
        Side passive_side = (aggressor_side == Side::Buy) ? Side::Sell : Side::Buy;

        auto key = make_key(symbol, price, passive_side);
        auto it = levels_.find(key);
        if (it == levels_.end()) return;

        auto& level = it->second;
        level.volume_traded += qty;

        if (!level.has_our_order) return;

        // PESSIMISTIC CHECK: Did an order AFTER us get filled?
        if (passive_sequence > 0 && passive_sequence > level.our_sequence) {
            // ORDER AFTER US GOT FILLED = WE ARE DEFINITELY FILLED
            confirm_fill(level, timestamp_ns);
            return;
        }

        // Process trade through queue (FIFO)
        Quantity remaining_trade = qty;
        bool reached_us = false;
        bool passed_us = false;

        for (size_t i = 0; i < level.queue.size() && remaining_trade > 0; ++i) {
            auto& entry = level.queue[i];

            if (entry.is_ours) {
                reached_us = true;
            }

            Quantity fill_this = std::min(remaining_trade, entry.remaining);
            entry.remaining -= fill_this;
            remaining_trade -= fill_this;

            if (entry.is_ours && fill_this > 0) {
                // We got some fill (FIFO based)
                // But in pessimistic mode, we only count it when confirmed
                level.our_remaining -= fill_this;
            }

            if (!entry.is_ours && reached_us && fill_this > 0) {
                // Order AFTER us got filled while we still have remaining
                // This is weird but possible with partial fills
                passed_us = true;
            }
        }

        // Clean up filled entries
        cleanup_filled_entries(level);

        // PESSIMISTIC CONFIRMATION
        if (passed_us || level.our_remaining == 0) {
            confirm_fill(level, timestamp_ns);
        }

        // PROBABILISTIC CHECK (for stats, not for actual fill)
        if (config_.track_probabilistic) {
            check_probabilistic_fill(level, timestamp_ns);
        }
    }

    /**
     * Cancel our order
     */
    void cancel_order(OrderId id) {
        auto it = orders_.find(id);
        if (it == orders_.end()) return;

        it->second.is_active = false;

        auto level_it = order_to_level_.find(id);
        if (level_it != order_to_level_.end()) {
            auto& level = levels_[level_it->second];
            level.has_our_order = false;

            // Remove from queue
            for (auto qit = level.queue.begin(); qit != level.queue.end(); ++qit) {
                if (qit->is_ours && qit->our_order_id == id) {
                    level.queue.erase(qit);
                    break;
                }
            }
        }
    }

    /**
     * Get current fill estimate
     */
    FillResult get_fill_estimate(OrderId id) const {
        auto order_it = orders_.find(id);
        if (order_it == orders_.end()) {
            return FillResult{};
        }

        auto level_it = order_to_level_.find(id);
        if (level_it == order_to_level_.end()) {
            return FillResult{};
        }

        auto levels_it = levels_.find(level_it->second);
        if (levels_it == levels_.end()) {
            return FillResult{};
        }

        const auto& order = order_it->second;
        const auto& level = levels_it->second;

        return calculate_fill_estimate(order, level);
    }

    void set_fill_callback(FillCallback cb) { on_fill_ = std::move(cb); }

    // Statistics
    size_t active_orders() const {
        size_t count = 0;
        for (const auto& [id, order] : orders_) {
            if (order.is_active) count++;
        }
        return count;
    }

private:
    using LevelKey = std::tuple<Symbol, Price, Side>;

    Config config_;
    uint64_t next_sequence_;

    std::unordered_map<OrderId, PaperOrderState> orders_;
    std::map<LevelKey, PriceLevelQueue> levels_;
    std::unordered_map<OrderId, LevelKey> order_to_level_;

    FillCallback on_fill_;

    static LevelKey make_key(Symbol s, Price p, Side side) {
        return std::make_tuple(s, p, side);
    }

    Quantity calculate_total_remaining(const PriceLevelQueue& level) const {
        Quantity total = 0;
        for (const auto& entry : level.queue) {
            if (!entry.is_ours) {
                total += entry.remaining;
            }
        }
        return total;
    }

    void remove_from_front(PriceLevelQueue& level, Quantity qty,
                          uint64_t timestamp_ns) {
        Quantity remaining = qty;

        while (remaining > 0 && !level.queue.empty()) {
            auto& front = level.queue.front();

            if (front.remaining <= remaining) {
                remaining -= front.remaining;

                if (front.is_ours) {
                    // Our order got removed (filled or cancelled)
                    level.our_remaining = 0;
                    confirm_fill(level, timestamp_ns);
                }

                level.queue.pop_front();

                // Update our position
                if (level.has_our_order && level.our_position > 0) {
                    level.our_position--;
                }
            } else {
                front.remaining -= remaining;
                remaining = 0;
            }
        }
    }

    void cleanup_filled_entries(PriceLevelQueue& level) {
        while (!level.queue.empty() && level.queue.front().remaining == 0) {
            if (level.queue.front().is_ours) {
                // Don't remove our entry, just mark as filled
                break;
            }
            level.queue.pop_front();
            if (level.has_our_order && level.our_position > 0) {
                level.our_position--;
            }
        }
    }

    void confirm_fill(PriceLevelQueue& level, uint64_t timestamp_ns) {
        if (!level.has_our_order) return;

        auto order_it = orders_.find(level.our_order_id);
        if (order_it == orders_.end()) return;

        auto& order = order_it->second;
        Quantity fill_qty = order.quantity - order.filled;

        if (fill_qty == 0) return;

        order.filled = order.quantity;
        order.is_active = false;
        level.has_our_order = false;

        FillResult result{
            .filled = true,
            .confidence = FillConfidence::Confirmed,
            .fill_quantity = fill_qty,
            .fill_price = level.price,
            .fill_time_ns = timestamp_ns,
            .queue_wait_ns = timestamp_ns - order.submit_time_ns,
            .queue_ahead_at_fill = 0
        };

        if (on_fill_) {
            on_fill_(order.id, result);
        }
    }

    void check_probabilistic_fill(PriceLevelQueue& level,
                                  uint64_t timestamp_ns) {
        if (!level.has_our_order) return;

        // Calculate fill probability based on volume traded
        Quantity ahead = level.queue_ahead();
        double fill_ratio = (level.total_ahead_at_entry > 0)
            ? static_cast<double>(level.volume_traded) / level.total_ahead_at_entry
            : 0.0;

        FillConfidence conf;
        if (fill_ratio >= config_.partial_fill_threshold) {
            conf = FillConfidence::VeryLikely;
        } else if (fill_ratio >= 0.5) {
            conf = FillConfidence::Likely;
        } else if (level.volume_traded > 0) {
            conf = FillConfidence::Possible;
        } else {
            conf = FillConfidence::Unlikely;
        }

        // Store for stats (don't trigger callback in pessimistic mode)
        probabilistic_estimates_[level.our_order_id] = FillResult{
            .filled = (conf == FillConfidence::VeryLikely),
            .confidence = conf,
            .fill_quantity = level.our_original_qty,
            .fill_price = level.price,
            .fill_time_ns = timestamp_ns,
            .queue_wait_ns = timestamp_ns - orders_[level.our_order_id].submit_time_ns,
            .queue_ahead_at_fill = ahead
        };
    }

    FillResult calculate_fill_estimate(const PaperOrderState& order,
                                       const PriceLevelQueue& level) const {
        if (order.filled >= order.quantity) {
            return FillResult{
                .filled = true,
                .confidence = FillConfidence::Confirmed,
                .fill_quantity = order.quantity,
                .fill_price = level.price
            };
        }

        Quantity ahead = level.queue_ahead();
        double fill_ratio = (level.total_ahead_at_entry > 0)
            ? static_cast<double>(level.volume_traded) / level.total_ahead_at_entry
            : 0.0;

        FillConfidence conf;
        if (fill_ratio >= config_.partial_fill_threshold) {
            conf = FillConfidence::VeryLikely;
        } else if (fill_ratio >= 0.5) {
            conf = FillConfidence::Likely;
        } else if (level.volume_traded > 0) {
            conf = FillConfidence::Possible;
        } else {
            conf = FillConfidence::Unlikely;
        }

        return FillResult{
            .filled = false,
            .confidence = conf,
            .fill_quantity = 0,
            .fill_price = level.price,
            .queue_ahead_at_fill = ahead
        };
    }

    std::unordered_map<OrderId, FillResult> probabilistic_estimates_;
};

/**
 * Paper Trading Statistics
 */
struct PaperTradingStats {
    uint64_t total_orders = 0;
    uint64_t confirmed_fills = 0;
    uint64_t likely_fills = 0;
    uint64_t possible_fills = 0;

    double confirmed_pnl = 0.0;
    double likely_pnl = 0.0;
    double possible_pnl = 0.0;

    uint64_t total_queue_wait_ns = 0;
    uint64_t max_queue_wait_ns = 0;

    void record_fill(const FillResult& result, double pnl) {
        switch (result.confidence) {
            case FillConfidence::Confirmed:
                confirmed_fills++;
                confirmed_pnl += pnl;
                break;
            case FillConfidence::VeryLikely:
            case FillConfidence::Likely:
                likely_fills++;
                likely_pnl += pnl;
                break;
            case FillConfidence::Possible:
            case FillConfidence::Unlikely:
                possible_fills++;
                possible_pnl += pnl;
                break;
        }

        if (result.filled) {
            total_queue_wait_ns += result.queue_wait_ns;
            max_queue_wait_ns = std::max(max_queue_wait_ns, result.queue_wait_ns);
        }
    }

    double pessimistic_pnl() const {
        return confirmed_pnl;
    }

    double expected_pnl() const {
        return confirmed_pnl + likely_pnl * 0.7;
    }

    double optimistic_pnl() const {
        return confirmed_pnl + likely_pnl + possible_pnl;
    }

    double avg_queue_wait_ms() const {
        if (confirmed_fills == 0) return 0;
        return static_cast<double>(total_queue_wait_ns) / confirmed_fills / 1'000'000.0;
    }
};

}  // namespace paper
}  // namespace hft
