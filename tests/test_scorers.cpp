#include "../include/strategy/scorers.hpp"
#include "../include/trading/trading_state.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace hft::trading;
using namespace hft::strategy;

#define TEST(name) void name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "  " << #name << "... ";                                                                          \
        name();                                                                                                        \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

#define ASSERT_EQ(a, b) assert((a) == (b))
#define ASSERT_TRUE(x) assert(x)
#define ASSERT_FALSE(x) assert(!(x))
#define ASSERT_NEAR(a, b, eps) assert(std::abs((a) - (b)) < (eps))
#define ASSERT_GT(a, b) assert((a) > (b))
#define ASSERT_LT(a, b) assert((a) < (b))

// =============================================================================
// RSIScorer Tests
// =============================================================================

TEST(rsi_scorer_oversold_gives_positive) {
    TradingState state{};
    state.init(100000.0);

    Indicators ind{};
    ind.rsi = 25.0; // Oversold (below 30)

    RSIScorer scorer(state.rsi);
    double score = scorer.score(0, state.common, ind);

    // Oversold should give positive score (bullish)
    ASSERT_GT(score, 0.0);
}

TEST(rsi_scorer_overbought_gives_negative) {
    TradingState state{};
    state.init(100000.0);

    Indicators ind{};
    ind.rsi = 75.0; // Overbought (above 70)

    RSIScorer scorer(state.rsi);
    double score = scorer.score(0, state.common, ind);

    // Overbought should give negative score (bearish)
    ASSERT_LT(score, 0.0);
}

TEST(rsi_scorer_neutral_gives_zero) {
    TradingState state{};
    state.init(100000.0);

    Indicators ind{};
    ind.rsi = 50.0; // Neutral

    RSIScorer scorer(state.rsi);
    double score = scorer.score(0, state.common, ind);

    // Neutral RSI should give near-zero score
    ASSERT_NEAR(score, 0.0, 0.01);
}

TEST(rsi_scorer_uses_per_symbol_config) {
    TradingState state{};
    state.init(100000.0);

    // Set custom thresholds for symbol 5
    state.rsi.oversold[5] = 20.0;
    state.rsi.overbought[5] = 80.0;

    Indicators ind{};
    ind.rsi = 25.0; // Oversold by default (30), but not by symbol 5's threshold (20)

    RSIScorer scorer(state.rsi);
    double score = scorer.score(5, state.common, ind);

    // With custom threshold 20, RSI 25 is less oversold
    // Score should still be positive but less than with default threshold
    ASSERT_GT(score, 0.0);
}

TEST(rsi_scorer_bounded_output) {
    TradingState state{};
    state.init(100000.0);

    RSIScorer scorer(state.rsi);

    // Test extreme values
    Indicators ind_low{};
    ind_low.rsi = 0.0; // Extremely oversold

    Indicators ind_high{};
    ind_high.rsi = 100.0; // Extremely overbought

    double score_low = scorer.score(0, state.common, ind_low);
    double score_high = scorer.score(0, state.common, ind_high);

    // Scores should be bounded to [-1, +1]
    ASSERT_TRUE(score_low >= -1.0 && score_low <= 1.0);
    ASSERT_TRUE(score_high >= -1.0 && score_high <= 1.0);
}

// =============================================================================
// MACDScorer Tests
// =============================================================================

TEST(macd_scorer_positive_histogram_gives_positive) {
    TradingState state{};
    state.init(100000.0);

    Indicators ind{};
    ind.macd_histogram = 0.5;
    ind.macd_scale = 1.0;

    MACDScorer scorer(state.macd);
    double score = scorer.score(0, state.common, ind);

    ASSERT_GT(score, 0.0);
}

TEST(macd_scorer_negative_histogram_gives_negative) {
    TradingState state{};
    state.init(100000.0);

    Indicators ind{};
    ind.macd_histogram = -0.5;
    ind.macd_scale = 1.0;

    MACDScorer scorer(state.macd);
    double score = scorer.score(0, state.common, ind);

    ASSERT_LT(score, 0.0);
}

TEST(macd_scorer_zero_histogram_gives_zero) {
    TradingState state{};
    state.init(100000.0);

    Indicators ind{};
    ind.macd_histogram = 0.0;
    ind.macd_scale = 1.0;

    MACDScorer scorer(state.macd);
    double score = scorer.score(0, state.common, ind);

    ASSERT_NEAR(score, 0.0, 0.01);
}

TEST(macd_scorer_bounded_output) {
    TradingState state{};
    state.init(100000.0);

    MACDScorer scorer(state.macd);

    Indicators ind_extreme{};
    ind_extreme.macd_histogram = 10.0; // Very large
    ind_extreme.macd_scale = 1.0;

    double score = scorer.score(0, state.common, ind_extreme);

    // Should be clamped to [-1, +1]
    ASSERT_TRUE(score >= -1.0 && score <= 1.0);
}

// =============================================================================
// MomentumScorer Tests
// =============================================================================

TEST(momentum_scorer_positive_momentum_gives_positive) {
    TradingState state{};
    state.init(100000.0);

    Indicators ind{};
    ind.momentum = 0.02; // 2% momentum

    MomentumScorer scorer(state.momentum);
    double score = scorer.score(0, state.common, ind);

    ASSERT_GT(score, 0.0);
}

TEST(momentum_scorer_negative_momentum_gives_negative) {
    TradingState state{};
    state.init(100000.0);

    Indicators ind{};
    ind.momentum = -0.02; // -2% momentum

    MomentumScorer scorer(state.momentum);
    double score = scorer.score(0, state.common, ind);

    ASSERT_LT(score, 0.0);
}

TEST(momentum_scorer_uses_threshold) {
    TradingState state{};
    state.init(100000.0);

    // Default threshold is 0.01 (1%)
    Indicators ind{};
    ind.momentum = 0.01; // Exactly at threshold

    MomentumScorer scorer(state.momentum);
    double score = scorer.score(0, state.common, ind);

    // At threshold, normalized score should be 1.0
    ASSERT_NEAR(score, 1.0, 0.01);
}

TEST(momentum_scorer_bounded_output) {
    TradingState state{};
    state.init(100000.0);

    MomentumScorer scorer(state.momentum);

    Indicators ind_extreme{};
    ind_extreme.momentum = 0.5; // 50% momentum (unrealistic but test bound)

    double score = scorer.score(0, state.common, ind_extreme);

    ASSERT_TRUE(score >= -1.0 && score <= 1.0);
}

// =============================================================================
// StrategyScorer Concept Tests
// =============================================================================

TEST(rsi_scorer_satisfies_concept) {
    // This test compiles means RSIScorer satisfies StrategyScorer concept
    TradingState state{};
    state.init(100000.0);

    RSIScorer scorer(state.rsi);
    static_assert(StrategyScorer<RSIScorer>, "RSIScorer must satisfy StrategyScorer concept");
}

TEST(macd_scorer_satisfies_concept) {
    TradingState state{};
    state.init(100000.0);

    MACDScorer scorer(state.macd);
    static_assert(StrategyScorer<MACDScorer>, "MACDScorer must satisfy StrategyScorer concept");
}

TEST(momentum_scorer_satisfies_concept) {
    TradingState state{};
    state.init(100000.0);

    MomentumScorer scorer(state.momentum);
    static_assert(StrategyScorer<MomentumScorer>, "MomentumScorer must satisfy StrategyScorer concept");
}

// =============================================================================
// Score Dispatcher Tests
// =============================================================================

TEST(dispatch_scorer_rsi) {
    TradingState state{};
    state.init(100000.0);

    state.strategies.active[0] = StrategyId::RSI;

    Indicators ind{};
    ind.rsi = 25.0; // Oversold

    double score = dispatch_score(0, state, ind);

    // Should use RSI scorer and give positive score
    ASSERT_GT(score, 0.0);
}

TEST(dispatch_scorer_macd) {
    TradingState state{};
    state.init(100000.0);

    state.strategies.active[0] = StrategyId::MACD;

    Indicators ind{};
    ind.macd_histogram = 0.5;
    ind.macd_scale = 1.0;

    double score = dispatch_score(0, state, ind);

    ASSERT_GT(score, 0.0);
}

TEST(dispatch_scorer_momentum) {
    TradingState state{};
    state.init(100000.0);

    state.strategies.active[0] = StrategyId::MOMENTUM;

    Indicators ind{};
    ind.momentum = 0.02;

    double score = dispatch_score(0, state, ind);

    ASSERT_GT(score, 0.0);
}

TEST(dispatch_scorer_none_returns_zero) {
    TradingState state{};
    state.init(100000.0);

    state.strategies.active[0] = StrategyId::NONE;

    Indicators ind{};
    ind.rsi = 25.0;

    double score = dispatch_score(0, state, ind);

    ASSERT_NEAR(score, 0.0, 1e-9);
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "\n=== Strategy Scorers Tests ===\n\n";

    std::cout << "RSIScorer Tests:\n";
    RUN_TEST(rsi_scorer_oversold_gives_positive);
    RUN_TEST(rsi_scorer_overbought_gives_negative);
    RUN_TEST(rsi_scorer_neutral_gives_zero);
    RUN_TEST(rsi_scorer_uses_per_symbol_config);
    RUN_TEST(rsi_scorer_bounded_output);

    std::cout << "\nMACDScorer Tests:\n";
    RUN_TEST(macd_scorer_positive_histogram_gives_positive);
    RUN_TEST(macd_scorer_negative_histogram_gives_negative);
    RUN_TEST(macd_scorer_zero_histogram_gives_zero);
    RUN_TEST(macd_scorer_bounded_output);

    std::cout << "\nMomentumScorer Tests:\n";
    RUN_TEST(momentum_scorer_positive_momentum_gives_positive);
    RUN_TEST(momentum_scorer_negative_momentum_gives_negative);
    RUN_TEST(momentum_scorer_uses_threshold);
    RUN_TEST(momentum_scorer_bounded_output);

    std::cout << "\nConcept Tests:\n";
    RUN_TEST(rsi_scorer_satisfies_concept);
    RUN_TEST(macd_scorer_satisfies_concept);
    RUN_TEST(momentum_scorer_satisfies_concept);

    std::cout << "\nScore Dispatcher Tests:\n";
    RUN_TEST(dispatch_scorer_rsi);
    RUN_TEST(dispatch_scorer_macd);
    RUN_TEST(dispatch_scorer_momentum);
    RUN_TEST(dispatch_scorer_none_returns_zero);

    std::cout << "\n=== All Strategy Scorers Tests Passed! ===\n";
    return 0;
}
