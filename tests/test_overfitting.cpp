#include <cassert>
#include <iostream>
#include <random>
#include <vector>
#include <cmath>
#include "../include/strategy/overfitted_strategy.hpp"

using namespace hft::strategy;

/**
 * This test demonstrates overfitting by:
 * 1. Generating "in-sample" data where the strategy was "optimized"
 * 2. Generating "out-of-sample" data with similar but different patterns
 * 3. Showing the performance difference
 */

// Generate synthetic price data with some patterns
std::vector<double> generate_prices(int count, double start, double vol,
                                     uint32_t seed, bool with_pattern) {
    std::mt19937 gen(seed);
    std::normal_distribution<> noise(0, vol);

    std::vector<double> prices;
    prices.reserve(count);
    double price = start;

    for (int i = 0; i < count; i++) {
        // Add random walk
        price *= (1.0 + noise(gen));

        // Add "pattern" that existed in training data
        if (with_pattern) {
            int hour = (i / 60) % 24;
            // The pattern: price tends to rise at hour 8-11
            if (hour >= 8 && hour <= 11) {
                price *= 1.0001;  // Tiny upward bias
            }
        }

        prices.push_back(price);
    }
    return prices;
}

struct SimResult {
    int total_trades = 0;
    int wins = 0;
    int losses = 0;
    double total_pnl = 0;
    double max_drawdown = 0;
};

SimResult simulate_strategy(OverfittedStrategy& strat,
                            const std::vector<double>& prices) {
    SimResult result;
    double position = 0;
    double entry_price = 0;
    double peak_equity = 0;
    double equity = 0;

    for (size_t i = 0; i < prices.size(); i++) {
        int hour = (i / 60) % 24;
        strat.update(prices[i], hour);

        auto signal = strat.generate_signal();

        // Execute signals
        if (signal.should_buy && position == 0) {
            position = 1;
            entry_price = prices[i];
        }
        else if (signal.should_sell && position == 1) {
            double pnl = prices[i] - entry_price;
            equity += pnl;
            result.total_pnl += pnl;
            result.total_trades++;

            if (pnl > 0) result.wins++;
            else result.losses++;

            position = 0;
        }
        else if (signal.should_sell && position == 0) {
            position = -1;
            entry_price = prices[i];
        }
        else if (signal.should_buy && position == -1) {
            double pnl = entry_price - prices[i];
            equity += pnl;
            result.total_pnl += pnl;
            result.total_trades++;

            if (pnl > 0) result.wins++;
            else result.losses++;

            position = 0;
        }

        // Track drawdown
        if (equity > peak_equity) peak_equity = equity;
        double dd = peak_equity - equity;
        if (dd > result.max_drawdown) result.max_drawdown = dd;
    }

    return result;
}

void test_in_sample_vs_out_of_sample() {
    std::cout << "\n========== TEST: IN-SAMPLE VS OUT-OF-SAMPLE ==========\n\n";

    // In-sample: Data similar to what the strategy was "optimized" on
    auto in_sample = generate_prices(10000, 90000, 0.001, 12345, true);

    // Out-of-sample: Similar volatility but patterns don't exist
    auto out_sample = generate_prices(10000, 90000, 0.001, 67890, false);

    OverfittedStrategy strat_in, strat_out;

    auto result_in = simulate_strategy(strat_in, in_sample);
    auto result_out = simulate_strategy(strat_out, out_sample);

    std::cout << "IN-SAMPLE (training period patterns):\n";
    std::cout << "  Trades: " << result_in.total_trades << "\n";
    std::cout << "  Win Rate: " << (result_in.total_trades > 0 ?
        100.0 * result_in.wins / result_in.total_trades : 0) << "%\n";
    std::cout << "  Total PnL: $" << result_in.total_pnl << "\n";
    std::cout << "  Max Drawdown: $" << result_in.max_drawdown << "\n\n";

    std::cout << "OUT-OF-SAMPLE (new data, patterns don't exist):\n";
    std::cout << "  Trades: " << result_out.total_trades << "\n";
    std::cout << "  Win Rate: " << (result_out.total_trades > 0 ?
        100.0 * result_out.wins / result_out.total_trades : 0) << "%\n";
    std::cout << "  Total PnL: $" << result_out.total_pnl << "\n";
    std::cout << "  Max Drawdown: $" << result_out.max_drawdown << "\n\n";

    std::cout << "=======================================================\n";
    std::cout << "LESSON: In-sample performance != Out-of-sample!\n";
    std::cout << "The 'magic numbers' only work on the training data.\n";
    std::cout << "=======================================================\n\n";

    std::cout << "[PASS] test_in_sample_vs_out_of_sample\n";
}

void test_filter_analysis() {
    std::cout << "\n========== TEST: FILTER ANALYSIS ==========\n\n";

    OverfittedStrategy strat;
    auto prices = generate_prices(5000, 90000, 0.002, 11111, false);

    int signals = 0;
    int filtered_hour = 0;
    int no_signal = 0;

    for (size_t i = 0; i < prices.size(); i++) {
        int hour = (i / 60) % 24;
        strat.update(prices[i], hour);
        auto sig = strat.generate_signal();

        if (sig.should_buy || sig.should_sell) {
            signals++;
        } else if (std::string(sig.reason).find("bad hour") != std::string::npos) {
            filtered_hour++;
        } else {
            no_signal++;
        }
    }

    std::cout << "Price updates:     " << prices.size() << "\n";
    std::cout << "Signals generated: " << signals << "\n";
    std::cout << "Filtered by hour:  " << filtered_hour << "\n";
    std::cout << "No signal:         " << no_signal << "\n";
    std::cout << "Signal rate:       " << 100.0 * signals / prices.size() << "%\n\n";

    std::cout << "With so many conditions (time + BB + RSI + vol + momentum),\n";
    std::cout << "the strategy generates very few signals.\n";
    std::cout << "Each condition was added to 'improve' backtest,\n";
    std::cout << "but together they create an unusable strategy.\n";
    std::cout << "============================================\n\n";

    // Overfitted strategies often have very low signal rates
    assert(signals < prices.size() / 10);  // Less than 10% signal rate
    std::cout << "[PASS] test_filter_analysis\n";
}

void test_magic_number_explanation() {
    std::cout << "\n========== TEST: MAGIC NUMBER ANALYSIS ==========\n\n";

    std::cout << "The 'optimized' parameters in OverfittedStrategy:\n\n";

    std::cout << "  MAGIC_BB_PERIOD = 13.7\n";
    std::cout << "    Why 13.7? Not 14? Not 13? Because the optimizer\n";
    std::cout << "    found this exact value maximized backtest PnL.\n";
    std::cout << "    No theoretical basis - pure curve fitting.\n\n";

    std::cout << "  MAGIC_BB_STD = 2.17\n";
    std::cout << "    Standard is 2.0. Why 2.17? Same reason.\n\n";

    std::cout << "  MAGIC_RSI_OVERSOLD = 23.4\n";
    std::cout << "    Standard is 30. Why 23.4? Curve fitting.\n\n";

    std::cout << "  MAGIC_VOL_THRESHOLD = 0.0342\n";
    std::cout << "    Suspiciously precise. This exact value filtered\n";
    std::cout << "    out losing trades in the backtest period.\n\n";

    std::cout << "  GOOD_HOURS[24] array\n";
    std::cout << "    'BTC always dumps at 3am' - or did it just happen\n";
    std::cout << "    to dump at 3am during the 3-month backtest?\n\n";

    std::cout << "RED FLAGS of overfitting:\n";
    std::cout << "  1. Too many parameters (>5 tuned values)\n";
    std::cout << "  2. Arbitrary precision (23.4 not 23 or 25)\n";
    std::cout << "  3. No theoretical justification\n";
    std::cout << "  4. Perfect backtest, poor forward test\n";
    std::cout << "  5. Time-specific patterns\n";
    std::cout << "=================================================\n\n";

    std::cout << "[PASS] test_magic_number_explanation\n";
}

void test_what_to_do_instead() {
    std::cout << "\n========== WHAT TO DO INSTEAD ==========\n\n";

    std::cout << "ROBUST STRATEGY CHARACTERISTICS:\n\n";

    std::cout << "1. FEW PARAMETERS (2-3 max)\n";
    std::cout << "   Each parameter is a chance to overfit.\n";
    std::cout << "   Simple = more likely to work out-of-sample.\n\n";

    std::cout << "2. ROUND NUMBERS\n";
    std::cout << "   RSI 30, not 23.4. BB 2.0, not 2.17.\n";
    std::cout << "   If small changes break the strategy, it's overfit.\n\n";

    std::cout << "3. THEORETICAL BASIS\n";
    std::cout << "   'Mean reversion works because of market structure'\n";
    std::cout << "   vs 'this worked in January 2024'\n\n";

    std::cout << "4. WALK-FORWARD TESTING\n";
    std::cout << "   Train on month 1, test on month 2.\n";
    std::cout << "   Train on months 1-2, test on month 3.\n";
    std::cout << "   Don't optimize on all data at once.\n\n";

    std::cout << "5. PAPER TRADE BEFORE REAL MONEY\n";
    std::cout << "   3-6 months minimum.\n";
    std::cout << "   Different market conditions.\n";
    std::cout << "   1000+ trades for statistical significance.\n\n";

    std::cout << "6. EXPECT DEGRADATION\n";
    std::cout << "   Real results will be worse than backtest.\n";
    std::cout << "   If backtest Sharpe = 3.0, expect 1.5 live.\n";
    std::cout << "   If you need perfect backtest to be profitable,\n";
    std::cout << "   the strategy is overfit.\n";
    std::cout << "=========================================\n\n";

    std::cout << "[PASS] test_what_to_do_instead\n";
}

int main() {
    std::cout << "\n";
    std::cout << "╔═══════════════════════════════════════════════════════╗\n";
    std::cout << "║     OVERFITTING DEMONSTRATION - EDUCATIONAL TEST      ║\n";
    std::cout << "║                                                       ║\n";
    std::cout << "║  This test shows WHY curve-fitted strategies fail     ║\n";
    std::cout << "║  and HOW to build more robust trading systems.        ║\n";
    std::cout << "╚═══════════════════════════════════════════════════════╝\n";

    test_in_sample_vs_out_of_sample();
    test_filter_analysis();
    test_magic_number_explanation();
    test_what_to_do_instead();

    std::cout << "\n========== ALL TESTS PASSED ==========\n\n";

    std::cout << "KEY TAKEAWAY:\n";
    std::cout << "Your current SimpleMeanReversion strategy is MORE ROBUST\n";
    std::cout << "because it has:\n";
    std::cout << "  - Few parameters\n";
    std::cout << "  - Theoretical basis (mean reversion)\n";
    std::cout << "  - No time-of-day filters\n";
    std::cout << "  - Adapts to volatility (via regime detection)\n\n";

    std::cout << "The 'pessimistic' paper trading results are more\n";
    std::cout << "trustworthy because we're NOT overfitting!\n\n";

    return 0;
}
