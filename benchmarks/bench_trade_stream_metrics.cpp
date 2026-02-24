#include "../include/metrics/trade_stream_metrics.hpp"

#include <chrono>
#include <iostream>
#include <random>

using namespace hft;

int main() {
    constexpr int NUM_ITERATIONS = 1'000'000;

    TradeStreamMetrics metrics;

    std::mt19937 rng(42);
    std::uniform_int_distribution<Price> price_dist(9500, 10500);
    std::uniform_int_distribution<Quantity> qty_dist(1, 1000);
    std::uniform_int_distribution<int> side_dist(0, 1);

    // Warmup
    for (int i = 0; i < 1000; ++i) {
        metrics.on_trade(price_dist(rng), qty_dist(rng), side_dist(rng), i * 1000);
    }

    // Benchmark on_trade()
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        uint64_t timestamp = i * 1000; // 1ms between trades
        metrics.on_trade(price_dist(rng), qty_dist(rng), side_dist(rng), timestamp);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration) / NUM_ITERATIONS;
    double avg_us = avg_ns / 1000.0;

    std::cout << "=== TradeStreamMetrics Performance Benchmark ===\n";
    std::cout << "Iterations: " << NUM_ITERATIONS << "\n";
    std::cout << "Total time: " << duration / 1e9 << " seconds\n";
    std::cout << "Average time per on_trade(): " << avg_ns << " ns (" << avg_us << " μs)\n";

    if (avg_us < 1.0) {
        std::cout << "\n✓ Target met: < 1 μs per on_trade()\n";
        return 0;
    } else {
        std::cout << "\n✗ Target missed: " << avg_us << " μs per on_trade() (target: < 1 μs)\n";
        return 1;
    }
}
