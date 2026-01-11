#pragma once

/**
 * OverfittedStrategy - Intentionally overfit to demonstrate failure
 *
 * This strategy has been "optimized" with magic numbers that worked
 * perfectly on historical BTC data from a specific period.
 *
 * Classic overfitting signs:
 * 1. Too many parameters (7+ tuned values)
 * 2. Arbitrary thresholds (why 2.17? why 13?)
 * 3. Time-specific patterns (hour-of-day trading)
 * 4. Perfect backtest, terrible forward test
 */

#include <array>
#include <cmath>
#include <cstdint>

namespace hft {
namespace strategy {

class OverfittedStrategy {
public:
    // "Optimized" parameters from backtesting on BTC Jan-Mar 2024
    // These magic numbers were curve-fitted to maximize backtest PnL
    static constexpr double MAGIC_BB_PERIOD = 13.7;      // Why 13.7? Because it fit the data
    static constexpr double MAGIC_BB_STD = 2.17;         // Why 2.17? Curve fitting
    static constexpr double MAGIC_RSI_OVERSOLD = 23.4;   // Why 23.4? Optimized
    static constexpr double MAGIC_RSI_OVERBOUGHT = 78.2; // Why 78.2? Optimized
    static constexpr double MAGIC_VOL_THRESHOLD = 0.0342; // Specific to that period
    static constexpr double MAGIC_MOMENTUM_WINDOW = 17;   // Another magic number
    static constexpr double MAGIC_ENTRY_MULT = 1.847;     // Suspiciously precise

    // Time-of-day "patterns" that worked in backtest
    // (spurious correlations from limited data)
    static constexpr std::array<bool, 24> GOOD_HOURS = {
        false, false, true,  true,   // 00-03: "BTC dumps at night"
        true,  false, false, false,  // 04-07: "Asian session weak"
        true,  true,  true,  false,  // 08-11: "European open good"
        false, true,  true,  true,   // 12-15: "US pre-market"
        true,  false, false, false,  // 16-19: "US close bad"
        false, false, false, false   // 20-23: "Night = no trade"
    };

    struct Signal {
        bool should_buy = false;
        bool should_sell = false;
        double confidence = 0.0;
        const char* reason = "";
    };

    OverfittedStrategy() = default;

    void update(double price, int hour_utc) {
        // Update price buffer
        prices_[price_idx_] = price;
        price_idx_ = (price_idx_ + 1) % BUFFER_SIZE;
        if (sample_count_ < BUFFER_SIZE) sample_count_++;

        current_hour_ = hour_utc;
        last_price_ = price;
    }

    Signal generate_signal() const {
        Signal sig;

        if (sample_count_ < BUFFER_SIZE) {
            sig.reason = "warming up";
            return sig;
        }

        // Rule 1: Time-of-day filter (OVERFIT!)
        // This pattern existed in 3 months of data, probably noise
        if (!GOOD_HOURS[current_hour_ % 24]) {
            sig.reason = "bad hour (overfit pattern)";
            return sig;
        }

        // Rule 2: Magic Bollinger Bands
        double sma = calculate_sma(static_cast<int>(MAGIC_BB_PERIOD));
        double std = calculate_std(static_cast<int>(MAGIC_BB_PERIOD), sma);
        double upper_bb = sma + MAGIC_BB_STD * std;
        double lower_bb = sma - MAGIC_BB_STD * std;

        // Rule 3: Magic RSI
        double rsi = calculate_rsi(14);

        // Rule 4: Magic volatility filter
        double volatility = std / sma;
        if (volatility < MAGIC_VOL_THRESHOLD) {
            sig.reason = "volatility too low (overfit threshold)";
            return sig;
        }

        // Rule 5: Magic momentum
        double momentum = calculate_momentum(static_cast<int>(MAGIC_MOMENTUM_WINDOW));

        // Complex entry logic with multiple magic numbers
        // This worked PERFECTLY in backtest...
        if (last_price_ < lower_bb &&
            rsi < MAGIC_RSI_OVERSOLD &&
            momentum > -MAGIC_ENTRY_MULT * volatility) {
            sig.should_buy = true;
            sig.confidence = (MAGIC_RSI_OVERSOLD - rsi) / MAGIC_RSI_OVERSOLD;
            sig.reason = "oversold + momentum (overfit)";
        }
        else if (last_price_ > upper_bb &&
                 rsi > MAGIC_RSI_OVERBOUGHT &&
                 momentum < MAGIC_ENTRY_MULT * volatility) {
            sig.should_sell = true;
            sig.confidence = (rsi - MAGIC_RSI_OVERBOUGHT) / (100 - MAGIC_RSI_OVERBOUGHT);
            sig.reason = "overbought + momentum (overfit)";
        }
        else {
            sig.reason = "no signal";
        }

        return sig;
    }

    // Statistics for analysis
    struct Stats {
        int total_signals = 0;
        int filtered_by_hour = 0;
        int filtered_by_volatility = 0;
        int buy_signals = 0;
        int sell_signals = 0;
    };

    Stats stats;

private:
    static constexpr size_t BUFFER_SIZE = 100;
    std::array<double, BUFFER_SIZE> prices_{};
    size_t price_idx_ = 0;
    size_t sample_count_ = 0;
    int current_hour_ = 0;
    double last_price_ = 0;

    double calculate_sma(int period) const {
        if (period > static_cast<int>(sample_count_)) period = sample_count_;
        double sum = 0;
        for (int i = 0; i < period; i++) {
            int idx = (price_idx_ - 1 - i + BUFFER_SIZE) % BUFFER_SIZE;
            sum += prices_[idx];
        }
        return sum / period;
    }

    double calculate_std(int period, double mean) const {
        if (period > static_cast<int>(sample_count_)) period = sample_count_;
        double sum_sq = 0;
        for (int i = 0; i < period; i++) {
            int idx = (price_idx_ - 1 - i + BUFFER_SIZE) % BUFFER_SIZE;
            double diff = prices_[idx] - mean;
            sum_sq += diff * diff;
        }
        return std::sqrt(sum_sq / period);
    }

    double calculate_rsi(int period) const {
        if (period >= static_cast<int>(sample_count_)) return 50.0;

        double gains = 0, losses = 0;
        for (int i = 0; i < period; i++) {
            int idx = (price_idx_ - 1 - i + BUFFER_SIZE) % BUFFER_SIZE;
            int prev_idx = (idx - 1 + BUFFER_SIZE) % BUFFER_SIZE;
            double change = prices_[idx] - prices_[prev_idx];
            if (change > 0) gains += change;
            else losses -= change;
        }

        if (losses == 0) return 100.0;
        double rs = gains / losses;
        return 100.0 - (100.0 / (1.0 + rs));
    }

    double calculate_momentum(int period) const {
        if (period >= static_cast<int>(sample_count_)) return 0.0;
        int old_idx = (price_idx_ - period + BUFFER_SIZE) % BUFFER_SIZE;
        return (last_price_ - prices_[old_idx]) / prices_[old_idx];
    }
};

/**
 * Why this strategy will fail in live trading:
 *
 * 1. TIME-OF-DAY PATTERNS: The "good hours" were likely random noise
 *    in a 3-month sample. Markets don't follow fixed hourly patterns.
 *
 * 2. MAGIC NUMBERS: BB period 13.7, RSI 23.4, etc. were optimized
 *    to fit historical data. Slightly different values would have
 *    given completely different results.
 *
 * 3. VOLATILITY THRESHOLD: 0.0342 is suspiciously precise. It was
 *    the value that maximized backtest returns, not a meaningful level.
 *
 * 4. MULTIPLE CONDITIONS: Requiring ALL conditions (BB + RSI + momentum +
 *    time + volatility) means very few trades, and those trades were
 *    cherry-picked by the optimizer.
 *
 * 5. NO ADAPTATION: Market regimes change. Parameters that worked in
 *    Q1 2024 probably won't work in Q2 2024.
 *
 * Expected failure modes:
 * - Too few signals (filters are too specific)
 * - Signals at wrong times (patterns don't persist)
 * - Larger losses than backtest (slippage, execution)
 */

}  // namespace strategy
}  // namespace hft
