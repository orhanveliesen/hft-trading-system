#include "../include/simd/simd_ops.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <iostream>

using namespace hft;

void test_simd_backend_detection() {
    std::cout << "SIMD Backend: " << simd::simd_backend << "\n";
    std::cout << "SIMD Width: " << simd::simd_width << " doubles\n";
    std::cout << "SIMD Alignment: " << simd::simd_align << " bytes\n";
    std::cout << "Has SIMD: " << (simd::has_simd() ? "Yes" : "No") << "\n";
    std::cout << "Has AVX-512: " << (simd::has_avx512() ? "Yes" : "No") << "\n";
    std::cout << "Has AVX2: " << (simd::has_avx2() ? "Yes" : "No") << "\n";
    std::cout << "Has SSE2: " << (simd::has_sse2() ? "Yes" : "No") << "\n\n";
}

void test_accumulate_volumes_simple() {
    alignas(64) double prices[] = {10000.0, 10010.0, 10020.0, 10030.0};
    alignas(64) double quantities[] = {100.0, 200.0, 150.0, 250.0};
    alignas(64) int is_buy[] = {-1, 0, -1, 0}; // buy, sell, buy, sell

    double buy_vol = 0.0;
    double sell_vol = 0.0;
    double vwap_sum = 0.0;

    simd::accumulate_volumes(prices, quantities, is_buy, 4, buy_vol, sell_vol, vwap_sum);

    // Expected:
    // buy_vol = 100 + 150 = 250
    // sell_vol = 200 + 250 = 450
    // vwap_sum = 10000*100 + 10010*200 + 10020*150 + 10030*250 = 7'012'500

    assert(std::abs(buy_vol - 250.0) < 0.01);
    assert(std::abs(sell_vol - 450.0) < 0.01);
    assert(std::abs(vwap_sum - 7'012'500.0) < 0.01);

    std::cout << "✓ test_accumulate_volumes_simple\n";
}

void test_accumulate_volumes_all_buy() {
    alignas(64) double prices[] = {10000.0, 10010.0, 10020.0, 10030.0};
    alignas(64) double quantities[] = {100.0, 200.0, 150.0, 250.0};
    alignas(64) int is_buy[] = {-1, -1, -1, -1}; // all buy

    double buy_vol = 0.0;
    double sell_vol = 0.0;
    double vwap_sum = 0.0;

    simd::accumulate_volumes(prices, quantities, is_buy, 4, buy_vol, sell_vol, vwap_sum);

    assert(std::abs(buy_vol - 700.0) < 0.01);
    assert(std::abs(sell_vol - 0.0) < 0.01);

    std::cout << "✓ test_accumulate_volumes_all_buy\n";
}

void test_accumulate_volumes_all_sell() {
    alignas(64) double prices[] = {10000.0, 10010.0, 10020.0, 10030.0};
    alignas(64) double quantities[] = {100.0, 200.0, 150.0, 250.0};
    alignas(64) int is_buy[] = {0, 0, 0, 0}; // all sell

    double buy_vol = 0.0;
    double sell_vol = 0.0;
    double vwap_sum = 0.0;

    simd::accumulate_volumes(prices, quantities, is_buy, 4, buy_vol, sell_vol, vwap_sum);

    assert(std::abs(buy_vol - 0.0) < 0.01);
    assert(std::abs(sell_vol - 700.0) < 0.01);

    std::cout << "✓ test_accumulate_volumes_all_sell\n";
}

void test_accumulate_volumes_large() {
    constexpr size_t N = 1000;
    alignas(64) std::array<double, N> prices;
    alignas(64) std::array<double, N> quantities;
    alignas(64) std::array<int, N> is_buy;

    // Fill with test data
    for (size_t i = 0; i < N; i++) {
        prices[i] = 10000.0 + static_cast<double>(i);
        quantities[i] = 100.0 + static_cast<double>(i % 100);
        is_buy[i] = (i % 2 == 0) ? -1 : 0;
    }

    double buy_vol = 0.0;
    double sell_vol = 0.0;
    double vwap_sum = 0.0;

    simd::accumulate_volumes(prices.data(), quantities.data(), is_buy.data(), N, buy_vol, sell_vol, vwap_sum);

    // Verify with scalar calculation
    double expected_buy = 0.0;
    double expected_sell = 0.0;
    double expected_vwap = 0.0;

    for (size_t i = 0; i < N; i++) {
        if (is_buy[i]) {
            expected_buy += quantities[i];
        } else {
            expected_sell += quantities[i];
        }
        expected_vwap += prices[i] * quantities[i];
    }

    assert(std::abs(buy_vol - expected_buy) < 0.01);
    assert(std::abs(sell_vol - expected_sell) < 0.01);
    assert(std::abs(vwap_sum - expected_vwap) < 0.01);

    std::cout << "✓ test_accumulate_volumes_large (1000 elements)\n";
}

void test_accumulate_volumes_odd_size() {
    // Test with size not divisible by SIMD width
    alignas(64) double prices[] = {10000.0, 10010.0, 10020.0, 10030.0, 10040.0};
    alignas(64) double quantities[] = {100.0, 200.0, 150.0, 250.0, 300.0};
    alignas(64) int is_buy[] = {-1, 0, -1, 0, -1};

    double buy_vol = 0.0;
    double sell_vol = 0.0;
    double vwap_sum = 0.0;

    simd::accumulate_volumes(prices, quantities, is_buy, 5, buy_vol, sell_vol, vwap_sum);

    // Expected:
    // buy_vol = 100 + 150 + 300 = 550
    // sell_vol = 200 + 250 = 450

    assert(std::abs(buy_vol - 550.0) < 0.01);
    assert(std::abs(sell_vol - 450.0) < 0.01);

    std::cout << "✓ test_accumulate_volumes_odd_size\n";
}

void test_horizontal_sum() {
    double vec[4] = {1.0, 2.0, 3.0, 4.0};
    double sum = simd::horizontal_sum_4d(vec);
    assert(std::abs(sum - 10.0) < 0.01);
    std::cout << "✓ test_horizontal_sum\n";
}

void test_blend() {
    double a = 100.0;
    double b = 200.0;

    double result_true = simd::blend(-1, a, b);
    double result_false = simd::blend(0, a, b);

    assert(result_true == a);
    assert(result_false == b);

    std::cout << "✓ test_blend\n";
}

int main() {
    std::cout << "Running SIMD library tests...\n\n";

    test_simd_backend_detection();

    test_accumulate_volumes_simple();
    test_accumulate_volumes_all_buy();
    test_accumulate_volumes_all_sell();
    test_accumulate_volumes_large();
    test_accumulate_volumes_odd_size();
    test_horizontal_sum();
    test_blend();

    std::cout << "\n✅ All 7 SIMD tests passed!\n";
    return 0;
}
