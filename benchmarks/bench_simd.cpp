#include "../include/simd/simd_ops.hpp"

#include <array>
#include <chrono>
#include <iostream>
#include <random>

int main() {
    constexpr size_t COUNT = 1000;
    constexpr int ITERATIONS = 100'000;

    // Prepare aligned test data
    alignas(64) std::array<double, COUNT> prices;
    alignas(64) std::array<double, COUNT> quantities;
    alignas(64) std::array<int, COUNT> is_buy;

    std::mt19937 rng(42);
    std::uniform_real_distribution<double> price_dist(9500.0, 10500.0);
    std::uniform_real_distribution<double> qty_dist(1.0, 1000.0);

    for (size_t i = 0; i < COUNT; i++) {
        prices[i] = price_dist(rng);
        quantities[i] = qty_dist(rng);
        is_buy[i] = (i % 2 == 0) ? -1 : 0;
    }

    // Warmup
    for (int i = 0; i < 1000; i++) {
        double b, s, v;
        hft::simd::accumulate_volumes(prices.data(), quantities.data(), is_buy.data(), COUNT, b, s, v);
    }

    // Benchmark
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < ITERATIONS; i++) {
        double b, s, v;
        hft::simd::accumulate_volumes(prices.data(), quantities.data(), is_buy.data(), COUNT, b, s, v);
        // Prevent optimization
        asm volatile("" : : "r"(b), "r"(s), "r"(v) : "memory");
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double ns_per_iter = static_cast<double>(ns) / ITERATIONS;

    std::cout << "=== SIMD Performance Benchmark ===\n\n";
    std::cout << "Backend: " << hft::simd::simd_backend << "\n";
    std::cout << "SIMD Width: " << hft::simd::simd_width << " doubles\n";
    std::cout << "Elements: " << COUNT << " trades\n";
    std::cout << "Iterations: " << ITERATIONS << "\n\n";

    std::cout << "Time per accumulation: " << ns_per_iter << " ns\n";
    std::cout << "Throughput: " << (COUNT * ITERATIONS * 1e9) / ns << " elements/sec\n";
    std::cout << "Throughput: " << (ITERATIONS * 1e9) / ns << " iterations/sec\n\n";

    // Calculate theoretical speedup
    if (hft::simd::has_avx512()) {
        std::cout << "Expected speedup vs scalar: ~8x (AVX-512)\n";
    } else if (hft::simd::has_avx2()) {
        std::cout << "Expected speedup vs scalar: ~4x (AVX2)\n";
    } else if (hft::simd::has_sse2()) {
        std::cout << "Expected speedup vs scalar: ~2x (SSE2)\n";
    } else {
        std::cout << "Scalar backend (no SIMD)\n";
    }

    std::cout << "\n✅ Benchmark complete!\n";
    return 0;
}
