#pragma once

#include "../types.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
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
        double cumulative_delta = 0.0;
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

    explicit TradeStreamMetrics(Quantity large_trade_threshold = 500) : large_trade_threshold_(large_trade_threshold) {}

    void on_trade(Price price, Quantity quantity, bool is_buy, uint64_t timestamp_us) {
        Trade trade{price, quantity, is_buy, timestamp_us};
        trades_.push_back(trade);

        // Keep only trades within 1 minute window (max window size)
        constexpr uint64_t ONE_MINUTE_US = 60'000'000;
        while (!trades_.empty() && (timestamp_us - trades_.front().timestamp_us > ONE_MINUTE_US)) {
            trades_.pop_front();
        }
    }

    Metrics get_metrics(TradeWindow window) const {
        uint64_t window_us = get_window_duration_us(window);
        if (trades_.empty()) {
            return Metrics{};
        }

        int64_t current_time = static_cast<int64_t>(trades_.back().timestamp_us);
        int64_t window_start = current_time - static_cast<int64_t>(window_us);

        // Find trades within window (strictly within: age < window duration)
        auto window_begin = std::find_if(trades_.begin(), trades_.end(), [window_start](const Trade& t) {
            return static_cast<int64_t>(t.timestamp_us) > window_start;
        });

        if (window_begin == trades_.end()) {
            return Metrics{};
        }

        return calculate_metrics(window_begin, trades_.end());
    }

    void reset() { trades_.clear(); }

private:
    struct Trade {
        Price price;
        Quantity quantity;
        bool is_buy;
        uint64_t timestamp_us;
    };

    Quantity large_trade_threshold_;
    std::deque<Trade> trades_;

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

    Metrics calculate_metrics(std::deque<Trade>::const_iterator begin, std::deque<Trade>::const_iterator end) const {
        Metrics m;

        if (begin == end) {
            return m;
        }

        // Volume and trade counts
        double buy_vol = 0.0;
        double sell_vol = 0.0;
        int buy_count = 0;
        int sell_count = 0;
        int large_count = 0;
        double vwap_sum = 0.0;
        double total_vol = 0.0;

        // Price tracking (use integer types, convert at end)
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

        // Timing (running accumulators - NO vector)
        double sum_inter_time = 0.0;
        size_t inter_trade_count = 0;
        uint64_t min_inter_time = std::numeric_limits<uint64_t>::max();
        int burst_cnt = 0;
        constexpr uint64_t BURST_THRESHOLD_US = 10'000; // 10ms

        // Welford's online algorithm for variance (NO vector)
        double price_change_mean = 0.0;
        double price_change_m2 = 0.0; // sum of squared deviations
        size_t price_change_count = 0;

        // Handle first trade (removes branch from loop)
        auto it = begin;
        const Trade& first = *it;
        double first_qty = static_cast<double>(first.quantity);
        Price first_price = first.price;
        uint64_t first_time = first.timestamp_us;

        // Process first trade
        if (first.is_buy) {
            buy_vol += first_qty;
            buy_count++;
            current_buy_streak = 1;
            max_buy_s = 1;
        } else {
            sell_vol += first_qty;
            sell_count++;
            current_sell_streak = 1;
            max_sell_s = 1;
        }
        total_vol += first_qty;
        if (first.quantity >= large_trade_threshold_) {
            large_count++;
        }
        vwap_sum += static_cast<double>(first_price) * first_qty;
        min_price = first_price;
        max_price = first_price;

        Price prev_price = first_price;
        uint64_t prev_time = first_time;

        // Process remaining trades (no branch in loop)
        ++it;
        for (; it != end; ++it) {
            const Trade& t = *it;
            double qty = static_cast<double>(t.quantity);
            Price price = t.price;

            // Volume
            if (t.is_buy) {
                buy_vol += qty;
                buy_count++;
                current_buy_streak++;
                current_sell_streak = 0;
                max_buy_s = std::max(max_buy_s, current_buy_streak);
            } else {
                sell_vol += qty;
                sell_count++;
                current_sell_streak++;
                current_buy_streak = 0;
                max_sell_s = std::max(max_sell_s, current_sell_streak);
            }

            total_vol += qty;

            // Large trades
            if (t.quantity >= large_trade_threshold_) {
                large_count++;
            }

            // VWAP
            vwap_sum += static_cast<double>(price) * qty;

            // Price high/low (integer comparison)
            min_price = std::min(min_price, price);
            max_price = std::max(max_price, price);

            // Ticks (integer comparison)
            if (price > prev_price) {
                upticks++;
            } else if (price < prev_price) {
                downticks++;
            } else {
                zeroticks++;
            }

            // Welford's online variance for price changes
            double price_change = static_cast<double>(price) - static_cast<double>(prev_price);
            price_change_count++;
            double delta = price_change - price_change_mean;
            price_change_mean += delta / static_cast<double>(price_change_count);
            double delta2 = price_change - price_change_mean;
            price_change_m2 += delta * delta2;

            // Inter-trade time (running accumulator)
            uint64_t inter_time = t.timestamp_us - prev_time;
            sum_inter_time += static_cast<double>(inter_time);
            inter_trade_count++;
            min_inter_time = std::min(min_inter_time, inter_time);

            if (inter_time <= BURST_THRESHOLD_US) {
                burst_cnt++;
            }

            prev_price = price;
            prev_time = t.timestamp_us;
        }

        // Fill metrics
        m.buy_volume = buy_vol;
        m.sell_volume = sell_vol;
        m.total_volume = total_vol;
        m.delta = buy_vol - sell_vol;
        m.cumulative_delta = m.delta;
        m.buy_ratio = (total_vol > 0.0) ? (buy_vol / total_vol) : 0.0;

        m.total_trades = buy_count + sell_count;
        m.buy_trades = buy_count;
        m.sell_trades = sell_count;
        m.large_trades = large_count;

        m.vwap = (total_vol > 0.0) ? (vwap_sum / total_vol) : 0.0;
        m.high = static_cast<double>(max_price);
        m.low = static_cast<double>(min_price);

        // Price velocity (price change per second)
        if (inter_trade_count > 0) {
            uint64_t total_time_us = prev_time - first_time;
            if (total_time_us > 0) {
                double total_price_change = static_cast<double>(prev_price) - static_cast<double>(first_price);
                double total_time_s = static_cast<double>(total_time_us) / 1'000'000.0;
                m.price_velocity = total_price_change / total_time_s;
            }
        }

        // Realized volatility (Welford's algorithm)
        if (price_change_count > 1) {
            m.realized_volatility = std::sqrt(price_change_m2 / static_cast<double>(price_change_count));
        }

        // Streaks (current streaks at end of window)
        m.buy_streak = current_buy_streak;
        m.sell_streak = current_sell_streak;
        m.max_buy_streak = max_buy_s;
        m.max_sell_streak = max_sell_s;

        // Timing
        if (inter_trade_count > 0) {
            m.avg_inter_trade_time_us = sum_inter_time / static_cast<double>(inter_trade_count);
            m.min_inter_trade_time_us = static_cast<double>(min_inter_time);
        }
        m.burst_count = burst_cnt;

        // Ticks
        m.uptick_count = upticks;
        m.downtick_count = downticks;
        m.zerotick_count = zeroticks;
        int total_ticks = upticks + downticks + zeroticks;
        if (total_ticks > 0) {
            m.tick_ratio = static_cast<double>(upticks - downticks) / static_cast<double>(total_ticks);
        }

        return m;
    }
};

} // namespace hft
