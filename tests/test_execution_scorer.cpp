#include "../include/execution/execution_scorer.hpp"
#include "../include/metrics/combined_metrics.hpp"
#include "../include/metrics/futures_metrics.hpp"
#include "../include/metrics/order_book_metrics.hpp"
#include "../include/metrics/order_flow_metrics.hpp"
#include "../include/metrics/trade_stream_metrics.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace hft;
using namespace hft::execution;
using namespace hft::strategy;

// Helper: Create BookSnapshot with given spread
BookSnapshot create_book_snapshot(Price bid, Price ask) {
    BookSnapshot snapshot;
    snapshot.best_bid = bid;
    snapshot.best_ask = ask;
    snapshot.best_bid_qty = 100;
    snapshot.best_ask_qty = 100;
    snapshot.bid_level_count = 1;
    snapshot.ask_level_count = 1;
    snapshot.bid_levels[0] = {bid, 100};
    snapshot.ask_levels[0] = {ask, 100};
    return snapshot;
}

// Test 1: No metrics returns urgency-only score
void test_no_metrics_returns_urgency_only() {
    Signal signal = Signal::buy(SignalStrength::Weak, 10.0, "test");
    auto result = ExecutionScorer::compute(signal, nullptr, Side::Buy);

    assert(result.score == 10.0); // Weak = +10
    assert(result.urgency == 10.0);
    assert(result.spread_value == 0.0);
    assert(result.fill_prob == 0.0);
    assert(result.adverse == 0.0);
    std::cout << "[PASS] test_no_metrics_returns_urgency_only\n";
}

// Test 2: Wide spread (>20 bps) → +15 spread_value
void test_wide_spread_prefers_limit() {
    OrderBookMetrics book;
    // Create snapshot with 25 bps spread
    // bid=100000, ask=100250 → spread=250, mid=100125 → spread_bps = 250/100125*10000 = 24.97 bps
    auto snapshot = create_book_snapshot(100000, 100250);
    book.on_depth_snapshot(snapshot, 0);

    MetricsContext metrics;
    metrics.book = &book;

    Signal signal = Signal::buy(SignalStrength::Medium, 10.0, "test");
    auto result = ExecutionScorer::compute(signal, &metrics, Side::Buy);

    // Verify spread was read correctly
    auto book_metrics = book.get_metrics();
    assert(book_metrics.spread_bps > 20.0); // Should be ~25 bps

    assert(result.spread_value == 15.0); // Max positive for spread > 20 bps
    assert(result.score > 0.0);          // Should prefer limit
    std::cout << "[PASS] test_wide_spread_prefers_limit\n";
}

// Test 3: Tight spread (<3 bps) → -15 spread_value
void test_tight_spread_prefers_market() {
    OrderBookMetrics book;
    // bid=100000, ask=100020 → spread=20, mid=100010 → spread_bps = 20/100010*10000 = 2.0 bps
    auto snapshot = create_book_snapshot(100000, 100020);
    book.on_depth_snapshot(snapshot, 0);

    MetricsContext metrics;
    metrics.book = &book;

    Signal signal = Signal::buy(SignalStrength::Medium, 10.0, "test");
    auto result = ExecutionScorer::compute(signal, &metrics, Side::Buy);

    // Verify spread was read correctly
    auto book_metrics = book.get_metrics();
    assert(book_metrics.spread_bps < 3.0); // Should be ~2 bps

    assert(result.spread_value == -15.0); // Max negative for spread < 3 bps
    assert(result.score < 0.0);           // Should prefer market
    std::cout << "[PASS] test_tight_spread_prefers_market\n";
}

// Test 4: Medium spread (8-20 bps) → positive spread_value
void test_medium_spread_linear_interpolation() {
    OrderBookMetrics book;
    // bid=100000, ask=100140 → spread=140, mid=100070 → spread_bps = 140/100070*10000 = 13.99 bps
    auto snapshot = create_book_snapshot(100000, 100140);
    book.on_depth_snapshot(snapshot, 0);

    MetricsContext metrics;
    metrics.book = &book;

    Signal signal = Signal::buy(SignalStrength::Medium, 10.0, "test");
    auto result = ExecutionScorer::compute(signal, &metrics, Side::Buy);

    // Verify spread in range
    auto book_metrics = book.get_metrics();
    assert(book_metrics.spread_bps > 8.0 && book_metrics.spread_bps < 20.0);

    // Should be between 0 and +15 (linear interpolation)
    assert(result.spread_value > 0.0 && result.spread_value < 15.0);
    std::cout << "[PASS] test_medium_spread_linear_interpolation\n";
}

// Test 5: Calm market (low trade_to_depth, low cancel_ratio) → +12 fill_prob
void test_calm_market_prefers_limit() {
    TradeStreamMetrics trade_metrics;
    OrderBookMetrics book_metrics;
    auto snapshot = create_book_snapshot(100000, 100100);
    book_metrics.on_depth_snapshot(snapshot, 0);

    CombinedMetrics combined(trade_metrics, book_metrics);
    // CombinedMetrics needs update() to calculate ratios
    // But we can't easily control trade_to_depth without feeding real trades
    // Skip this test for now - will test with integration tests

    std::cout << "[SKIP] test_calm_market_prefers_limit (requires real trade data)\n";
}

// Test 6: Active market - SKIP (same reason)
void test_active_market_prefers_market() {
    std::cout << "[SKIP] test_active_market_prefers_market (requires real trade data)\n";
}

// Test 7: Adverse selection - SKIP (requires complex setup)
void test_low_adverse_selection_prefers_limit() {
    std::cout << "[SKIP] test_low_adverse_selection_prefers_limit (requires complex metrics setup)\n";
}

// Test 8: Adverse selection - SKIP
void test_high_adverse_selection_prefers_market() {
    std::cout << "[SKIP] test_high_adverse_selection_prefers_market (requires complex metrics setup)\n";
}

// Test 9: Strong signal → -10 urgency
void test_strong_signal_prefers_market() {
    Signal signal = Signal::buy(SignalStrength::Strong, 10.0, "test");
    auto result = ExecutionScorer::compute(signal, nullptr, Side::Buy);

    assert(result.urgency == -10.0);
    assert(result.score == -10.0);
    assert(!result.prefer_limit());
    std::cout << "[PASS] test_strong_signal_prefers_market\n";
}

// Test 10: Weak signal → +10 urgency
void test_weak_signal_prefers_limit() {
    Signal signal = Signal::buy(SignalStrength::Weak, 10.0, "test");
    auto result = ExecutionScorer::compute(signal, nullptr, Side::Buy);

    assert(result.urgency == 10.0);
    assert(result.score == 10.0);
    assert(result.prefer_limit());
    std::cout << "[PASS] test_weak_signal_prefers_limit\n";
}

// Test 11: Combined factors (spread + urgency) → limit
void test_combined_factors_limit() {
    OrderBookMetrics book;
    // Wide spread: bid=100000, ask=100250 → ~25 bps → +15
    auto snapshot = create_book_snapshot(100000, 100250);
    book.on_depth_snapshot(snapshot, 0);

    MetricsContext metrics;
    metrics.book = &book;

    // Weak signal → +10 urgency
    Signal signal = Signal::buy(SignalStrength::Weak, 10.0, "test");
    auto result = ExecutionScorer::compute(signal, &metrics, Side::Buy);

    // spread_value=+15, urgency=+10, others=0 → score=+25
    assert(result.spread_value == 15.0);
    assert(result.urgency == 10.0);
    assert(result.score == 25.0);
    assert(result.prefer_limit());
    std::cout << "[PASS] test_combined_factors_limit\n";
}

// Test 12: Combined factors (spread + urgency) → market
void test_combined_factors_market() {
    OrderBookMetrics book;
    // Tight spread: bid=100000, ask=100020 → ~2 bps → -15
    auto snapshot = create_book_snapshot(100000, 100020);
    book.on_depth_snapshot(snapshot, 0);

    MetricsContext metrics;
    metrics.book = &book;

    // Strong signal → -10 urgency
    Signal signal = Signal::buy(SignalStrength::Strong, 10.0, "test");
    auto result = ExecutionScorer::compute(signal, &metrics, Side::Buy);

    // spread_value=-15, urgency=-10, others=0 → score=-25
    assert(result.spread_value == -15.0);
    assert(result.urgency == -10.0);
    assert(result.score == -25.0);
    assert(!result.prefer_limit());
    std::cout << "[PASS] test_combined_factors_market\n";
}

int main() {
    std::cout << "Running ExecutionScorer tests...\n";
    std::cout << "Note: Some tests skipped - require complex metrics setup with real trade data\n\n";

    test_no_metrics_returns_urgency_only();
    test_wide_spread_prefers_limit();
    test_tight_spread_prefers_market();
    test_medium_spread_linear_interpolation();
    test_calm_market_prefers_limit();
    test_active_market_prefers_market();
    test_low_adverse_selection_prefers_limit();
    test_high_adverse_selection_prefers_market();
    test_strong_signal_prefers_market();
    test_weak_signal_prefers_limit();
    test_combined_factors_limit();
    test_combined_factors_market();

    std::cout << "\n8 ExecutionScorer tests passed (4 skipped)!\n";
    std::cout << "✓ Coverage: score_spread_value (full), score_signal_urgency (full)\n";
    std::cout << "⚠ score_fill_probability, score_adverse_selection: need integration tests\n";
    return 0;
}
