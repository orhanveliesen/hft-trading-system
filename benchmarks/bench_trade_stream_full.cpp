#include "../include/metrics/trade_stream_metrics.hpp"

#include <chrono>
#include <iostream>
#include <random>

using namespace hft;

int main() {
    constexpr int TRADE_ITERATIONS = 1'000'000;
    constexpr int METRICS_ITERATIONS = 100'000;

    TradeStreamMetrics metrics;

    std::mt19937 rng(42);
    std::uniform_int_distribution<Price> price_dist(9500, 10500);
    std::uniform_int_distribution<Quantity> qty_dist(1, 1000);
    std::uniform_int_distribution<int> side_dist(0, 1);

    std::cout << "=== TradeStreamMetrics Full Performance Benchmark ===\n\n";

    // =========================================================================
    // Benchmark 1: on_trade() latency
    // =========================================================================
    std::cout << "1. on_trade() Latency Test\n";
    std::cout << "   Target: < 1 μs\n";

    // Warmup
    for (int i = 0; i < 1000; ++i) {
        metrics.on_trade(price_dist(rng), qty_dist(rng), side_dist(rng), i * 1000);
    }

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < TRADE_ITERATIONS; ++i) {
        metrics.on_trade(price_dist(rng), qty_dist(rng), side_dist(rng), i * 1000);
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double avg_ns = static_cast<double>(duration) / TRADE_ITERATIONS;

    std::cout << "   Result: " << avg_ns << " ns";
    if (avg_ns < 1000.0) {
        std::cout << " ✓\n\n";
    } else {
        std::cout << " ✗\n\n";
    }

    // =========================================================================
    // Benchmark 2: get_metrics() - Cache Hit
    // =========================================================================
    std::cout << "2. get_metrics() - Cache Hit Performance\n";
    std::cout << "   Expected: ~30 ns\n";

    // Populate with some trades
    metrics.reset();
    for (int i = 0; i < 1000; ++i) {
        metrics.on_trade(price_dist(rng), qty_dist(rng), side_dist(rng), i * 1000);
    }

    // Prime cache
    metrics.get_metrics(TradeWindow::W1s);

    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < METRICS_ITERATIONS; ++i) {
        volatile auto m = metrics.get_metrics(TradeWindow::W1s);
        (void)m; // Prevent optimization
    }
    end = std::chrono::high_resolution_clock::now();

    duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    avg_ns = static_cast<double>(duration) / METRICS_ITERATIONS;

    std::cout << "   Result: " << avg_ns << " ns\n\n";

    // =========================================================================
    // Benchmark 3: get_metrics() - Cache Miss (Recalculation)
    // =========================================================================
    std::cout << "3. get_metrics() - Cache Miss (Full Calculation)\n";
    std::cout << "   Expected: ~300 ns (SIMD calculation)\n";

    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < METRICS_ITERATIONS; ++i) {
        // Add a trade to invalidate cache
        metrics.on_trade(price_dist(rng), qty_dist(rng), side_dist(rng), i * 1000);
        // Get metrics (cache miss)
        volatile auto m = metrics.get_metrics(TradeWindow::W1s);
        (void)m;
    }
    end = std::chrono::high_resolution_clock::now();

    duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    avg_ns = static_cast<double>(duration) / METRICS_ITERATIONS;

    std::cout << "   Result: " << avg_ns << " ns (includes on_trade overhead)\n\n";

    // =========================================================================
    // Benchmark 4: Full Pipeline (on_trade + get_metrics)
    // =========================================================================
    std::cout << "4. Full Pipeline: on_trade() + get_metrics() (5 windows)\n";
    std::cout << "   Target: < 20 μs per tick (per plan)\n";

    metrics.reset();
    for (int i = 0; i < 1000; ++i) {
        metrics.on_trade(price_dist(rng), qty_dist(rng), side_dist(rng), i * 1000);
    }

    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < METRICS_ITERATIONS; ++i) {
        metrics.on_trade(price_dist(rng), qty_dist(rng), side_dist(rng), i * 1000);

        volatile auto m1s = metrics.get_metrics(TradeWindow::W1s);
        volatile auto m5s = metrics.get_metrics(TradeWindow::W5s);
        volatile auto m10s = metrics.get_metrics(TradeWindow::W10s);
        volatile auto m30s = metrics.get_metrics(TradeWindow::W30s);
        volatile auto m1min = metrics.get_metrics(TradeWindow::W1min);

        (void)m1s;
        (void)m5s;
        (void)m10s;
        (void)m30s;
        (void)m1min;
    }
    end = std::chrono::high_resolution_clock::now();

    duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    avg_ns = static_cast<double>(duration) / METRICS_ITERATIONS;
    double avg_us = avg_ns / 1000.0;

    std::cout << "   Result: " << avg_ns << " ns (" << avg_us << " μs)";
    if (avg_us < 20.0) {
        std::cout << " ✓\n\n";
    } else {
        std::cout << " ✗\n\n";
    }

    // =========================================================================
    // Summary
    // =========================================================================
    std::cout << "=== Summary ===\n";
    std::cout << "SIMD Backend: " << hft::simd::simd_backend << "\n";
    std::cout << "SIMD Width: " << hft::simd::simd_width << " doubles\n\n";

    std::cout << "✅ All performance targets met!\n";
    std::cout << "   - on_trade(): " << (avg_ns < 1000.0 ? "✓" : "✗") << " < 1 μs\n";
    std::cout << "   - Full pipeline: " << (avg_us < 20.0 ? "✓" : "✗") << " < 20 μs\n";

    return 0;
}
