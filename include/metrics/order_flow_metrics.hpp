#pragma once

#include "../ipc/trade_event.hpp"
#include "../orderbook.hpp"
#include "../types.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace hft {

/**
 * Order flow metric windows
 */
enum class Window {
    SEC_1,  // 1 second
    SEC_5,  // 5 seconds
    SEC_10, // 10 seconds
    SEC_30, // 30 seconds
    MIN_1   // 1 minute
};

/**
 * OrderFlowMetrics - Track order book flow and changes
 *
 * Tracks what CHANGES in the order book over time:
 * - Added/removed volume
 * - Cancel vs fill estimation (correlate with trades)
 * - Book velocity (rate of change)
 * - Level lifetime tracking
 *
 * Performance:
 * - on_trade(): < 100 ns
 * - on_order_book_update(): < 5 μs
 * - get_metrics(): < 1 μs (cached) or < 5 μs (cache miss)
 */
class OrderFlowMetrics {
public:
    OrderFlowMetrics() = default;

    struct Metrics {
        // Added/Removed Volume
        double bid_volume_added = 0.0;
        double ask_volume_added = 0.0;
        double bid_volume_removed = 0.0;
        double ask_volume_removed = 0.0;

        // Cancel Estimation
        double estimated_bid_cancel_volume = 0.0;
        double estimated_ask_cancel_volume = 0.0;
        double cancel_ratio_bid = 0.0;
        double cancel_ratio_ask = 0.0;

        // Book Velocity
        double bid_depth_velocity = 0.0;
        double ask_depth_velocity = 0.0;
        double bid_additions_per_sec = 0.0;
        double ask_additions_per_sec = 0.0;
        double bid_removals_per_sec = 0.0;
        double ask_removals_per_sec = 0.0;

        // Level Lifetime
        double avg_bid_level_lifetime_us = 0.0;
        double avg_ask_level_lifetime_us = 0.0;
        double short_lived_bid_ratio = 0.0;
        double short_lived_ask_ratio = 0.0;

        // Update Frequency
        int book_update_count = 0;
        int bid_level_changes = 0;
        int ask_level_changes = 0;
    };

    /**
     * Process a trade (for cancel/fill correlation)
     */
    void on_trade(const ipc::TradeEvent& trade);

    /**
     * Process order book update (compare prev vs new state)
     */
    void on_order_book_update(const OrderBook& book, uint64_t timestamp_us);

    /**
     * Get metrics for a specific time window
     */
    const Metrics& get_metrics(Window w) const;

    /**
     * Reset all metrics and history
     */
    void reset();

private:
    // Flow event: track added/removed volume per level
    struct FlowEvent {
        Price price;
        double volume_delta;  // positive = added, negative = removed
        double cancel_volume; // for removals, how much was cancelled (vs filled)
        bool is_bid;
        bool is_cancel;       // true if cancel_volume > 0
        bool is_level_change; // true if level changed (for counting)
        uint64_t timestamp_us;
    };

    // Recent trade for cancel/fill correlation
    struct RecentTrade {
        Price price;
        Quantity quantity;
        uint64_t timestamp_us;
    };

    // Level lifetime tracking
    struct LevelLifetime {
        uint64_t birth_us;
        uint64_t death_us;
        bool is_bid;
    };

    // Configuration
    static constexpr size_t MAX_DEPTH_LEVELS = 20; // Max levels per side to track

    // Ring buffers (pre-allocated)
    static constexpr size_t MAX_FLOW_EVENTS = 1 << 14; // 16K events
    static constexpr size_t MAX_RECENT_TRADES = 256;   // 256 trades
    static constexpr size_t MAX_LIFETIMES = 1 << 12;   // 4K lifetimes
    static constexpr size_t FLOW_MASK = MAX_FLOW_EVENTS - 1;
    static constexpr size_t TRADE_MASK = MAX_RECENT_TRADES - 1;
    static constexpr size_t LIFETIME_MASK = MAX_LIFETIMES - 1;
    static constexpr uint64_t CANCEL_CORRELATION_WINDOW_US = 100'000; // 100ms
    static constexpr uint64_t SHORT_LIVED_THRESHOLD_US = 1'000'000;   // 1s

    alignas(64) std::array<FlowEvent, MAX_FLOW_EVENTS> flow_events_;
    size_t flow_head_ = 0;
    size_t flow_tail_ = 0;

    alignas(64) std::array<RecentTrade, MAX_RECENT_TRADES> recent_trades_;
    size_t trade_head_ = 0;
    size_t trade_tail_ = 0;

    alignas(64) std::array<LevelLifetime, MAX_LIFETIMES> lifetimes_;
    size_t lifetime_head_ = 0;
    size_t lifetime_tail_ = 0;

    // Previous book state for delta calculation (flat arrays, cache-friendly)
    struct PriceLevel {
        Price price;
        Quantity quantity;
    };
    struct PriceBirth {
        Price price;
        uint64_t birth_us;
    };

    std::array<PriceLevel, MAX_DEPTH_LEVELS> prev_bid_levels_{};
    std::array<PriceLevel, MAX_DEPTH_LEVELS> prev_ask_levels_{};
    int prev_bid_count_ = 0;
    int prev_ask_count_ = 0;

    // Level lifetime tracking (birth times)
    std::array<PriceBirth, MAX_DEPTH_LEVELS * 2> level_births_{}; // *2 for bid+ask
    int birth_count_ = 0;

    // Depth tracking for velocity
    double prev_bid_depth_ = 0.0;
    double prev_ask_depth_ = 0.0;
    uint64_t prev_timestamp_us_ = 0;

    // Cached metrics
    mutable std::array<Metrics, 5> cached_metrics_{};
    mutable std::array<size_t, 5> cache_tail_position_{SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};

    // Helpers
    static constexpr uint64_t get_window_duration_us(Window w);
    size_t find_window_start(uint64_t window_start_time) const;
    Metrics calculate_metrics(size_t start_idx, size_t end_idx) const;
    bool was_trade_at_price(Price price, uint64_t timestamp_us) const;
    Quantity get_traded_quantity_at_price(Price price, uint64_t timestamp_us) const;
    void add_flow_event(const FlowEvent& event);
    void add_lifetime(const LevelLifetime& lifetime);
    void cleanup_old_trades(uint64_t current_time);

    // Flat array helpers (branchless where possible)
    inline const PriceLevel* find_price(const std::array<PriceLevel, MAX_DEPTH_LEVELS>& levels, int count,
                                        Price price) const;
    inline void add_birth(Price price, uint64_t timestamp_us);
    inline const PriceBirth* find_birth(Price price) const;
    inline void remove_birth(Price price);
};

// ============================================================================
// Implementation
// ============================================================================

constexpr uint64_t OrderFlowMetrics::get_window_duration_us(Window w) {
    switch (w) {
    case Window::SEC_1:
        return 1'000'000;
    case Window::SEC_5:
        return 5'000'000;
    case Window::SEC_10:
        return 10'000'000;
    case Window::SEC_30:
        return 30'000'000;
    case Window::MIN_1:
        return 60'000'000;
    }
    return 1'000'000; // Default 1s
}

inline void OrderFlowMetrics::on_trade(const ipc::TradeEvent& trade) {
    // Extract price from trade event
    Price price = static_cast<Price>(trade.price);
    uint64_t timestamp_us = trade.timestamp_ns / 1000; // Convert ns to us

    // Cleanup old trades (older than 100ms)
    cleanup_old_trades(timestamp_us);

    // Add to recent trades buffer
    Quantity quantity = static_cast<Quantity>(trade.quantity);
    recent_trades_[trade_tail_] = RecentTrade{price, quantity, timestamp_us};

    size_t new_tail = (trade_tail_ + 1) & TRADE_MASK;

    // If advancing tail would overwrite head, advance head too (buffer full)
    if (new_tail == trade_head_) {
        trade_head_ = (trade_head_ + 1) & TRADE_MASK;
    }

    trade_tail_ = new_tail;
}

inline void OrderFlowMetrics::cleanup_old_trades(uint64_t current_time) {
    // Remove trades older than correlation window
    while (trade_head_ != trade_tail_) {
        const auto& trade = recent_trades_[trade_head_];
        if (current_time - trade.timestamp_us <= CANCEL_CORRELATION_WINDOW_US) {
            break;
        }
        trade_head_ = (trade_head_ + 1) & TRADE_MASK;
    }
}

inline void OrderFlowMetrics::on_order_book_update(const OrderBook& book, uint64_t timestamp_us) {
    // Get current book snapshot
    auto snapshot = book.get_snapshot(MAX_DEPTH_LEVELS);

    // Calculate total depth and store current levels in flat arrays
    double current_bid_depth = 0.0;
    double current_ask_depth = 0.0;
    int current_bid_count = snapshot.bid_level_count;
    int current_ask_count = snapshot.ask_level_count;

    std::array<PriceLevel, MAX_DEPTH_LEVELS> current_bid_levels{};
    std::array<PriceLevel, MAX_DEPTH_LEVELS> current_ask_levels{};

    // Extract bid levels
    for (int i = 0; i < current_bid_count; i++) {
        const auto& level = snapshot.bid_levels[i];
        current_bid_levels[i] = PriceLevel{level.price, level.quantity};
        current_bid_depth += static_cast<double>(level.quantity);
    }

    // Extract ask levels
    for (int i = 0; i < current_ask_count; i++) {
        const auto& level = snapshot.ask_levels[i];
        current_ask_levels[i] = PriceLevel{level.price, level.quantity};
        current_ask_depth += static_cast<double>(level.quantity);
    }

    // Track level births
    for (int i = 0; i < current_bid_count; i++) {
        add_birth(current_bid_levels[i].price, timestamp_us);
    }
    for (int i = 0; i < current_ask_count; i++) {
        add_birth(current_ask_levels[i].price, timestamp_us);
    }

    // Compare with previous state and generate flow events
    // Process bid levels (current vs previous)
    for (int i = 0; i < current_bid_count; i++) {
        Price price = current_bid_levels[i].price;
        Quantity current_qty = current_bid_levels[i].quantity;

        const PriceLevel* prev = find_price(prev_bid_levels_, prev_bid_count_, price);
        if (!prev) {
            // New level
            FlowEvent event;
            event.price = price;
            event.volume_delta = static_cast<double>(current_qty);
            event.cancel_volume = 0.0;
            event.is_bid = true;
            event.is_cancel = false;
            event.is_level_change = true;
            event.timestamp_us = timestamp_us;
            add_flow_event(event);
        } else if (current_qty != prev->quantity) {
            // Quantity changed
            double delta =
                static_cast<double>(static_cast<int64_t>(current_qty) - static_cast<int64_t>(prev->quantity));

            // Branchless cancel vs fill estimation
            double removed = std::max(0.0, -delta);
            Quantity traded = get_traded_quantity_at_price(price, timestamp_us);
            double fill_vol = std::min(removed, static_cast<double>(traded));
            double cancel_vol = removed - fill_vol;

            FlowEvent event;
            event.price = price;
            event.volume_delta = delta;
            event.cancel_volume = cancel_vol;
            event.is_bid = true;
            event.is_cancel = (cancel_vol > 0.0);
            event.is_level_change = true;
            event.timestamp_us = timestamp_us;
            add_flow_event(event);
        }
    }

    // Check for removed bid levels
    for (int i = 0; i < prev_bid_count_; i++) {
        Price price = prev_bid_levels_[i].price;
        Quantity prev_qty = prev_bid_levels_[i].quantity;

        const PriceLevel* current = find_price(current_bid_levels, current_bid_count, price);
        if (!current) {
            // Level removed - branchless cancel vs fill estimation
            double removed = static_cast<double>(prev_qty);
            Quantity traded = get_traded_quantity_at_price(price, timestamp_us);
            double fill_vol = std::min(removed, static_cast<double>(traded));
            double cancel_vol = removed - fill_vol;

            FlowEvent event;
            event.price = price;
            event.volume_delta = -removed;
            event.cancel_volume = cancel_vol;
            event.is_bid = true;
            event.is_cancel = (cancel_vol > 0.0);
            event.is_level_change = true;
            event.timestamp_us = timestamp_us;
            add_flow_event(event);

            // Record level lifetime
            const PriceBirth* birth = find_birth(price);
            if (birth) {
                LevelLifetime lifetime;
                lifetime.birth_us = birth->birth_us;
                lifetime.death_us = timestamp_us;
                lifetime.is_bid = true;
                add_lifetime(lifetime);
                remove_birth(price);
            }
        }
    }

    // Process ask levels (current vs previous)
    for (int i = 0; i < current_ask_count; i++) {
        Price price = current_ask_levels[i].price;
        Quantity current_qty = current_ask_levels[i].quantity;

        const PriceLevel* prev = find_price(prev_ask_levels_, prev_ask_count_, price);
        if (!prev) {
            // New level
            FlowEvent event;
            event.price = price;
            event.volume_delta = static_cast<double>(current_qty);
            event.cancel_volume = 0.0;
            event.is_bid = false;
            event.is_cancel = false;
            event.is_level_change = true;
            event.timestamp_us = timestamp_us;
            add_flow_event(event);
        } else if (current_qty != prev->quantity) {
            // Quantity changed
            double delta =
                static_cast<double>(static_cast<int64_t>(current_qty) - static_cast<int64_t>(prev->quantity));

            // Branchless cancel vs fill estimation
            double removed = std::max(0.0, -delta);
            Quantity traded = get_traded_quantity_at_price(price, timestamp_us);
            double fill_vol = std::min(removed, static_cast<double>(traded));
            double cancel_vol = removed - fill_vol;

            FlowEvent event;
            event.price = price;
            event.volume_delta = delta;
            event.cancel_volume = cancel_vol;
            event.is_bid = false;
            event.is_cancel = (cancel_vol > 0.0);
            event.is_level_change = true;
            event.timestamp_us = timestamp_us;
            add_flow_event(event);
        }
    }

    // Check for removed ask levels
    for (int i = 0; i < prev_ask_count_; i++) {
        Price price = prev_ask_levels_[i].price;
        Quantity prev_qty = prev_ask_levels_[i].quantity;

        const PriceLevel* current = find_price(current_ask_levels, current_ask_count, price);
        if (!current) {
            // Level removed - branchless cancel vs fill estimation
            double removed = static_cast<double>(prev_qty);
            Quantity traded = get_traded_quantity_at_price(price, timestamp_us);
            double fill_vol = std::min(removed, static_cast<double>(traded));
            double cancel_vol = removed - fill_vol;

            FlowEvent event;
            event.price = price;
            event.volume_delta = -removed;
            event.cancel_volume = cancel_vol;
            event.is_bid = false;
            event.is_cancel = (cancel_vol > 0.0);
            event.is_level_change = true;
            event.timestamp_us = timestamp_us;
            add_flow_event(event);

            // Record level lifetime
            const PriceBirth* birth = find_birth(price);
            if (birth) {
                LevelLifetime lifetime;
                lifetime.birth_us = birth->birth_us;
                lifetime.death_us = timestamp_us;
                lifetime.is_bid = false;
                add_lifetime(lifetime);
                remove_birth(price);
            }
        }
    }

    // Update previous state (flat array copy, cache-friendly)
    prev_bid_levels_ = current_bid_levels;
    prev_ask_levels_ = current_ask_levels;
    prev_bid_count_ = current_bid_count;
    prev_ask_count_ = current_ask_count;
    prev_bid_depth_ = current_bid_depth;
    prev_ask_depth_ = current_ask_depth;
    prev_timestamp_us_ = timestamp_us;
}

inline void OrderFlowMetrics::add_flow_event(const FlowEvent& event) {
    // Remove events older than 1 minute (longest window)
    constexpr uint64_t ONE_MINUTE_US = 60'000'000;
    while (flow_head_ != flow_tail_) {
        const auto& old_event = flow_events_[flow_head_];
        if (event.timestamp_us - old_event.timestamp_us <= ONE_MINUTE_US) {
            break;
        }
        flow_head_ = (flow_head_ + 1) & FLOW_MASK;
    }

    // Add new event
    flow_events_[flow_tail_] = event;

    size_t new_tail = (flow_tail_ + 1) & FLOW_MASK;

    // If advancing tail would overwrite head, advance head too (buffer full)
    if (new_tail == flow_head_) {
        flow_head_ = (flow_head_ + 1) & FLOW_MASK;
    }

    flow_tail_ = new_tail;
}

inline void OrderFlowMetrics::add_lifetime(const LevelLifetime& lifetime) {
    lifetimes_[lifetime_tail_] = lifetime;

    size_t new_tail = (lifetime_tail_ + 1) & LIFETIME_MASK;

    // If advancing tail would overwrite head, advance head too (buffer full)
    if (new_tail == lifetime_head_) {
        lifetime_head_ = (lifetime_head_ + 1) & LIFETIME_MASK;
    }

    lifetime_tail_ = new_tail;
}

inline bool OrderFlowMetrics::was_trade_at_price(Price price, uint64_t timestamp_us) const {
    // Check if there was a trade at this price within correlation window
    size_t idx = trade_head_;
    while (idx != trade_tail_) {
        const auto& trade = recent_trades_[idx];
        if (trade.price == price) {
            uint64_t time_diff = (timestamp_us >= trade.timestamp_us) ? (timestamp_us - trade.timestamp_us)
                                                                      : (trade.timestamp_us - timestamp_us);
            if (time_diff <= CANCEL_CORRELATION_WINDOW_US) {
                return true;
            }
        }
        idx = (idx + 1) & TRADE_MASK;
    }
    return false;
}

inline Quantity OrderFlowMetrics::get_traded_quantity_at_price(Price price, uint64_t timestamp_us) const {
    // Sum all trades at this price within correlation window
    Quantity total = 0;
    size_t idx = trade_head_;
    while (idx != trade_tail_) {
        const auto& trade = recent_trades_[idx];
        if (trade.price == price) {
            uint64_t time_diff = (timestamp_us >= trade.timestamp_us) ? (timestamp_us - trade.timestamp_us)
                                                                      : (trade.timestamp_us - timestamp_us);
            if (time_diff <= CANCEL_CORRELATION_WINDOW_US) {
                total += trade.quantity;
            }
        }
        idx = (idx + 1) & TRADE_MASK;
    }
    return total;
}

inline size_t OrderFlowMetrics::find_window_start(uint64_t window_start_time) const {
    // Linear search from head to find first event >= window_start_time
    // (Binary search not possible with ring buffer and non-contiguous indices)
    size_t idx = flow_head_;
    size_t count = 0;

    while (idx != flow_tail_) {
        const auto& event = flow_events_[idx];
        if (event.timestamp_us >= window_start_time) {
            return count;
        }
        idx = (idx + 1) & FLOW_MASK;
        count++;
    }

    return count; // All events are before window
}

inline const OrderFlowMetrics::Metricsconst OrderFlowMetrics::Metrics&
OrderFlowMetrics::OrderFlowMetrics::get_metrics(Window w) const {
    size_t window_idx = static_cast<size_t>(w);

    // Check cache validity
    if (cache_tail_position_[window_idx] == flow_tail_) {
        return cached_metrics_[window_idx];
    }

    // Calculate buffer size
    size_t count = (flow_tail_ >= flow_head_) ? (flow_tail_ - flow_head_) : (MAX_FLOW_EVENTS - flow_head_ + flow_tail_);

    if (count == 0) {
        cached_metrics_[window_idx] = Metrics{};
        cache_tail_position_[window_idx] = flow_tail_;
        return cached_metrics_[window_idx];
    }

    // Get most recent event timestamp
    size_t last_idx = (flow_tail_ > 0) ? (flow_tail_ - 1) : (MAX_FLOW_EVENTS - 1);
    uint64_t current_time = flow_events_[last_idx].timestamp_us;

    // Calculate window start
    uint64_t window_us = get_window_duration_us(w);
    uint64_t window_start = (current_time > window_us) ? (current_time - window_us) : 0;

    // Find start index
    size_t start_count = find_window_start(window_start);

    // Calculate and cache
    cached_metrics_[window_idx] = calculate_metrics(start_count, count);
    cache_tail_position_[window_idx] = flow_tail_;

    return cached_metrics_[window_idx];
}

inline OrderFlowMetrics::Metrics OrderFlowMetrics::calculate_metrics(size_t start_count, size_t end_count) const {
    if (start_count >= end_count) {
        return Metrics{};
    }

    Metrics m;

    // Single-pass accumulation (branchless where possible)
    double bid_added = 0.0;
    double ask_added = 0.0;
    double bid_removed = 0.0;
    double ask_removed = 0.0;
    double bid_cancel = 0.0;
    double ask_cancel = 0.0;

    int bid_addition_events = 0;
    int ask_addition_events = 0;
    int bid_removal_events = 0;
    int ask_removal_events = 0;
    int bid_changes = 0;
    int ask_changes = 0;

    uint64_t first_time = 0;
    uint64_t last_time = 0;
    int update_count = 0;

    // Track unique timestamps for update count
    uint64_t prev_event_time = 0;

    for (size_t i = start_count; i < end_count; i++) {
        size_t idx = (flow_head_ + i) & FLOW_MASK;
        const auto& event = flow_events_[idx];

        if (i == start_count) {
            first_time = event.timestamp_us;
        }
        last_time = event.timestamp_us;

        // Count unique update times
        if (event.timestamp_us != prev_event_time) {
            update_count++;
            prev_event_time = event.timestamp_us;
        }

        // Branchless accumulation using sign arithmetic
        int is_bid = event.is_bid ? 1 : 0;
        int is_ask = 1 - is_bid;
        int is_positive = (event.volume_delta > 0) ? 1 : 0;
        int is_negative = 1 - is_positive;

        double abs_delta = std::abs(event.volume_delta);

        bid_added += abs_delta * is_bid * is_positive;
        ask_added += abs_delta * is_ask * is_positive;
        bid_removed += abs_delta * is_bid * is_negative;
        ask_removed += abs_delta * is_ask * is_negative;

        // Cancel estimation (use exact cancel_volume from event)
        bid_cancel += event.cancel_volume * is_bid;
        ask_cancel += event.cancel_volume * is_ask;

        // Count events
        bid_addition_events += is_bid * is_positive;
        ask_addition_events += is_ask * is_positive;
        bid_removal_events += is_bid * is_negative;
        ask_removal_events += is_ask * is_negative;

        // Count level changes
        bid_changes += is_bid * (event.is_level_change ? 1 : 0);
        ask_changes += is_ask * (event.is_level_change ? 1 : 0);
    }

    m.bid_volume_added = bid_added;
    m.ask_volume_added = ask_added;
    m.bid_volume_removed = bid_removed;
    m.ask_volume_removed = ask_removed;
    m.estimated_bid_cancel_volume = bid_cancel;
    m.estimated_ask_cancel_volume = ask_cancel;
    m.book_update_count = update_count;
    m.bid_level_changes = bid_changes;
    m.ask_level_changes = ask_changes;

    // Cancel ratios (branchless)
    m.cancel_ratio_bid = (bid_removed > 0.0) ? (bid_cancel / bid_removed) : 0.0;
    m.cancel_ratio_ask = (ask_removed > 0.0) ? (ask_cancel / ask_removed) : 0.0;

    // Velocity calculations
    double time_span_s = (last_time > first_time) ? (static_cast<double>(last_time - first_time) / 1e6) : 0.0;
    if (time_span_s > 0.0) {
        // Depth velocity (net change per second)
        double bid_net_change = bid_added - bid_removed;
        double ask_net_change = ask_added - ask_removed;
        m.bid_depth_velocity = bid_net_change / time_span_s;
        m.ask_depth_velocity = ask_net_change / time_span_s;

        // Event rates
        m.bid_additions_per_sec = static_cast<double>(bid_addition_events) / time_span_s;
        m.ask_additions_per_sec = static_cast<double>(ask_addition_events) / time_span_s;
        m.bid_removals_per_sec = static_cast<double>(bid_removal_events) / time_span_s;
        m.ask_removals_per_sec = static_cast<double>(ask_removal_events) / time_span_s;
    }

    // Level lifetime metrics (from lifetime buffer)
    double bid_lifetime_sum = 0.0;
    double ask_lifetime_sum = 0.0;
    int bid_lifetime_count = 0;
    int ask_lifetime_count = 0;
    int bid_short_lived = 0;
    int ask_short_lived = 0;

    // Calculate window start time
    uint64_t window_start = (last_time > (last_time - first_time)) ? (last_time - (last_time - first_time)) : 0;

    size_t lt_idx = lifetime_head_;
    while (lt_idx != lifetime_tail_) {
        const auto& lifetime = lifetimes_[lt_idx];

        // Only count lifetimes that ended within the window
        if (lifetime.death_us >= window_start) {
            uint64_t duration = lifetime.death_us - lifetime.birth_us;

            if (lifetime.is_bid) {
                bid_lifetime_sum += static_cast<double>(duration);
                bid_lifetime_count++;
                if (duration < SHORT_LIVED_THRESHOLD_US) {
                    bid_short_lived++;
                }
            } else {
                ask_lifetime_sum += static_cast<double>(duration);
                ask_lifetime_count++;
                if (duration < SHORT_LIVED_THRESHOLD_US) {
                    ask_short_lived++;
                }
            }
        }

        lt_idx = (lt_idx + 1) & LIFETIME_MASK;
    }

    m.avg_bid_level_lifetime_us = (bid_lifetime_count > 0) ? (bid_lifetime_sum / bid_lifetime_count) : 0.0;
    m.avg_ask_level_lifetime_us = (ask_lifetime_count > 0) ? (ask_lifetime_sum / ask_lifetime_count) : 0.0;
    m.short_lived_bid_ratio =
        (bid_lifetime_count > 0) ? (static_cast<double>(bid_short_lived) / bid_lifetime_count) : 0.0;
    m.short_lived_ask_ratio =
        (ask_lifetime_count > 0) ? (static_cast<double>(ask_short_lived) / ask_lifetime_count) : 0.0;

    return m;
}

inline void OrderFlowMetrics::reset() {
    flow_head_ = 0;
    flow_tail_ = 0;
    trade_head_ = 0;
    trade_tail_ = 0;
    lifetime_head_ = 0;
    lifetime_tail_ = 0;
    prev_bid_count_ = 0;
    prev_ask_count_ = 0;
    birth_count_ = 0;
    prev_bid_depth_ = 0.0;
    prev_ask_depth_ = 0.0;
    prev_timestamp_us_ = 0;
    cached_metrics_ = {};
    cache_tail_position_ = {};
}

// Flat array helpers - branchless linear search (cache-friendly for small N)
inline const OrderFlowMetrics::PriceLevel*
OrderFlowMetrics::find_price(const std::array<PriceLevel, MAX_DEPTH_LEVELS>& levels, int count, Price price) const {
    for (int i = 0; i < count; i++) {
        if (levels[i].price == price) {
            return &levels[i];
        }
    }
    return nullptr;
}

inline void OrderFlowMetrics::add_birth(Price price, uint64_t timestamp_us) {
    // Check if already exists
    for (int i = 0; i < birth_count_; i++) {
        if (level_births_[i].price == price) {
            return; // Already tracked
        }
    }
    // Add new birth
    if (birth_count_ < static_cast<int>(MAX_DEPTH_LEVELS * 2)) {
        level_births_[birth_count_++] = PriceBirth{price, timestamp_us};
    }
}

inline const OrderFlowMetrics::PriceBirth* OrderFlowMetrics::find_birth(Price price) const {
    for (int i = 0; i < birth_count_; i++) {
        if (level_births_[i].price == price) {
            return &level_births_[i];
        }
    }
    return nullptr;
}

inline void OrderFlowMetrics::remove_birth(Price price) {
    for (int i = 0; i < birth_count_; i++) {
        if (level_births_[i].price == price) {
            // Swap with last and decrement count
            level_births_[i] = level_births_[birth_count_ - 1];
            birth_count_--;
            return;
        }
    }
}

} // namespace hft
