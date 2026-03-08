#include "../include/metrics/futures_metrics.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace hft;

// Helper for double comparison
bool approx_equal(double a, double b, double epsilon = 1e-6) {
    return std::abs(a - b) < epsilon;
}

// ============================================================================
// Funding Rate Tests (4)
// ============================================================================

void test_funding_rate_storage() {
    FuturesMetrics fm;
    fm.on_mark_price(42000.0, 41950.0, 0.0005, 0, 1000000);

    auto m = fm.get_metrics(FuturesWindow::W1s);
    assert(approx_equal(m.funding_rate, 0.0005));
    std::cout << "✓ test_funding_rate_storage\n";
}

void test_funding_rate_ema_convergence() {
    FuturesMetrics fm;

    // Feed 60 identical rates
    for (int i = 0; i < 60; i++) {
        fm.on_mark_price(42000.0, 41950.0, 0.0005, 0, 1000000 + i * 1000000);
        fm.update(1000000 + i * 1000000);
    }

    // EMA should converge to 0.0005 for all windows
    for (auto window :
         {FuturesWindow::W1s, FuturesWindow::W5s, FuturesWindow::W10s, FuturesWindow::W30s, FuturesWindow::W1min}) {
        auto m = fm.get_metrics(window);
        assert(approx_equal(m.funding_rate_ema, 0.0005, 1e-4)); // Slightly larger epsilon for convergence
    }
    std::cout << "✓ test_funding_rate_ema_convergence\n";
}

void test_funding_rate_extreme_threshold() {
    FuturesMetrics fm;

    // Test extreme = true
    fm.on_mark_price(42000.0, 41950.0, 0.0015, 0, 1000000);
    auto m1 = fm.get_metrics(FuturesWindow::W1s);
    assert(m1.funding_rate_extreme == true);

    // Test extreme = false
    fm.on_mark_price(42000.0, 41950.0, 0.0001, 0, 2000000);
    auto m2 = fm.get_metrics(FuturesWindow::W1s);
    assert(m2.funding_rate_extreme == false);

    std::cout << "✓ test_funding_rate_extreme_threshold\n";
}

void test_negative_funding_rate() {
    FuturesMetrics fm;
    fm.on_mark_price(42000.0, 41950.0, -0.0008, 0, 1000000);

    auto m = fm.get_metrics(FuturesWindow::W1s);
    assert(approx_equal(m.funding_rate, -0.0008));
    assert(m.funding_rate_extreme == false); // |-0.0008| < 0.001
    std::cout << "✓ test_negative_funding_rate\n";
}

// ============================================================================
// Basis Tests (5)
// ============================================================================

void test_basis_calculation() {
    FuturesMetrics fm;

    // Futures mid = 42100, spot mid = 42000
    fm.on_futures_bbo(42090.0, 42110.0, 1000000); // Mid = 42100
    fm.on_spot_bbo(41990.0, 42010.0, 1000000);    // Mid = 42000

    auto m = fm.get_metrics(FuturesWindow::W1s);
    assert(approx_equal(m.basis, 100.0));
    assert(approx_equal(m.basis_bps, 23.8, 0.1)); // (100/42000)*10000 = 23.8 bps
    std::cout << "✓ test_basis_calculation\n";
}

void test_basis_backwardation() {
    FuturesMetrics fm;

    // Futures below spot (backwardation)
    fm.on_futures_bbo(41990.0, 42010.0, 1000000); // Mid = 42000
    fm.on_spot_bbo(42090.0, 42110.0, 1000000);    // Mid = 42100

    auto m = fm.get_metrics(FuturesWindow::W1s);
    assert(approx_equal(m.basis, -100.0));
    assert(m.basis_bps < 0.0);
    std::cout << "✓ test_basis_backwardation\n";
}

void test_basis_missing_spot() {
    FuturesMetrics fm;

    // Only futures received
    fm.on_futures_bbo(42090.0, 42110.0, 1000000);

    auto m = fm.get_metrics(FuturesWindow::W1s);
    assert(approx_equal(m.basis, 0.0));
    assert(approx_equal(m.basis_bps, 0.0));
    std::cout << "✓ test_basis_missing_spot\n";
}

void test_basis_missing_futures() {
    FuturesMetrics fm;

    // Only spot received
    fm.on_spot_bbo(41990.0, 42010.0, 1000000);

    auto m = fm.get_metrics(FuturesWindow::W1s);
    assert(approx_equal(m.basis, 0.0));
    assert(approx_equal(m.basis_bps, 0.0));
    std::cout << "✓ test_basis_missing_futures\n";
}

void test_basis_ema_smoothing() {
    FuturesMetrics fm;

    // First update: basis = 100
    fm.on_futures_bbo(42090.0, 42110.0, 1000000);
    fm.on_spot_bbo(41990.0, 42010.0, 1000000);
    fm.update(1000000);
    auto m1 = fm.get_metrics(FuturesWindow::W5s); // Use W5s (alpha=0.333) not W1s (alpha=1.0)
    double first_ema = m1.basis_ema;
    assert(approx_equal(first_ema, 100.0)); // First value seeds EMA

    // Second update: basis = 200
    fm.on_futures_bbo(42190.0, 42210.0, 2000000);
    fm.on_spot_bbo(41990.0, 42010.0, 2000000);
    fm.update(2000000);
    auto m2 = fm.get_metrics(FuturesWindow::W5s); // W5s has actual smoothing (alpha=0.333)
    // EMA = 0.333 * 200 + 0.667 * 100 = 66.6 + 66.7 = 133.3 (between 100 and 200)
    assert(m2.basis_ema > 100.0 && m2.basis_ema < 200.0);
    std::cout << "✓ test_basis_ema_smoothing\n";
}

// ============================================================================
// Liquidation Tests (6)
// ============================================================================

void test_liquidation_volume_and_count() {
    FuturesMetrics fm;

    // Single liquidation event: 1.0 BTC @ 42000
    fm.on_liquidation(Side::Sell, 42000.0, 1.0, 1000000);

    auto m = fm.get_metrics(FuturesWindow::W1s);
    assert(approx_equal(m.liquidation_volume, 42000.0)); // price * qty
    assert(m.liquidation_count == 1);
    std::cout << "✓ test_liquidation_volume_and_count\n";
}

void test_liquidation_window_expiration() {
    FuturesMetrics fm;

    // Event at t=1s
    fm.on_liquidation(Side::Sell, 42000.0, 1.0, 1000000);

    // Query at t=1s (within window)
    fm.update(1000000);
    auto m1 = fm.get_metrics(FuturesWindow::W1s);
    assert(m1.liquidation_count == 1);

    // Query at t=2.5s (outside 1s window)
    fm.update(2500000);
    auto m2 = fm.get_metrics(FuturesWindow::W1s);
    assert(m2.liquidation_count == 0);

    std::cout << "✓ test_liquidation_window_expiration\n";
}

void test_liquidation_imbalance_sell_only() {
    FuturesMetrics fm;

    // All SELL liquidations (longs getting liquidated)
    fm.on_liquidation(Side::Sell, 42000.0, 1.0, 1000000);
    fm.on_liquidation(Side::Sell, 42000.0, 2.0, 1000000);
    fm.on_liquidation(Side::Sell, 42000.0, 3.0, 1000000);

    auto m = fm.get_metrics(FuturesWindow::W1s);
    assert(approx_equal(m.liquidation_imbalance, 1.0)); // All longs
    std::cout << "✓ test_liquidation_imbalance_sell_only\n";
}

void test_liquidation_imbalance_buy_only() {
    FuturesMetrics fm;

    // All BUY liquidations (shorts getting liquidated)
    fm.on_liquidation(Side::Buy, 42000.0, 1.0, 1000000);
    fm.on_liquidation(Side::Buy, 42000.0, 2.0, 1000000);

    auto m = fm.get_metrics(FuturesWindow::W1s);
    assert(approx_equal(m.liquidation_imbalance, -1.0)); // All shorts
    std::cout << "✓ test_liquidation_imbalance_buy_only\n";
}

void test_liquidation_imbalance_balanced() {
    FuturesMetrics fm;

    // Equal both sides
    fm.on_liquidation(Side::Sell, 42000.0, 1.0, 1000000);
    fm.on_liquidation(Side::Buy, 42000.0, 1.0, 1000000);

    auto m = fm.get_metrics(FuturesWindow::W1s);
    assert(approx_equal(m.liquidation_imbalance, 0.0, 1e-3));
    std::cout << "✓ test_liquidation_imbalance_balanced\n";
}

void test_liquidation_no_events() {
    FuturesMetrics fm;

    auto m = fm.get_metrics(FuturesWindow::W1s);
    assert(approx_equal(m.liquidation_volume, 0.0));
    assert(m.liquidation_count == 0);
    assert(approx_equal(m.long_liquidation_volume, 0.0));
    assert(approx_equal(m.short_liquidation_volume, 0.0));
    assert(approx_equal(m.liquidation_imbalance, 0.0));
    std::cout << "✓ test_liquidation_no_events\n";
}

// ============================================================================
// Edge Case Tests (3)
// ============================================================================

void test_fresh_instance() {
    FuturesMetrics fm;

    auto m = fm.get_metrics(FuturesWindow::W1s);
    assert(approx_equal(m.funding_rate, 0.0));
    assert(approx_equal(m.funding_rate_ema, 0.0));
    assert(m.funding_rate_extreme == false);
    assert(approx_equal(m.basis, 0.0));
    assert(approx_equal(m.basis_bps, 0.0));
    assert(approx_equal(m.basis_ema, 0.0));
    assert(approx_equal(m.liquidation_volume, 0.0));
    assert(m.liquidation_count == 0);
    std::cout << "✓ test_fresh_instance\n";
}

void test_update_without_feed() {
    FuturesMetrics fm;

    // Call update without any data
    fm.update(1000000);

    auto m = fm.get_metrics(FuturesWindow::W1s);
    assert(approx_equal(m.funding_rate, 0.0));
    std::cout << "✓ test_update_without_feed\n";
}

void test_rapid_events() {
    FuturesMetrics fm;

    // 10000 liquidation events (exceeds buffer size of 8192 - tests wrap-around)
    for (int i = 0; i < 10000; i++) {
        fm.on_liquidation(Side::Sell, 42000.0, 0.1, 1000000 + i * 100);
    }

    auto m = fm.get_metrics(FuturesWindow::W1s);
    // Should handle without overflow/crash, ring buffer wraps correctly
    assert(m.liquidation_count > 0);
    std::cout << "✓ test_rapid_events\n";
}

// ============================================================================
// Main
// ============================================================================

int main() {
    // Funding Rate Tests
    test_funding_rate_storage();
    test_funding_rate_ema_convergence();
    test_funding_rate_extreme_threshold();
    test_negative_funding_rate();

    // Basis Tests
    test_basis_calculation();
    test_basis_backwardation();
    test_basis_missing_spot();
    test_basis_missing_futures();
    test_basis_ema_smoothing();

    // Liquidation Tests
    test_liquidation_volume_and_count();
    test_liquidation_window_expiration();
    test_liquidation_imbalance_sell_only();
    test_liquidation_imbalance_buy_only();
    test_liquidation_imbalance_balanced();
    test_liquidation_no_events();

    // Edge Case Tests
    test_fresh_instance();
    test_update_without_feed();
    test_rapid_events();

    std::cout << "\nAll 18 tests passed!\n";
    return 0;
}
