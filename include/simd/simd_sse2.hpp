#pragma once

#include <cstddef>
#include <emmintrin.h> // SSE2 intrinsics

namespace hft {
namespace simd {

/**
 * SSE2 backend for SIMD operations (128-bit, 2 doubles at once).
 *
 * Requires: CPU with SSE2 support (Intel Pentium 4+, AMD Athlon 64+)
 * Compile with: -msse2 (usually enabled by default on x86-64)
 *
 * Performance: ~2x faster than scalar
 */
struct sse2_backend {
    /**
     * Accumulate buy/sell volumes and VWAP using SSE2 vectorization.
     *
     * Processes 2 doubles per iteration (128-bit SIMD).
     *
     * @param prices Array of prices
     * @param quantities Array of quantities
     * @param is_buy Array of buy flags (int: -1 = all bits set for buy, 0 for sell)
     * @param count Number of elements
     * @param[out] buy_volume Total buy volume
     * @param[out] sell_volume Total sell volume
     * @param[out] vwap_sum Sum of price * quantity
     */
    static void accumulate_volumes(const double* prices, const double* quantities, const int* is_buy, size_t count,
                                   double& buy_volume, double& sell_volume, double& vwap_sum) {
        // Reset output variables (must match scalar backend behavior)
        buy_volume = 0.0;
        sell_volume = 0.0;
        vwap_sum = 0.0;

        __m128d buy_vec = _mm_setzero_pd();
        __m128d sell_vec = _mm_setzero_pd();
        __m128d vwap_vec = _mm_setzero_pd();

        size_t i = 0;

        // Process 2 doubles at a time
        for (; i + 1 < count; i += 2) {
            __m128d p = _mm_loadu_pd(&prices[i]);
            __m128d q = _mm_loadu_pd(&quantities[i]);

            // Load 2 ints and convert to double mask
            // is_buy[i] is -1 (all bits set) for buy, 0 for sell
            int is_buy_0 = is_buy[i];
            int is_buy_1 = is_buy[i + 1];

            // Create mask: set all bits if is_buy[i] != 0
            __m128d mask = _mm_castsi128_pd(_mm_set_epi64x(is_buy_1 ? -1LL : 0LL, is_buy_0 ? -1LL : 0LL));

            // Branchless conditional accumulation
            __m128d buy_q = _mm_and_pd(q, mask);
            __m128d sell_q = _mm_andnot_pd(mask, q);

            buy_vec = _mm_add_pd(buy_vec, buy_q);
            sell_vec = _mm_add_pd(sell_vec, sell_q);

            // VWAP accumulation
            __m128d weighted = _mm_mul_pd(p, q);
            vwap_vec = _mm_add_pd(vwap_vec, weighted);
        }

        // Horizontal sum (reduce 2 doubles to 1)
        alignas(16) double buy_arr[2], sell_arr[2], vwap_arr[2];
        _mm_store_pd(buy_arr, buy_vec);
        _mm_store_pd(sell_arr, sell_vec);
        _mm_store_pd(vwap_arr, vwap_vec);

        buy_volume = buy_arr[0] + buy_arr[1];
        sell_volume = sell_arr[0] + sell_arr[1];
        vwap_sum = vwap_arr[0] + vwap_arr[1];

        // Handle remaining elements (scalar)
        for (; i < count; i++) {
            double qty = quantities[i];
            buy_volume += is_buy[i] ? qty : 0.0;
            sell_volume += is_buy[i] ? 0.0 : qty;
            vwap_sum += prices[i] * qty;
        }
    }

    static double horizontal_sum_4d(const double vec[4]) { return vec[0] + vec[1] + vec[2] + vec[3]; }

    static double blend(int mask, double a, double b) { return mask ? a : b; }
};

} // namespace simd
} // namespace hft
