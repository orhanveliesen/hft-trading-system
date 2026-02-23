#include "../include/metrics/trade_stream_metrics.hpp"
#include "../include/metrics/trade_stream_metrics_v2.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>

using namespace hft;

template <typename MetricsType>
void benchmark_on_trade(const char* name, int iterations) {
    MetricsType metrics;

    std::mt19937 rng(42);
    std::uniform_int_distribution<Price> price_dist(9500, 10500);
    std::uniform_int_distribution<Quantity> qty_dist(1, 1000);
    std::uniform_int_distribution<int> side_dist(0, 1);

    // Warmup
    for (int i = 0; i < 1000; ++i) {
        metrics.on_trade(price_dist(rng), qty_dist(rng), side_dist(rng), i * 1000);
    }

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        metrics.on_trade(price_dist(rng), qty_dist(rng), side_dist(rng), i * 1000);
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double avg_ns = static_cast<double>(duration) / iterations;

    std::cout << "  " << std::setw(20) << std::left << name << std::setw(12) << std::right << std::fixed
              << std::setprecision(2) << avg_ns << " ns\n";
}

template <typename MetricsType>
void benchmark_get_metrics_cached(const char* name, int iterations) {
    MetricsType metrics;

    std::mt19937 rng(42);
    std::uniform_int_distribution<Price> price_dist(9500, 10500);
    std::uniform_int_distribution<Quantity> qty_dist(1, 1000);
    std::uniform_int_distribution<int> side_dist(0, 1);

    // Fill with 1000 trades
    for (int i = 0; i < 1000; ++i) {
        metrics.on_trade(price_dist(rng), qty_dist(rng), side_dist(rng), i * 1000);
    }

    // Prime cache
    metrics.get_metrics(TradeWindow::W1s);

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        volatile auto m = metrics.get_metrics(TradeWindow::W1s);
        (void)m;
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double avg_ns = static_cast<double>(duration) / iterations;

    std::cout << "  " << std::setw(20) << std::left << name << std::setw(12) << std::right << std::fixed
              << std::setprecision(2) << avg_ns << " ns\n";
}

template <typename MetricsType>
void benchmark_get_metrics_miss(const char* name, int iterations) {
    MetricsType metrics;

    std::mt19937 rng(42);
    std::uniform_int_distribution<Price> price_dist(9500, 10500);
    std::uniform_int_distribution<Quantity> qty_dist(1, 1000);
    std::uniform_int_distribution<int> side_dist(0, 1);

    // Fill with 1000 trades
    for (int i = 0; i < 1000; ++i) {
        metrics.on_trade(price_dist(rng), qty_dist(rng), side_dist(rng), i * 1000);
    }

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        // Invalidate cache by adding trade
        metrics.on_trade(price_dist(rng), qty_dist(rng), side_dist(rng), 1'000'000 + i * 1000);
        volatile auto m = metrics.get_metrics(TradeWindow::W1s);
        (void)m;
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double avg_ns = static_cast<double>(duration) / iterations;

    std::cout << "  " << std::setw(20) << std::left << name << std::setw(12) << std::right << std::fixed
              << std::setprecision(2) << avg_ns << " ns (includes on_trade)\n";
}

template <typename MetricsType>
void benchmark_realistic(const char* name, int iterations) {
    MetricsType metrics;

    std::mt19937 rng(42);
    std::uniform_int_distribution<Price> price_dist(9500, 10500);
    std::uniform_int_distribution<Quantity> qty_dist(1, 1000);
    std::uniform_int_distribution<int> side_dist(0, 1);

    // Warmup
    for (int i = 0; i < 1000; ++i) {
        metrics.on_trade(price_dist(rng), qty_dist(rng), side_dist(rng), i * 1000);
    }

    // Realistic: 100 trades, then 1 query
    auto start = std::chrono::high_resolution_clock::now();
    for (int iter = 0; iter < iterations; ++iter) {
        for (int i = 0; i < 100; ++i) {
            metrics.on_trade(price_dist(rng), qty_dist(rng), side_dist(rng), iter * 100000 + i * 1000);
        }
        volatile auto m = metrics.get_metrics(TradeWindow::W1s);
        (void)m;
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double avg_ns = static_cast<double>(duration) / (iterations * 100);

    std::cout << "  " << std::setw(20) << std::left << name << std::setw(12) << std::right << std::fixed
              << std::setprecision(2) << avg_ns << " ns per trade\n";
}

template <typename MetricsType>
void benchmark_full_pipeline(const char* name, int iterations) {
    MetricsType metrics;

    std::mt19937 rng(42);
    std::uniform_int_distribution<Price> price_dist(9500, 10500);
    std::uniform_int_distribution<Quantity> qty_dist(1, 1000);
    std::uniform_int_distribution<int> side_dist(0, 1);

    // Fill buffer
    for (int i = 0; i < 1000; ++i) {
        metrics.on_trade(price_dist(rng), qty_dist(rng), side_dist(rng), i * 1000);
    }

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        metrics.on_trade(price_dist(rng), qty_dist(rng), side_dist(rng), i * 1000);

        // Query all 5 windows (worst case)
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
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double avg_ns = static_cast<double>(duration) / iterations;
    double avg_us = avg_ns / 1000.0;

    std::cout << "  " << std::setw(20) << std::left << name << std::setw(12) << std::right << std::fixed
              << std::setprecision(2) << avg_ns << " ns (" << avg_us << " μs)\n";
}

int main() {
    std::cout << "=== TradeStreamMetrics V1 vs V2 Performance Comparison ===\n\n";
    std::cout << "V1: Single ring buffer + binary search\n";
    std::cout << "V2: Separate arrays per window (5x insertions, no search)\n\n";

    std::cout << "1. on_trade() Latency\n";
    std::cout << "   ----------------------\n";
    benchmark_on_trade<TradeStreamMetrics>("V1 (single array)", 1'000'000);
    benchmark_on_trade<TradeStreamMetricsV2>("V2 (5 arrays)", 1'000'000);
    std::cout << "\n";

    std::cout << "2. get_metrics() - Cache Hit (1000 trades in buffer)\n";
    std::cout << "   ---------------------------------------------------\n";
    benchmark_get_metrics_cached<TradeStreamMetrics>("V1", 100'000);
    benchmark_get_metrics_cached<TradeStreamMetricsV2>("V2", 100'000);
    std::cout << "\n";

    std::cout << "3. get_metrics() - Cache Miss (recalculation)\n";
    std::cout << "   --------------------------------------------\n";
    benchmark_get_metrics_miss<TradeStreamMetrics>("V1", 10'000);
    benchmark_get_metrics_miss<TradeStreamMetricsV2>("V2", 10'000);
    std::cout << "\n";

    std::cout << "4. Realistic Usage (100 trades + 1 query)\n";
    std::cout << "   ----------------------------------------\n";
    benchmark_realistic<TradeStreamMetrics>("V1", 10'000);
    benchmark_realistic<TradeStreamMetricsV2>("V2", 10'000);
    std::cout << "\n";

    std::cout << "5. Full Pipeline (1 trade + 5 window queries)\n";
    std::cout << "   -------------------------------------------\n";
    benchmark_full_pipeline<TradeStreamMetrics>("V1", 10'000);
    benchmark_full_pipeline<TradeStreamMetricsV2>("V2", 10'000);
    std::cout << "\n";

    std::cout << "=== Summary ===\n";
    std::cout << "SIMD Backend: " << hft::simd::simd_backend << "\n";
    std::cout << "SIMD Width: " << hft::simd::simd_width << " doubles\n\n";

    std::cout << "Memory Usage:\n";
    std::cout << "  V1: ~1.7 MB (65,536 trades × 26 bytes)\n";
    std::cout << "  V2: ~3.2 MB (5 buffers: 2K + 8K + 16K + 32K + 64K)\n\n";

    return 0;
}
