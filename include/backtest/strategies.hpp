#pragma once

#include "kline_backtest.hpp"
#include <deque>
#include <numeric>
#include <cmath>

namespace hft {
namespace backtest {

/**
 * Simple Moving Average Crossover Strategy
 *
 * Buy when fast MA crosses above slow MA.
 * Sell when fast MA crosses below slow MA.
 */
class SMACrossover : public IStrategy {
public:
    SMACrossover(int fast_period = 10, int slow_period = 30)
        : fast_period_(fast_period)
        , slow_period_(slow_period)
        , prev_fast_(0)
        , prev_slow_(0)
    {}

    Signal on_kline(const exchange::Kline& kline, const BacktestPosition& position) override {
        double close = kline.close / 10000.0;
        closes_.push_back(close);

        // Wait for enough data
        if (closes_.size() < static_cast<size_t>(slow_period_)) {
            return Signal::None;
        }

        // Trim buffer
        while (closes_.size() > static_cast<size_t>(slow_period_)) {
            closes_.pop_front();
        }

        // Calculate MAs
        double fast_ma = calculate_sma(fast_period_);
        double slow_ma = calculate_sma(slow_period_);

        Signal signal = Signal::None;

        // Check crossover
        if (prev_fast_ > 0 && prev_slow_ > 0) {
            // Golden cross: fast crosses above slow
            if (prev_fast_ <= prev_slow_ && fast_ma > slow_ma) {
                signal = Signal::Buy;
            }
            // Death cross: fast crosses below slow
            else if (prev_fast_ >= prev_slow_ && fast_ma < slow_ma) {
                signal = position.is_long() ? Signal::Close : Signal::Sell;
            }
        }

        prev_fast_ = fast_ma;
        prev_slow_ = slow_ma;

        return signal;
    }

private:
    int fast_period_;
    int slow_period_;
    std::deque<double> closes_;
    double prev_fast_;
    double prev_slow_;

    double calculate_sma(int period) const {
        if (closes_.size() < static_cast<size_t>(period)) return 0;

        double sum = 0;
        auto it = closes_.end() - period;
        for (int i = 0; i < period; ++i, ++it) {
            sum += *it;
        }
        return sum / period;
    }
};

/**
 * RSI Strategy
 *
 * Buy when RSI < oversold (default 30).
 * Sell when RSI > overbought (default 70).
 */
class RSIStrategy : public IStrategy {
public:
    RSIStrategy(int period = 14, double oversold = 30, double overbought = 70)
        : period_(period)
        , oversold_(oversold)
        , overbought_(overbought)
        , prev_close_(0)
    {}

    Signal on_kline(const exchange::Kline& kline, const BacktestPosition& position) override {
        double close = kline.close / 10000.0;

        if (prev_close_ > 0) {
            double change = close - prev_close_;
            if (change > 0) {
                gains_.push_back(change);
                losses_.push_back(0);
            } else {
                gains_.push_back(0);
                losses_.push_back(-change);
            }
        }
        prev_close_ = close;

        // Wait for enough data
        if (gains_.size() < static_cast<size_t>(period_)) {
            return Signal::None;
        }

        // Trim buffers
        while (gains_.size() > static_cast<size_t>(period_)) {
            gains_.pop_front();
            losses_.pop_front();
        }

        double rsi = calculate_rsi();

        // Generate signals
        if (rsi < oversold_ && position.is_flat()) {
            return Signal::Buy;
        } else if (rsi > overbought_ && position.is_long()) {
            return Signal::Close;
        }

        return Signal::None;
    }

private:
    int period_;
    double oversold_;
    double overbought_;
    double prev_close_;
    std::deque<double> gains_;
    std::deque<double> losses_;

    double calculate_rsi() const {
        double avg_gain = 0, avg_loss = 0;

        for (double g : gains_) avg_gain += g;
        for (double l : losses_) avg_loss += l;

        avg_gain /= period_;
        avg_loss /= period_;

        if (avg_loss == 0) return 100;

        double rs = avg_gain / avg_loss;
        return 100 - (100 / (1 + rs));
    }
};

/**
 * Mean Reversion Strategy
 *
 * Buy when price is N standard deviations below the mean.
 * Sell when price returns to mean.
 */
class MeanReversion : public IStrategy {
public:
    MeanReversion(int lookback = 20, double std_multiplier = 2.0)
        : lookback_(lookback)
        , std_multiplier_(std_multiplier)
    {}

    Signal on_kline(const exchange::Kline& kline, const BacktestPosition& position) override {
        double close = kline.close / 10000.0;
        closes_.push_back(close);

        if (closes_.size() < static_cast<size_t>(lookback_)) {
            return Signal::None;
        }

        while (closes_.size() > static_cast<size_t>(lookback_)) {
            closes_.pop_front();
        }

        // Calculate mean and std
        double mean = 0;
        for (double c : closes_) mean += c;
        mean /= closes_.size();

        double variance = 0;
        for (double c : closes_) {
            variance += (c - mean) * (c - mean);
        }
        variance /= closes_.size();
        double std_dev = std::sqrt(variance);

        double lower_band = mean - std_multiplier_ * std_dev;
        double upper_band = mean + std_multiplier_ * std_dev;

        // Mean reversion signals
        if (position.is_flat() && close < lower_band) {
            return Signal::Buy;
        } else if (position.is_long() && close > mean) {
            return Signal::Close;  // Take profit at mean
        } else if (position.is_long() && close > upper_band) {
            return Signal::Close;  // Strong take profit
        }

        return Signal::None;
    }

private:
    int lookback_;
    double std_multiplier_;
    std::deque<double> closes_;
};

/**
 * Breakout Strategy
 *
 * Buy on new high, sell on new low.
 */
class BreakoutStrategy : public IStrategy {
public:
    explicit BreakoutStrategy(int lookback = 20)
        : lookback_(lookback)
    {}

    Signal on_kline(const exchange::Kline& kline, const BacktestPosition& position) override {
        double high = kline.high / 10000.0;
        double low = kline.low / 10000.0;
        double close = kline.close / 10000.0;

        highs_.push_back(high);
        lows_.push_back(low);

        if (highs_.size() < static_cast<size_t>(lookback_)) {
            return Signal::None;
        }

        while (highs_.size() > static_cast<size_t>(lookback_)) {
            highs_.pop_front();
            lows_.pop_front();
        }

        // Find highest high and lowest low (excluding current candle)
        double highest = 0;
        double lowest = 999999999;
        auto h_it = highs_.begin();
        auto l_it = lows_.begin();
        for (size_t i = 0; i < highs_.size() - 1; ++i, ++h_it, ++l_it) {
            if (*h_it > highest) highest = *h_it;
            if (*l_it < lowest) lowest = *l_it;
        }

        // Breakout signals
        if (position.is_flat() && close > highest) {
            return Signal::Buy;  // Bullish breakout
        } else if (position.is_long()) {
            // Exit on breakdown or trailing stop
            if (close < lowest || close < position.avg_price * 0.97) {
                return Signal::Close;
            }
        }

        return Signal::None;
    }

private:
    int lookback_;
    std::deque<double> highs_;
    std::deque<double> lows_;
};

/**
 * MACD Strategy
 *
 * Buy when MACD line crosses above signal line.
 * Sell when MACD line crosses below signal line.
 */
class MACDStrategy : public IStrategy {
public:
    MACDStrategy(int fast = 12, int slow = 26, int signal = 9)
        : fast_period_(fast)
        , slow_period_(slow)
        , signal_period_(signal)
        , fast_ema_(0)
        , slow_ema_(0)
        , signal_ema_(0)
        , prev_macd_(0)
        , prev_signal_(0)
        , initialized_(false)
    {}

    Signal on_kline(const exchange::Kline& kline, const BacktestPosition& position) override {
        double close = kline.close / 10000.0;

        if (!initialized_) {
            fast_ema_ = close;
            slow_ema_ = close;
            signal_ema_ = 0;
            initialized_ = true;
            return Signal::None;
        }

        // Update EMAs
        double fast_mult = 2.0 / (fast_period_ + 1);
        double slow_mult = 2.0 / (slow_period_ + 1);
        double signal_mult = 2.0 / (signal_period_ + 1);

        fast_ema_ = (close - fast_ema_) * fast_mult + fast_ema_;
        slow_ema_ = (close - slow_ema_) * slow_mult + slow_ema_;

        double macd = fast_ema_ - slow_ema_;
        signal_ema_ = (macd - signal_ema_) * signal_mult + signal_ema_;

        Signal sig = Signal::None;

        // Check for crossover
        if (prev_macd_ != 0) {
            // MACD crosses above signal
            if (prev_macd_ <= prev_signal_ && macd > signal_ema_) {
                sig = Signal::Buy;
            }
            // MACD crosses below signal
            else if (prev_macd_ >= prev_signal_ && macd < signal_ema_) {
                sig = position.is_long() ? Signal::Close : Signal::Sell;
            }
        }

        prev_macd_ = macd;
        prev_signal_ = signal_ema_;

        return sig;
    }

private:
    int fast_period_;
    int slow_period_;
    int signal_period_;
    double fast_ema_;
    double slow_ema_;
    double signal_ema_;
    double prev_macd_;
    double prev_signal_;
    bool initialized_;
};

}  // namespace backtest
}  // namespace hft
