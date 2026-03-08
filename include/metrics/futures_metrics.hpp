#pragma once

#include "../types.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace hft {

/**
 * Futures metric time windows
 */
enum class FuturesWindow {
    W1s,  // 1 second
    W5s,  // 5 seconds
    W10s, // 10 seconds
    W30s, // 30 seconds
    W1min // 1 minute
};

/**
 * FuturesMetrics - Real-time metrics from futures market data
 *
 * Tracks futures-specific metrics across 5 rolling time windows:
 * - Funding rate and EMA (3 metrics)
 * - Basis (futures-spot spread) and EMA (3 metrics)
 * - Liquidation volume and count (2 metrics)
 * - Liquidation pressure by direction (3 metrics)
 *
 * Total: 11 metrics × 5 windows = 55 metrics
 *
 * Design:
 * - Header-only (inline implementations)
 * - Ring buffer for liquidation events (same pattern as TradeStreamMetrics)
 * - EMA smoothing for funding rate and basis
 * - Not thread-safe (caller responsibility)
 */
class FuturesMetrics {
public:
    struct Metrics {
        // Funding Rate (3)
        double funding_rate = 0.0;         // Latest funding rate
        double funding_rate_ema = 0.0;     // Exponential moving average
        bool funding_rate_extreme = false; // |rate| > 0.001 (0.1%)

        // Basis (Futures-Spot Spread) (3)
        double basis = 0.0;     // Futures mid - spot mid (absolute)
        double basis_bps = 0.0; // Basis in basis points
        double basis_ema = 0.0; // EMA of basis

        // Liquidation Volume (2)
        double liquidation_volume = 0.0; // Total notional liquidated
        uint32_t liquidation_count = 0;  // Number of liquidation events

        // Liquidation Pressure (3)
        double long_liquidation_volume = 0.0;  // Sell-side liquidations (longs getting rekt)
        double short_liquidation_volume = 0.0; // Buy-side liquidations (shorts getting rekt)
        double liquidation_imbalance = 0.0;    // (long - short) / (long + short), range [-1, 1]

        // Funding Schedule (1) - Phase 5.0
        uint64_t next_funding_time_ms = 0; // Next funding time in milliseconds since epoch
    };

    /**
     * Update from mark price stream.
     *
     * @param mark_price Futures mark price
     * @param index_price Index price (spot reference)
     * @param funding_rate Funding rate
     * @param next_funding_time_ms Next funding time in milliseconds (Phase 5.0)
     * @param timestamp_us Timestamp in microseconds
     */
    void on_mark_price(double mark_price, double index_price, double funding_rate, uint64_t next_funding_time_ms,
                       uint64_t timestamp_us);

    /**
     * Update from liquidation stream.
     *
     * @param side Liquidation side (Sell = long liquidated, Buy = short liquidated)
     * @param price Liquidation price
     * @param quantity Liquidation quantity
     * @param timestamp_us Timestamp in microseconds
     */
    void on_liquidation(Side side, double price, double quantity, uint64_t timestamp_us);

    /**
     * Update futures best bid/offer.
     *
     * @param bid Best bid price
     * @param ask Best ask price
     * @param timestamp_us Timestamp in microseconds
     */
    void on_futures_bbo(double bid, double ask, uint64_t timestamp_us);

    /**
     * Update spot best bid/offer.
     *
     * @param bid Best bid price
     * @param ask Best ask price
     * @param timestamp_us Timestamp in microseconds
     */
    void on_spot_bbo(double bid, double ask, uint64_t timestamp_us);

    /**
     * Update EMA calculations.
     *
     * Call this periodically (e.g., every 100ms) to update EMAs.
     *
     * @param now_us Current timestamp in microseconds
     */
    void update(uint64_t now_us);

    /**
     * Get metrics for a specific time window.
     *
     * @param window Time window to query
     * @return Metrics for the window
     */
    Metrics get_metrics(FuturesWindow window) const;

    /**
     * Reset all metrics and event history.
     */
    void reset();

private:
    struct LiquidationEvent {
        Side side;
        double notional; // price * qty
        uint64_t timestamp_us;
    };

    // Ring buffer for liquidations (same as TradeStreamMetrics pattern)
    // Must be power of 2 for bitmask trick to work (index & MASK)
    static constexpr size_t MAX_EVENTS = 1 << 13; // 8192 events
    static constexpr size_t MASK = MAX_EVENTS - 1;

    alignas(64) std::array<LiquidationEvent, MAX_EVENTS> events_;
    size_t event_head_ = 0;
    size_t event_tail_ = 0;
    size_t event_count_ = 0;

    // Latest values
    double funding_rate_ = 0.0;
    uint64_t next_funding_time_ms_ = 0; // Phase 5.0
    double futures_bid_ = 0.0;
    double futures_ask_ = 0.0;
    double spot_bid_ = 0.0;
    double spot_ask_ = 0.0;
    uint64_t last_update_us_ = 0;

    // EMA states per window
    struct WindowState {
        double funding_ema = 0.0;
        double basis_ema = 0.0;
        bool initialized = false;
    };
    std::array<WindowState, 5> window_states_;

    // EMA alphas (same as TradeStreamMetrics pattern)
    static constexpr double ALPHA_1S = 1.0;     // 1s window
    static constexpr double ALPHA_5S = 0.333;   // 5s window
    static constexpr double ALPHA_10S = 0.182;  // 10s window
    static constexpr double ALPHA_30S = 0.065;  // 30s window
    static constexpr double ALPHA_1MIN = 0.033; // 1min window

    // Extreme threshold for funding rate (0.1%)
    static constexpr double FUNDING_EXTREME_THRESHOLD = 0.001;

    // Helper methods
    static constexpr uint64_t get_window_duration_us(FuturesWindow window);
    static constexpr double get_window_alpha(FuturesWindow window);
    Metrics calculate_metrics(FuturesWindow window, uint64_t now_us) const;
};

// ============================================================================
// Implementation
// ============================================================================

constexpr uint64_t FuturesMetrics::get_window_duration_us(FuturesWindow window) {
    switch (window) {
    case FuturesWindow::W1s:
        return 1'000'000; // 1 second
    case FuturesWindow::W5s:
        return 5'000'000; // 5 seconds
    case FuturesWindow::W10s:
        return 10'000'000; // 10 seconds
    case FuturesWindow::W30s:
        return 30'000'000; // 30 seconds
    case FuturesWindow::W1min:
        return 60'000'000; // 1 minute
    }
    return 1'000'000; // Default 1s
}

constexpr double FuturesMetrics::get_window_alpha(FuturesWindow window) {
    switch (window) {
    case FuturesWindow::W1s:
        return ALPHA_1S;
    case FuturesWindow::W5s:
        return ALPHA_5S;
    case FuturesWindow::W10s:
        return ALPHA_10S;
    case FuturesWindow::W30s:
        return ALPHA_30S;
    case FuturesWindow::W1min:
        return ALPHA_1MIN;
    }
    return ALPHA_1S; // Default
}

inline void FuturesMetrics::on_mark_price(double mark_price, double index_price, double funding_rate,
                                          uint64_t next_funding_time_ms, uint64_t timestamp_us) {
    (void)mark_price;  // Unused - basis calculated from BBO
    (void)index_price; // Unused - may be used in future for basis spread analysis
    funding_rate_ = funding_rate;
    next_funding_time_ms_ = next_funding_time_ms; // Phase 5.0
    last_update_us_ = timestamp_us;
}

inline void FuturesMetrics::on_liquidation(Side side, double price, double quantity, uint64_t timestamp_us) {
    // Remove events older than 1 minute (longest window)
    constexpr uint64_t ONE_MINUTE_US = 60'000'000;
    while (event_count_ > 0 && (timestamp_us - events_[event_head_].timestamp_us > ONE_MINUTE_US)) {
        event_head_ = (event_head_ + 1) & MASK;
        event_count_--;
    }

    // Branchless: if (event_count_ == MAX_EVENTS) { head_++; count_--; }
    size_t is_full = (event_count_ == MAX_EVENTS);
    event_head_ = (event_head_ + is_full) & MASK;
    event_count_ -= is_full;

    // Add new event
    events_[event_tail_] = LiquidationEvent{side, price * quantity, timestamp_us};
    event_tail_ = (event_tail_ + 1) & MASK;
    event_count_++;

    last_update_us_ = timestamp_us;
}

inline void FuturesMetrics::on_futures_bbo(double bid, double ask, uint64_t timestamp_us) {
    futures_bid_ = bid;
    futures_ask_ = ask;
    last_update_us_ = timestamp_us;
}

inline void FuturesMetrics::on_spot_bbo(double bid, double ask, uint64_t timestamp_us) {
    spot_bid_ = bid;
    spot_ask_ = ask;
    last_update_us_ = timestamp_us;
}

inline void FuturesMetrics::update(uint64_t now_us) {
    // Calculate current basis
    double basis = 0.0;
    if (futures_bid_ > 0.0 && futures_ask_ > 0.0 && spot_bid_ > 0.0 && spot_ask_ > 0.0) {
        double futures_mid = (futures_bid_ + futures_ask_) / 2.0;
        double spot_mid = (spot_bid_ + spot_ask_) / 2.0;
        basis = futures_mid - spot_mid;
    }

    // Update EMA for each window
    for (size_t i = 0; i < 5; i++) {
        WindowState& ws = window_states_[i];
        double alpha = get_window_alpha(static_cast<FuturesWindow>(i));

        if (!ws.initialized) {
            // First value seeds EMA
            ws.funding_ema = funding_rate_;
            ws.basis_ema = basis;
            ws.initialized = true;
        } else {
            // EMA update: ema = alpha * new_value + (1 - alpha) * old_ema
            ws.funding_ema = alpha * funding_rate_ + (1.0 - alpha) * ws.funding_ema;
            ws.basis_ema = alpha * basis + (1.0 - alpha) * ws.basis_ema;
        }
    }

    last_update_us_ = now_us;
}

inline FuturesMetrics::Metrics FuturesMetrics::get_metrics(FuturesWindow window) const {
    return calculate_metrics(window, last_update_us_);
}

inline FuturesMetrics::Metrics FuturesMetrics::calculate_metrics(FuturesWindow window, uint64_t now_us) const {
    Metrics m;

    // Funding rate metrics
    m.funding_rate = funding_rate_;
    m.funding_rate_extreme = (std::abs(funding_rate_) > FUNDING_EXTREME_THRESHOLD);
    m.next_funding_time_ms = next_funding_time_ms_; // Phase 5.0

    size_t window_idx = static_cast<size_t>(window);
    const WindowState& ws = window_states_[window_idx];
    m.funding_rate_ema = ws.funding_ema;

    // Basis metrics
    if (futures_bid_ > 0.0 && futures_ask_ > 0.0 && spot_bid_ > 0.0 && spot_ask_ > 0.0) {
        double futures_mid = (futures_bid_ + futures_ask_) / 2.0;
        double spot_mid = (spot_bid_ + spot_ask_) / 2.0;
        m.basis = futures_mid - spot_mid;
        m.basis_bps = (m.basis / spot_mid) * 10000.0; // Convert to basis points
    }
    m.basis_ema = ws.basis_ema;

    // Liquidation metrics for the window
    if (event_count_ == 0) {
        return m; // All liquidation metrics are zero
    }

    uint64_t window_us = get_window_duration_us(window);
    uint64_t window_start = (now_us > window_us) ? (now_us - window_us) : 0;

    // Find first event within window (linear scan from tail backwards)
    // Events are ordered by timestamp (oldest at head, newest at tail)
    double total_volume = 0.0;
    double long_volume = 0.0;  // SELL liquidations (longs getting liquidated)
    double short_volume = 0.0; // BUY liquidations (shorts getting liquidated)
    uint32_t count = 0;

    for (size_t i = 0; i < event_count_; i++) {
        const LiquidationEvent& event = events_[(event_head_ + i) & MASK];

        // Skip events outside window
        if (event.timestamp_us < window_start) {
            continue;
        }

        total_volume += event.notional;
        count++;

        if (event.side == Side::Sell) {
            long_volume += event.notional;
        } else {
            short_volume += event.notional;
        }
    }

    m.liquidation_volume = total_volume;
    m.liquidation_count = count;
    m.long_liquidation_volume = long_volume;
    m.short_liquidation_volume = short_volume;

    // Imbalance: (long - short) / (long + short)
    // Range: [-1, 1] where 1 = all longs, -1 = all shorts, 0 = balanced
    double total_directional = long_volume + short_volume;
    if (total_directional > 0.0) {
        m.liquidation_imbalance = (long_volume - short_volume) / total_directional;
    }

    return m;
}

inline void FuturesMetrics::reset() {
    event_head_ = 0;
    event_tail_ = 0;
    event_count_ = 0;
    funding_rate_ = 0.0;
    futures_bid_ = 0.0;
    futures_ask_ = 0.0;
    spot_bid_ = 0.0;
    spot_ask_ = 0.0;
    last_update_us_ = 0;
    window_states_ = {};
}

} // namespace hft
