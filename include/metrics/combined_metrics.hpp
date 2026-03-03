#pragma once

#include "../types.hpp"
#include "order_book_metrics.hpp"
#include "trade_stream_metrics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace hft {

/**
 * CombinedMetrics - Higher-level signals from trade stream + order book
 *
 * Combines TradeStreamMetrics and OrderBookMetrics into:
 * - Trade vs Book: trade_to_depth_ratio
 * - Absorption: absorption_ratio_bid, absorption_ratio_ask
 * - Spread Dynamics: spread_mean, spread_max, spread_min, spread_volatility
 *
 * Windows: 1s, 5s, 10s, 30s, 1min (35 total metrics)
 *
 * Performance: < 1 μs per update()
 */
class CombinedMetrics {
public:
    enum class Window { SEC_1, SEC_5, SEC_10, SEC_30, MIN_1 };

    struct Metrics {
        // Trade vs Book
        double trade_to_depth_ratio = 0.0; // total_trade_volume / (bid_depth_10 + ask_depth_10)

        // Absorption
        double absorption_ratio_bid = 0.0; // sell_volume_at_best_bid / bid_quantity_decrease
        double absorption_ratio_ask = 0.0; // buy_volume_at_best_ask / ask_quantity_decrease

        // Spread Dynamics
        double spread_mean = 0.0;
        double spread_max = 0.0;
        double spread_min = 0.0;
        double spread_volatility = 0.0;
    };

    explicit CombinedMetrics(const TradeStreamMetrics& trade_metrics, const OrderBookMetrics& book_metrics)
        : trade_metrics_(trade_metrics), book_metrics_(book_metrics) {}

    void update(uint64_t timestamp_us);
    const Metrics& get_metrics(Window w) const;
    void reset();

private:
    // References to source metrics
    const TradeStreamMetrics& trade_metrics_;
    const OrderBookMetrics& book_metrics_;

    // Spread history for dynamics calculation
    struct SpreadSample {
        double spread;
        uint64_t timestamp_us;
    };

    static constexpr size_t MAX_SPREAD_SAMPLES = 8192; // Power of 2, ~81s at 100ms updates
    static constexpr size_t MASK = MAX_SPREAD_SAMPLES - 1;

    std::array<SpreadSample, MAX_SPREAD_SAMPLES> spread_history_{};
    size_t spread_head_ = 0;
    size_t spread_tail_ = 0;
    size_t spread_count_ = 0;

    // Quantity decrease history for absorption calculation
    struct QtyDecreaseSample {
        Quantity bid_qty_decrease;
        Quantity ask_qty_decrease;
        uint64_t timestamp_us;
    };

    static constexpr size_t MAX_QTY_SAMPLES = 8192; // Same as spread samples
    static constexpr size_t QTY_MASK = MAX_QTY_SAMPLES - 1;

    std::array<QtyDecreaseSample, MAX_QTY_SAMPLES> qty_decrease_history_{};
    size_t qty_head_ = 0;
    size_t qty_tail_ = 0;
    size_t qty_count_ = 0;

    // Previous book state for detecting changes
    Price prev_best_bid_ = INVALID_PRICE;
    Price prev_best_ask_ = INVALID_PRICE;
    Quantity prev_best_bid_qty_ = 0;
    Quantity prev_best_ask_qty_ = 0;

    // Cached metrics (invalidated on update via generation counter)
    uint64_t update_generation_ = 0;
    mutable std::array<Metrics, 5> cached_metrics_{};
    mutable std::array<uint64_t, 5> cache_generation_{};

    // Helper struct for spread statistics
    struct SpreadStats {
        double mean, max, min, vol;
    };

    // Helper methods
    static constexpr uint64_t get_window_duration_us(Window window);
    static TradeWindow map_to_trade_window(Window window);
    size_t find_window_start(uint64_t window_start_time) const;
    size_t find_qty_window_start(uint64_t window_start_time) const;
    Metrics calculate_metrics(Window window, uint64_t current_time) const;
    SpreadStats calculate_spread_stats(size_t start, size_t end) const;

    // Ring buffer accessors
    inline const SpreadSample& get_spread(size_t idx) const { return spread_history_[(spread_head_ + idx) & MASK]; }
    inline const QtyDecreaseSample& get_qty_decrease(size_t idx) const {
        return qty_decrease_history_[(qty_head_ + idx) & QTY_MASK];
    }
};

// ============================================================================
// Implementation
// ============================================================================

constexpr uint64_t CombinedMetrics::get_window_duration_us(Window window) {
    switch (window) {
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
    return 1'000'000;
}

inline void CombinedMetrics::update(uint64_t timestamp_us) {
    // Get current book metrics
    auto book_m = book_metrics_.get_metrics();

    constexpr uint64_t ONE_MINUTE_US = 60'000'000;

    // === Spread Tracking ===
    // Only store spread if book is valid (skip invalid samples, don't store 0)
    if (book_m.best_bid != INVALID_PRICE && book_m.best_ask != INVALID_PRICE) {
        // Remove old spread samples
        while (spread_count_ > 0 && (timestamp_us - spread_history_[spread_head_].timestamp_us > ONE_MINUTE_US)) {
            spread_head_ = (spread_head_ + 1) & MASK;
            spread_count_--;
        }

        // If full, advance head
        if (spread_count_ == MAX_SPREAD_SAMPLES) {
            spread_head_ = (spread_head_ + 1) & MASK;
            spread_count_--;
        }

        // Add new spread sample
        spread_history_[spread_tail_] = SpreadSample{book_m.spread, timestamp_us};
        spread_tail_ = (spread_tail_ + 1) & MASK;
        spread_count_++;
    }

    // === Quantity Decrease Tracking ===
    Quantity bid_qty_decrease = 0;
    Quantity ask_qty_decrease = 0;

    // Calculate qty decreases (same price, quantity decreased)
    if (prev_best_bid_ != INVALID_PRICE && prev_best_bid_ == book_m.best_bid) {
        if (prev_best_bid_qty_ > book_m.best_bid_qty) {
            bid_qty_decrease = prev_best_bid_qty_ - book_m.best_bid_qty;
        }
    }

    if (prev_best_ask_ != INVALID_PRICE && prev_best_ask_ == book_m.best_ask) {
        if (prev_best_ask_qty_ > book_m.best_ask_qty) {
            ask_qty_decrease = prev_best_ask_qty_ - book_m.best_ask_qty;
        }
    }

    // Store qty decrease sample only if non-zero (memory optimization)
    if (bid_qty_decrease > 0 || ask_qty_decrease > 0) {
        // Remove old qty decrease samples
        while (qty_count_ > 0 && (timestamp_us - qty_decrease_history_[qty_head_].timestamp_us > ONE_MINUTE_US)) {
            qty_head_ = (qty_head_ + 1) & QTY_MASK;
            qty_count_--;
        }

        // If full, advance head
        if (qty_count_ == MAX_QTY_SAMPLES) {
            qty_head_ = (qty_head_ + 1) & QTY_MASK;
            qty_count_--;
        }

        // Add new qty decrease sample
        qty_decrease_history_[qty_tail_] = QtyDecreaseSample{bid_qty_decrease, ask_qty_decrease, timestamp_us};
        qty_tail_ = (qty_tail_ + 1) & QTY_MASK;
        qty_count_++;
    }

    // Update previous state
    prev_best_bid_ = book_m.best_bid;
    prev_best_ask_ = book_m.best_ask;
    prev_best_bid_qty_ = book_m.best_bid_qty;
    prev_best_ask_qty_ = book_m.best_ask_qty;

    // Invalidate cache
    update_generation_++;
}


inline const CombinedMetrics::Metrics& CombinedMetrics::get_metrics(Window w) const {
    if (spread_count_ == 0) {
        return cached_metrics_[0]; // Return empty metrics
    }

    size_t window_idx = static_cast<size_t>(w);

    // Check cache validity using generation counter
    if (cache_generation_[window_idx] == update_generation_) {
        return cached_metrics_[window_idx];
    }

    // Recalculate
    uint64_t current_time = spread_history_[(spread_tail_ - 1) & MASK].timestamp_us;
    cached_metrics_[window_idx] = calculate_metrics(w, current_time);
    cache_generation_[window_idx] = update_generation_;

    return cached_metrics_[window_idx];
}

inline size_t CombinedMetrics::find_window_start(uint64_t window_start_time) const {
    size_t left = 0;
    size_t right = spread_count_;

    // Binary search for first sample with timestamp >= window_start_time
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        const SpreadSample& s = get_spread(mid);

        if (s.timestamp_us < window_start_time) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    return left;
}

inline size_t CombinedMetrics::find_qty_window_start(uint64_t window_start_time) const {
    size_t left = 0;
    size_t right = qty_count_;

    // Binary search for first sample with timestamp >= window_start_time
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        const QtyDecreaseSample& s = get_qty_decrease(mid);

        if (s.timestamp_us < window_start_time) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    return left;
}

// Map CombinedMetrics::Window to TradeWindow with compile-time validation
inline TradeWindow CombinedMetrics::map_to_trade_window(Window w) {
    // Static assert to ensure enum values match
    static_assert(static_cast<int>(Window::SEC_1) == static_cast<int>(TradeWindow::W1s), "Window enum mismatch: SEC_1");
    static_assert(static_cast<int>(Window::SEC_5) == static_cast<int>(TradeWindow::W5s), "Window enum mismatch: SEC_5");
    static_assert(static_cast<int>(Window::SEC_10) == static_cast<int>(TradeWindow::W10s),
                  "Window enum mismatch: SEC_10");
    static_assert(static_cast<int>(Window::SEC_30) == static_cast<int>(TradeWindow::W30s),
                  "Window enum mismatch: SEC_30");
    static_assert(static_cast<int>(Window::MIN_1) == static_cast<int>(TradeWindow::W1min),
                  "Window enum mismatch: MIN_1");

    return static_cast<TradeWindow>(w);
}

inline CombinedMetrics::Metrics CombinedMetrics::calculate_metrics(Window window, uint64_t current_time) const {
    Metrics m;

    // Get window duration
    uint64_t window_us = get_window_duration_us(window);
    uint64_t window_start = (current_time > window_us) ? (current_time - window_us) : 0;

    // === Trade to Depth Ratio ===
    auto trade_m = trade_metrics_.get_metrics(map_to_trade_window(window));
    auto book_m = book_metrics_.get_metrics();

    double total_depth = book_m.bid_depth_10 + book_m.ask_depth_10;
    // Branchless: avoid division by zero
    m.trade_to_depth_ratio = trade_m.total_volume / std::max(total_depth, 1.0);
    m.trade_to_depth_ratio *= (total_depth > 0.0); // Zero if no depth

    // === Absorption Ratios ===
    // Sum qty decreases over the window (must match trade volume window)
    size_t qty_start_idx = find_qty_window_start(window_start);

    double cumulative_bid_decrease = 0.0;
    double cumulative_ask_decrease = 0.0;

    for (size_t i = qty_start_idx; i < qty_count_; i++) {
        const auto& sample = get_qty_decrease(i);
        cumulative_bid_decrease += static_cast<double>(sample.bid_qty_decrease);
        cumulative_ask_decrease += static_cast<double>(sample.ask_qty_decrease);
    }

    double sell_volume = trade_m.sell_volume;
    double buy_volume = trade_m.buy_volume;

    // Calculate absorption ratios: trade_volume / qty_decrease
    // > 1 means strong absorption (more volume traded than qty lost)
    m.absorption_ratio_bid = sell_volume / std::max(cumulative_bid_decrease, 1.0);
    m.absorption_ratio_bid *= (cumulative_bid_decrease > 0.0); // Zero if no decrease

    m.absorption_ratio_ask = buy_volume / std::max(cumulative_ask_decrease, 1.0);
    m.absorption_ratio_ask *= (cumulative_ask_decrease > 0.0); // Zero if no decrease

    // === Spread Dynamics ===
    // Find samples within window
    size_t start_idx = find_window_start(window_start);
    if (start_idx >= spread_count_) {
        return m; // No samples in window
    }

    auto stats = calculate_spread_stats(start_idx, spread_count_);
    m.spread_mean = stats.mean;
    m.spread_max = stats.max;
    m.spread_min = stats.min;
    m.spread_volatility = stats.vol;

    return m;
}

inline CombinedMetrics::SpreadStats CombinedMetrics::calculate_spread_stats(size_t start, size_t end) const {
    if (start >= end) {
        return {0.0, 0.0, 0.0, 0.0};
    }

    const size_t n = end - start;

    // Single pass: calculate all stats using Welford's algorithm
    double M = 0.0; // Mean
    double S = 0.0; // Sum of squared differences
    double max_spread = -std::numeric_limits<double>::infinity();
    double min_spread = std::numeric_limits<double>::infinity();

    for (size_t i = start; i < end; i++) {
        double spread = get_spread(i).spread;

        // Update min/max
        max_spread = std::max(max_spread, spread);
        min_spread = std::min(min_spread, spread);

        // Welford's algorithm for mean and variance
        size_t k = i - start + 1;
        double delta = spread - M;
        M += delta / k;
        double delta2 = spread - M;
        S += delta * delta2;
    }

    double volatility = (n > 1) ? std::sqrt(S / (n - 1)) : 0.0;
    return {M, max_spread, min_spread, volatility};
}

inline void CombinedMetrics::reset() {
    spread_head_ = 0;
    spread_tail_ = 0;
    spread_count_ = 0;
    qty_head_ = 0;
    qty_tail_ = 0;
    qty_count_ = 0;
    prev_best_bid_ = INVALID_PRICE;
    prev_best_ask_ = INVALID_PRICE;
    prev_best_bid_qty_ = 0;
    prev_best_ask_qty_ = 0;
    update_generation_ = 0;
    cached_metrics_ = {};
    cache_generation_ = {};
}

} // namespace hft
