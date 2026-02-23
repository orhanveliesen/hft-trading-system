#pragma once

#include "../types.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace hft {

enum class TradeWindow : uint8_t { W1s = 0, W5s = 1, W10s = 2, W30s = 3, W1min = 4 };

class TradeStreamMetrics {
public:
    struct Metrics {
        // Volume
        double buy_volume = 0.0;
        double sell_volume = 0.0;
        double total_volume = 0.0;

        // Delta
        double delta = 0.0;
        double cumulative_delta = 0.0; // Note: currently same as delta (per-window, not across calls)
        double buy_ratio = 0.0;

        // Trade count
        int total_trades = 0;
        int buy_trades = 0;
        int sell_trades = 0;
        int large_trades = 0;

        // Price
        double vwap = 0.0;
        double high = 0.0;
        double low = 0.0;
        double price_velocity = 0.0;
        double realized_volatility = 0.0;

        // Streaks
        int buy_streak = 0;
        int sell_streak = 0;
        int max_buy_streak = 0;
        int max_sell_streak = 0;

        // Timing
        double avg_inter_trade_time_us = 0.0;
        double min_inter_trade_time_us = 0.0;
        int burst_count = 0;

        // Ticks
        int uptick_count = 0;
        int downtick_count = 0;
        int zerotick_count = 0;
        double tick_ratio = 0.0;
    };

    explicit TradeStreamMetrics(Quantity large_trade_threshold = 500) : large_trade_threshold_(large_trade_threshold) {
        // Initialize cache as invalid
        cache_tail_position_.fill(SIZE_MAX);
    }

    void on_trade(Price price, Quantity quantity, bool is_buy, uint64_t timestamp_us) {
        // Remove trades older than 1 minute
        constexpr uint64_t ONE_MINUTE_US = 60'000'000;
        while (count_ > 0 && (timestamp_us - trades_[head_].timestamp_us > ONE_MINUTE_US)) {
            head_ = (head_ + 1) & MASK;
            count_--;
        }

        // Branchless: if (count_ == MAX_TRADES) { head_++; count_--; }
        size_t is_full = (count_ == MAX_TRADES);
        head_ = (head_ + is_full) & MASK;
        count_ -= is_full;

        trades_[tail_] = Trade{price, quantity, is_buy, timestamp_us};
        tail_ = (tail_ + 1) & MASK;
        count_++;
    }

    Metrics get_metrics(TradeWindow window) const {
        if (count_ == 0) {
            return Metrics{};
        }

        size_t window_idx = static_cast<size_t>(window);

        // Check cache validity: cache is valid if tail_ hasn't changed
        if (cache_tail_position_[window_idx] == tail_) {
            return cached_metrics_[window_idx];
        }

        // Cache miss: recalculate
        uint64_t window_us = get_window_duration_us(window);
        uint64_t current_time = trades_[(tail_ - 1) & MASK].timestamp_us;
        int64_t window_start = static_cast<int64_t>(current_time) - static_cast<int64_t>(window_us);

        // Binary search for window boundary (trades are time-ordered)
        size_t start_idx = find_window_start(window_start);

        if (start_idx == count_) {
            return Metrics{};
        }

        // Calculate and cache
        cached_metrics_[window_idx] = calculate_metrics(start_idx, count_);
        cache_tail_position_[window_idx] = tail_;

        return cached_metrics_[window_idx];
    }

    void reset() {
        head_ = 0;
        tail_ = 0;
        count_ = 0;
        // Invalidate cache
        cache_tail_position_.fill(SIZE_MAX);
    }

private:
    struct Trade {
        Price price;
        Quantity quantity;
        bool is_buy;
        uint64_t timestamp_us;
    };

    // Ring buffer (power of 2 for fast modulo via bitwise AND)
    static constexpr size_t MAX_TRADES = 1 << 16; // 2^16 = 65536 trades
    static constexpr size_t MASK = MAX_TRADES - 1;

    std::array<Trade, MAX_TRADES> trades_;
    size_t head_ = 0; // oldest trade
    size_t tail_ = 0; // next insert position
    size_t count_ = 0;

    Quantity large_trade_threshold_;

    // Cache for metrics (invalidated when tail_ changes)
    mutable std::array<Metrics, 5> cached_metrics_;
    mutable std::array<size_t, 5> cache_tail_position_; // tail_ when cache was built

    static constexpr uint64_t get_window_duration_us(TradeWindow window) {
        switch (window) {
        case TradeWindow::W1s:
            return 1'000'000;
        case TradeWindow::W5s:
            return 5'000'000;
        case TradeWindow::W10s:
            return 10'000'000;
        case TradeWindow::W30s:
            return 30'000'000;
        case TradeWindow::W1min:
            return 60'000'000;
        }
        return 1'000'000;
    }

    // Binary search for first trade within window (O(log n) instead of O(n))
    size_t find_window_start(int64_t window_start) const {
        size_t left = 0;
        size_t right = count_;

        while (left < right) {
            size_t mid = left + (right - left) / 2;
            size_t actual_idx = (head_ + mid) & MASK;

            if (static_cast<int64_t>(trades_[actual_idx].timestamp_us) <= window_start) {
                left = mid + 1;
            } else {
                right = mid;
            }
        }

        return left;
    }

    // Get trade at logical index (handles ring buffer wrap-around)
    const Trade& get_trade(size_t logical_idx) const { return trades_[(head_ + logical_idx) & MASK]; }

    // Accumulator struct for calculate_metrics (SRP: group related state)
    struct TradeAccumulators {
        // Volume and trade counts
        double buy_vol = 0.0;
        double sell_vol = 0.0;
        int buy_count = 0;
        int sell_count = 0;
        int large_count = 0;
        double vwap_sum = 0.0;
        double total_vol = 0.0;

        // Price tracking
        Price min_price = std::numeric_limits<Price>::max();
        Price max_price = std::numeric_limits<Price>::lowest();

        // Streak tracking
        int current_buy_streak = 0;
        int current_sell_streak = 0;
        int max_buy_s = 0;
        int max_sell_s = 0;

        // Tick tracking
        int upticks = 0;
        int downticks = 0;
        int zeroticks = 0;

        // Timing
        double sum_inter_time = 0.0;
        size_t inter_trade_count = 0;
        uint64_t min_inter_time = std::numeric_limits<uint64_t>::max();
        int burst_cnt = 0;
        static constexpr uint64_t BURST_THRESHOLD_US = 10'000; // 10ms

        // Welford's algorithm for variance
        double price_change_mean = 0.0;
        double price_change_m2 = 0.0;
        size_t price_change_count = 0;

        // State tracking
        Price prev_price = 0;
        uint64_t prev_time = 0;
        Price first_price = 0;
        double first_price_d = 0.0;
        uint64_t first_time = 0;
    };

    // Process first trade (branchless)
    void accumulate_first_trade(TradeAccumulators& acc, const Trade& t) const {
        double qty = static_cast<double>(t.quantity);
        acc.first_price = t.price;
        acc.first_price_d = static_cast<double>(t.price);
        acc.first_time = t.timestamp_us;

        // Branchless: if (is_buy) { buy_vol += qty; buy_count++; ... } else { sell_vol += qty; ... }
        int is_buy = t.is_buy;
        int is_sell = !t.is_buy;
        acc.buy_vol += is_buy * qty;
        acc.sell_vol += is_sell * qty;
        acc.buy_count += is_buy;
        acc.sell_count += is_sell;
        acc.current_buy_streak = is_buy;
        acc.current_sell_streak = is_sell;
        acc.max_buy_s = is_buy;
        acc.max_sell_s = is_sell;
        acc.total_vol += qty;

        // Branchless: if (quantity >= large_trade_threshold_) { large_count++; }
        acc.large_count += (t.quantity >= large_trade_threshold_);

        // VWAP
        acc.vwap_sum += acc.first_price_d * qty;

        // Price high/low
        acc.min_price = t.price;
        acc.max_price = t.price;

        // State for next trade
        acc.prev_price = t.price;
        acc.prev_time = t.timestamp_us;
    }

    // Process a single trade (branchless hot path)
    void accumulate_trade(TradeAccumulators& acc, const Trade& t) const {
        double qty = static_cast<double>(t.quantity);
        Price price = t.price;
        double price_d = static_cast<double>(price);

        // Branchless: if (is_buy) { buy_vol += qty; current_buy_streak++; ... }
        //             else { sell_vol += qty; current_sell_streak++; ... }
        int is_buy = t.is_buy;
        int is_sell = !t.is_buy;
        acc.buy_vol += is_buy * qty;
        acc.sell_vol += is_sell * qty;
        acc.buy_count += is_buy;
        acc.sell_count += is_sell;
        acc.current_buy_streak = is_buy * (acc.current_buy_streak + 1);
        acc.current_sell_streak = is_sell * (acc.current_sell_streak + 1);
        acc.max_buy_s = std::max(acc.max_buy_s, acc.current_buy_streak);
        acc.max_sell_s = std::max(acc.max_sell_s, acc.current_sell_streak);
        acc.total_vol += qty;

        // Branchless: if (quantity >= large_trade_threshold_) { large_count++; }
        acc.large_count += (t.quantity >= large_trade_threshold_);

        // VWAP
        acc.vwap_sum += price_d * qty;

        // Price high/low (already branchless with std::min/max)
        acc.min_price = std::min(acc.min_price, price);
        acc.max_price = std::max(acc.max_price, price);

        // Branchless: if (price > prev) upticks++; else if (price < prev) downticks++; else zeroticks++;
        int price_cmp = (price > acc.prev_price) - (price < acc.prev_price);
        acc.upticks += (price_cmp == 1);
        acc.downticks += (price_cmp == -1);
        acc.zeroticks += (price_cmp == 0);

        // Welford's online variance for price changes
        double price_change = price_d - static_cast<double>(acc.prev_price);
        acc.price_change_count++;
        double delta = price_change - acc.price_change_mean;
        acc.price_change_mean += delta / static_cast<double>(acc.price_change_count);
        double delta2 = price_change - acc.price_change_mean;
        acc.price_change_m2 += delta * delta2;

        // Inter-trade time
        uint64_t inter_time = t.timestamp_us - acc.prev_time;
        acc.sum_inter_time += static_cast<double>(inter_time);
        acc.inter_trade_count++;
        acc.min_inter_time = std::min(acc.min_inter_time, inter_time);

        // Branchless: if (inter_time <= BURST_THRESHOLD_US) { burst_cnt++; }
        acc.burst_cnt += (inter_time <= TradeAccumulators::BURST_THRESHOLD_US);

        // Update state
        acc.prev_price = price;
        acc.prev_time = t.timestamp_us;
    }

    // Build final metrics from accumulators
    Metrics build_metrics(const TradeAccumulators& acc) const {
        Metrics m;

        // Volume
        m.buy_volume = acc.buy_vol;
        m.sell_volume = acc.sell_vol;
        m.total_volume = acc.total_vol;
        m.delta = acc.buy_vol - acc.sell_vol;
        m.cumulative_delta = m.delta; // Note: per-window delta (not cumulative across calls)
        m.buy_ratio = (acc.total_vol > 0.0) ? (acc.buy_vol / acc.total_vol) : 0.0;

        // Trade counts
        m.total_trades = acc.buy_count + acc.sell_count;
        m.buy_trades = acc.buy_count;
        m.sell_trades = acc.sell_count;
        m.large_trades = acc.large_count;

        // Price
        m.vwap = (acc.total_vol > 0.0) ? (acc.vwap_sum / acc.total_vol) : 0.0;
        m.high = static_cast<double>(acc.max_price);
        m.low = static_cast<double>(acc.min_price);

        // Price velocity (price change per second)
        if (acc.inter_trade_count > 0) {
            uint64_t total_time_us = acc.prev_time - acc.first_time;
            if (total_time_us > 0) {
                double total_price_change = static_cast<double>(acc.prev_price) - acc.first_price_d;
                double total_time_s = static_cast<double>(total_time_us) / 1'000'000.0;
                m.price_velocity = total_price_change / total_time_s;
            }
        }

        // Realized volatility (Welford's algorithm)
        if (acc.price_change_count > 1) {
            m.realized_volatility = std::sqrt(acc.price_change_m2 / static_cast<double>(acc.price_change_count));
        }

        // Streaks
        m.buy_streak = acc.current_buy_streak;
        m.sell_streak = acc.current_sell_streak;
        m.max_buy_streak = acc.max_buy_s;
        m.max_sell_streak = acc.max_sell_s;

        // Timing
        if (acc.inter_trade_count > 0) {
            m.avg_inter_trade_time_us = acc.sum_inter_time / static_cast<double>(acc.inter_trade_count);
            m.min_inter_trade_time_us = static_cast<double>(acc.min_inter_time);
        }
        m.burst_count = acc.burst_cnt;

        // Ticks
        m.uptick_count = acc.upticks;
        m.downtick_count = acc.downticks;
        m.zerotick_count = acc.zeroticks;
        int total_ticks = acc.upticks + acc.downticks + acc.zeroticks;
        if (total_ticks > 0) {
            m.tick_ratio = static_cast<double>(acc.upticks - acc.downticks) / static_cast<double>(total_ticks);
        }

        return m;
    }

    // Calculate metrics for window (now clean and readable)
    Metrics calculate_metrics(size_t start_idx, size_t end_idx) const {
        if (start_idx >= end_idx) {
            return Metrics{};
        }

        TradeAccumulators acc;
        accumulate_first_trade(acc, get_trade(start_idx));

        for (size_t i = start_idx + 1; i < end_idx; ++i) {
            accumulate_trade(acc, get_trade(i));
        }

        return build_metrics(acc);
    }
};

} // namespace hft
