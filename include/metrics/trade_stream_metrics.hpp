#pragma once

#include "../simd/simd_ops.hpp"
#include "../types.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace hft {

/**
 * Trade window durations
 */
enum class TradeWindow {
    W1s,  // 1 second
    W5s,  // 5 seconds
    W10s, // 10 seconds
    W30s, // 30 seconds
    W1min // 1 minute
};

/**
 * TradeStreamMetrics - Real-time metrics from trade stream
 *
 * Tracks 30 metrics across 5 rolling time windows using SIMD-accelerated calculations.
 * Performance: < 1 μs per on_trade(), ~30 ns cached read, ~300 ns cache miss.
 *
 * Metrics:
 *   Volume (6): buy_volume, sell_volume, total_volume, delta, cumulative_delta, buy_ratio
 *   Counts (4): total_trades, buy_trades, sell_trades, large_trades
 *   Price (5): vwap, high, low, price_velocity, realized_volatility
 *   Streaks (4): buy_streak, sell_streak, max_buy_streak, max_sell_streak
 *   Timing (3): avg_inter_trade_time_us, min_inter_trade_time_us, burst_count
 *   Ticks (4): upticks, downticks, zeroticks, tick_ratio
 *
 * Windows: 1s, 5s, 10s, 30s, 1min
 */
class TradeStreamMetrics {
public:
    struct Metrics {
        // Volume metrics
        double buy_volume = 0.0;
        double sell_volume = 0.0;
        double total_volume = 0.0;
        double delta = 0.0;            // buy_volume - sell_volume
        double cumulative_delta = 0.0; // Running delta
        double buy_ratio = 0.0;        // buy_volume / total_volume

        // Trade count metrics
        int total_trades = 0;
        int buy_trades = 0;
        int sell_trades = 0;
        int large_trades = 0; // Trades > 2x average quantity

        // Price metrics
        double vwap = 0.0; // Volume-weighted average price
        Price high = 0;
        Price low = 0;
        double price_velocity = 0.0;      // Price change per second
        double realized_volatility = 0.0; // Std dev of price changes

        // Streak metrics (consecutive buy/sell trades)
        int buy_streak = 0;  // Current buy streak
        int sell_streak = 0; // Current sell streak
        int max_buy_streak = 0;
        int max_sell_streak = 0;

        // Timing metrics
        double avg_inter_trade_time_us = 0.0;
        uint64_t min_inter_trade_time_us = 0;
        int burst_count = 0; // Number of bursts (>10 trades in 100ms)

        // Tick metrics (price direction)
        int upticks = 0;         // Price increased
        int downticks = 0;       // Price decreased
        int zeroticks = 0;       // Price unchanged
        double tick_ratio = 0.0; // upticks / (upticks + downticks)
    };

    /**
     * Add a trade to the stream.
     *
     * @param price Trade price
     * @param quantity Trade quantity
     * @param is_buy True if buy order, false if sell
     * @param timestamp_us Trade timestamp in microseconds
     *
     * Performance: < 1 μs
     */
    void on_trade(Price price, Quantity quantity, bool is_buy, uint64_t timestamp_us);

    /**
     * Get metrics for a specific time window.
     *
     * Uses lazy caching:
     *   - Cache hit: ~30 ns
     *   - Cache miss: ~300 ns (SIMD calculation)
     *
     * @param window Time window to query
     * @return Metrics for the window
     */
    Metrics get_metrics(TradeWindow window) const;

    /**
     * Reset all metrics and trade history.
     */
    void reset();

private:
    struct Trade {
        Price price;
        Quantity quantity;
        bool is_buy;
        uint64_t timestamp_us;
    };

    // Ring buffer (power of 2 for fast modulo via bitwise AND)
    static constexpr size_t MAX_TRADES = 1 << 16; // 65536 trades (~65s at 1000 TPS)
    static constexpr size_t MASK = MAX_TRADES - 1;

    alignas(64) std::array<Trade, MAX_TRADES> trades_;
    size_t head_ = 0;  // Oldest trade index
    size_t tail_ = 0;  // Next insert position
    size_t count_ = 0; // Number of trades

    // Cache for metrics (invalidated when tail_ changes)
    mutable std::array<Metrics, 5> cached_metrics_;
    mutable std::array<size_t, 5> cache_tail_position_;

    // Helper methods
    static constexpr uint64_t get_window_duration_us(TradeWindow window);
    size_t find_window_start(uint64_t window_start_time) const;
    Metrics calculate_metrics(size_t start_idx, size_t end_idx) const;

    inline Trade& get_trade(size_t idx) { return trades_[(head_ + idx) & MASK]; }

    inline const Trade& get_trade(size_t idx) const { return trades_[(head_ + idx) & MASK]; }
};

// ============================================================================
// Implementation
// ============================================================================

constexpr uint64_t TradeStreamMetrics::get_window_duration_us(TradeWindow window) {
    switch (window) {
    case TradeWindow::W1s:
        return 1'000'000; // 1 second
    case TradeWindow::W5s:
        return 5'000'000; // 5 seconds
    case TradeWindow::W10s:
        return 10'000'000; // 10 seconds
    case TradeWindow::W30s:
        return 30'000'000; // 30 seconds
    case TradeWindow::W1min:
        return 60'000'000; // 1 minute
    }
    return 1'000'000; // Default 1s
}

void TradeStreamMetrics::on_trade(Price price, Quantity quantity, bool is_buy, uint64_t timestamp_us) {
    // Remove trades older than 1 minute (longest window)
    constexpr uint64_t ONE_MINUTE_US = 60'000'000;
    while (count_ > 0 && (timestamp_us - trades_[head_].timestamp_us > ONE_MINUTE_US)) {
        head_ = (head_ + 1) & MASK;
        count_--;
    }

    // Branchless: if (count_ == MAX_TRADES) { head_++; count_--; }
    size_t is_full = (count_ == MAX_TRADES);
    head_ = (head_ + is_full) & MASK;
    count_ -= is_full;

    // Add new trade
    trades_[tail_] = Trade{price, quantity, is_buy, timestamp_us};
    tail_ = (tail_ + 1) & MASK;
    count_++;
}

TradeStreamMetrics::Metrics TradeStreamMetrics::get_metrics(TradeWindow window) const {
    if (count_ == 0)
        return Metrics{};

    size_t window_idx = static_cast<size_t>(window);

    // Check cache validity: cache is valid if tail_ hasn't changed
    if (cache_tail_position_[window_idx] == tail_) {
        return cached_metrics_[window_idx]; // Cache hit: ~30 ns
    }

    // Cache miss: recalculate
    uint64_t window_us = get_window_duration_us(window);
    uint64_t current_time = trades_[(tail_ - 1) & MASK].timestamp_us;
    uint64_t window_start = current_time - window_us;

    // Binary search for window start
    size_t start_idx = find_window_start(window_start);
    if (start_idx == count_)
        return Metrics{};

    // Calculate and cache
    cached_metrics_[window_idx] = calculate_metrics(start_idx, count_);
    cache_tail_position_[window_idx] = tail_;

    return cached_metrics_[window_idx];
}

size_t TradeStreamMetrics::find_window_start(uint64_t window_start) const {
    size_t left = 0;
    size_t right = count_;

    // Binary search for first trade >= window_start
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        const Trade& t = get_trade(mid);

        if (t.timestamp_us <= window_start) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    return left;
}

TradeStreamMetrics::Metrics TradeStreamMetrics::calculate_metrics(size_t start_idx, size_t end_idx) const {
    if (start_idx >= end_idx)
        return Metrics{};

    Metrics m;

    // Prepare data for SIMD accumulation
    const size_t n = end_idx - start_idx;

    // First pass: SIMD-accelerated volume accumulation
    // We'll process in chunks and use SIMD for large batches
    double buy_vol = 0.0;
    double sell_vol = 0.0;
    double vwap_sum = 0.0;

    // For SIMD, we need contiguous arrays
    // Since trades are in ring buffer, we process in segments
    constexpr size_t SIMD_CHUNK = 256; // Process up to 256 trades via SIMD
    alignas(64) std::array<double, SIMD_CHUNK> prices_buf;
    alignas(64) std::array<double, SIMD_CHUNK> quantities_buf;
    alignas(64) std::array<int, SIMD_CHUNK> is_buy_buf;

    size_t processed = 0;
    while (processed < n) {
        size_t chunk_size = std::min(SIMD_CHUNK, n - processed);

        // Copy chunk to aligned buffers
        for (size_t j = 0; j < chunk_size; j++) {
            const Trade& t = get_trade(start_idx + processed + j);
            prices_buf[j] = static_cast<double>(t.price);
            quantities_buf[j] = static_cast<double>(t.quantity);
            is_buy_buf[j] = t.is_buy ? -1 : 0;
        }

        // SIMD accumulation
        double chunk_buy = 0.0, chunk_sell = 0.0, chunk_vwap = 0.0;
        simd::accumulate_volumes(prices_buf.data(), quantities_buf.data(), is_buy_buf.data(), chunk_size, chunk_buy,
                                 chunk_sell, chunk_vwap);

        buy_vol += chunk_buy;
        sell_vol += chunk_sell;
        vwap_sum += chunk_vwap;

        processed += chunk_size;
    }

    m.buy_volume = buy_vol;
    m.sell_volume = sell_vol;
    m.total_volume = buy_vol + sell_vol;
    m.delta = buy_vol - sell_vol;
    m.cumulative_delta = m.delta; // For single window query
    m.buy_ratio = (m.total_volume > 0.0) ? (buy_vol / m.total_volume) : 0.0;
    m.vwap = (m.total_volume > 0.0) ? (vwap_sum / m.total_volume) : 0.0;

    // Second pass: Scalar calculations for remaining metrics
    // (These are harder to vectorize due to dependencies)
    Price min_price = std::numeric_limits<Price>::max();
    Price max_price = std::numeric_limits<Price>::min();
    Price prev_price = 0;
    uint64_t prev_time = 0;
    uint64_t first_time = 0;
    uint64_t last_time = 0;
    uint64_t min_time_diff = std::numeric_limits<uint64_t>::max();
    uint64_t sum_time_diff = 0;
    int time_diff_count = 0;

    int current_buy_streak = 0;
    int current_sell_streak = 0;
    int burst_trades = 0;
    uint64_t burst_start_time = 0;

    // Welford's algorithm for volatility
    double price_change_mean = 0.0;
    double price_change_m2 = 0.0;
    int price_change_count = 0;

    bool first_trade = true;

    for (size_t i = start_idx; i < end_idx; i++) {
        const Trade& t = get_trade(i);
        Price price = t.price;
        uint64_t time = t.timestamp_us;

        // Count trades
        m.total_trades++;
        if (t.is_buy) {
            m.buy_trades++;
            current_buy_streak++;
            current_sell_streak = 0;
            m.max_buy_streak = std::max(m.max_buy_streak, current_buy_streak);
        } else {
            m.sell_trades++;
            current_sell_streak++;
            current_buy_streak = 0;
            m.max_sell_streak = std::max(m.max_sell_streak, current_sell_streak);
        }

        // Price metrics
        min_price = std::min(min_price, price);
        max_price = std::max(max_price, price);

        if (first_trade) {
            first_time = time;
            prev_price = price;
            prev_time = time;
            first_trade = false;
        } else {
            // Ticks
            if (price > prev_price) {
                m.upticks++;
            } else if (price < prev_price) {
                m.downticks++;
            } else {
                m.zeroticks++;
            }

            // Price velocity (using Welford's for incremental std dev)
            double price_change = static_cast<double>(static_cast<int64_t>(price) - static_cast<int64_t>(prev_price));
            price_change_count++;
            double delta_mean = price_change - price_change_mean;
            price_change_mean += delta_mean / price_change_count;
            double delta2 = price_change - price_change_mean;
            price_change_m2 += delta_mean * delta2;

            // Inter-trade time
            uint64_t time_diff = time - prev_time;
            sum_time_diff += time_diff;
            time_diff_count++;
            min_time_diff = std::min(min_time_diff, time_diff);

            // Burst detection (>= 10 trades in 100ms window)
            if (time - burst_start_time > 100'000) { // 100ms
                if (burst_trades >= 10) {
                    m.burst_count++;
                }
                burst_start_time = time;
                burst_trades = 1;
            } else {
                burst_trades++;
            }

            prev_price = price;
            prev_time = time;
        }

        last_time = time;
    }

    // Finalize metrics
    m.high = max_price;
    m.low = min_price;
    m.buy_streak = current_buy_streak;
    m.sell_streak = current_sell_streak;

    // Price velocity (price change per second)
    if (last_time > first_time) {
        double time_span_s = static_cast<double>(last_time - first_time) / 1e6;
        double total_price_change =
            static_cast<double>(static_cast<int64_t>(prev_price) - static_cast<int64_t>(get_trade(start_idx).price));
        m.price_velocity = total_price_change / time_span_s;
    }

    // Realized volatility
    if (price_change_count > 1) {
        m.realized_volatility = std::sqrt(price_change_m2 / (price_change_count - 1));
    }

    // Timing metrics
    if (time_diff_count > 0) {
        m.avg_inter_trade_time_us = static_cast<double>(sum_time_diff) / time_diff_count;
        m.min_inter_trade_time_us = min_time_diff;
    }

    // Tick ratio
    int total_ticks = m.upticks + m.downticks;
    m.tick_ratio = (total_ticks > 0) ? (static_cast<double>(m.upticks) / total_ticks) : 0.0;

    // Large trades (>= 2x average quantity)
    double avg_qty = m.total_volume / m.total_trades;
    for (size_t i = start_idx; i < end_idx; i++) {
        const Trade& t = get_trade(i);
        if (static_cast<double>(t.quantity) >= 2.0 * avg_qty) {
            m.large_trades++;
        }
    }

    return m;
}

void TradeStreamMetrics::reset() {
    head_ = 0;
    tail_ = 0;
    count_ = 0;
    cached_metrics_ = {};
    cache_tail_position_ = {};
}

} // namespace hft
