#pragma once

#include "../types.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <vector>

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

        // Price tracking
        double min_price = std::numeric_limits<double>::max();
        double max_price = std::numeric_limits<double>::lowest();

        // Streak tracking
        int current_buy_streak = 0;
        int current_sell_streak = 0;
        int max_buy_s = 0;
        int max_sell_s = 0;

        // Previous price for tick calculation
        Price prev_price = 0;
        int upticks = 0;
        int downticks = 0;
        int zeroticks = 0;
        bool first_trade = true;

        // Timing
        std::vector<uint64_t> inter_trade_times;
        uint64_t prev_time = 0;
        uint64_t min_inter_time = std::numeric_limits<uint64_t>::max();
        int burst_cnt = 0;
        constexpr uint64_t BURST_THRESHOLD_US = 10'000; // 10ms

        // Price changes for volatility
        std::vector<double> price_changes;

        for (auto it = begin; it != end; ++it) {
            const Trade& t = *it;
            double qty = static_cast<double>(t.quantity);

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
            vwap_sum += static_cast<double>(t.price) * qty;

            // Price high/low
            double price_d = static_cast<double>(t.price);
            min_price = std::min(min_price, price_d);
            max_price = std::max(max_price, price_d);

            // Ticks
            if (!first_trade) {
                if (t.price > prev_price) {
                    upticks++;
                } else if (t.price < prev_price) {
                    downticks++;
                } else {
                    zeroticks++;
                }

                // Price changes for volatility
                price_changes.push_back(static_cast<double>(t.price) - static_cast<double>(prev_price));

                // Inter-trade time
                uint64_t inter_time = t.timestamp_us - prev_time;
                inter_trade_times.push_back(inter_time);
                min_inter_time = std::min(min_inter_time, inter_time);

                if (inter_time <= BURST_THRESHOLD_US) {
                    burst_cnt++;
                }
            }

            prev_price = t.price;
            prev_time = t.timestamp_us;
            first_trade = false;
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
        m.high = (max_price != std::numeric_limits<double>::lowest()) ? max_price : 0.0;
        m.low = (min_price != std::numeric_limits<double>::max()) ? min_price : 0.0;

        // Price velocity (price change per second)
        if (inter_trade_times.size() > 0 && !price_changes.empty()) {
            uint64_t total_time_us = (end - 1)->timestamp_us - begin->timestamp_us;
            if (total_time_us > 0) {
                double total_price_change = static_cast<double>((end - 1)->price) - static_cast<double>(begin->price);
                double total_time_s = static_cast<double>(total_time_us) / 1'000'000.0;
                m.price_velocity = total_price_change / total_time_s;
            }
        }

        // Realized volatility (std dev of price changes)
        if (price_changes.size() > 1) {
            double mean = 0.0;
            for (double pc : price_changes) {
                mean += pc;
            }
            mean /= price_changes.size();

            double variance = 0.0;
            for (double pc : price_changes) {
                double diff = pc - mean;
                variance += diff * diff;
            }
            variance /= price_changes.size();
            m.realized_volatility = std::sqrt(variance);
        }

        // Streaks (current streaks at end of window)
        m.buy_streak = current_buy_streak;
        m.sell_streak = current_sell_streak;
        m.max_buy_streak = max_buy_s;
        m.max_sell_streak = max_sell_s;

        // Timing
        if (inter_trade_times.size() > 0) {
            double sum_inter_time = 0.0;
            for (uint64_t t : inter_trade_times) {
                sum_inter_time += static_cast<double>(t);
            }
            m.avg_inter_trade_time_us = sum_inter_time / inter_trade_times.size();
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
