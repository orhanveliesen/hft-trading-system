#pragma once

#include <cstddef>

// Prevent direct inclusion of this header outside SIMD library
#ifndef HFT_SIMD_INTERNAL_INCLUDE_ALLOWED
#error "Do not include simd_avx2.hpp directly. Use simd_ops.hpp instead."
#endif

#include <immintrin.h>

namespace hft {
namespace simd {

/**
 * AVX2 backend for SIMD operations (256-bit, 4 doubles at once).
 *
 * Requires: CPU with AVX2 support (Intel Haswell+, AMD Excavator+)
 * Compile with: -mavx2
 */
struct avx2_backend {
    /**
     * Accumulate buy/sell volumes and VWAP using AVX2 vectorization.
     *
     * Processes 4 doubles per iteration (256-bit SIMD).
     * Branchless implementation using bitwise AND/ANDNOT for conditional accumulation.
     *
     * @param prices Array of prices (must be at least 32-byte aligned for best performance)
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

        __m256d buy_vec = _mm256_setzero_pd();
        __m256d sell_vec = _mm256_setzero_pd();
        __m256d vwap_vec = _mm256_setzero_pd();

        size_t i = 0;

        // Process 4 doubles at a time
        for (; i + 3 < count; i += 4) {
            // Load data (unaligned loads for flexibility)
            __m256d p = _mm256_loadu_pd(&prices[i]);
            __m256d q = _mm256_loadu_pd(&quantities[i]);

            // Load is_buy as 4 ints and convert to double mask
            __m128i is_buy_i = _mm_loadu_si128((__m128i*)&is_buy[i]);
            __m256d mask = _mm256_castsi256_pd(_mm256_cvtepi32_epi64(is_buy_i));

            // Branchless conditional accumulation
            // buy_qty = is_buy ? qty : 0
            // sell_qty = is_buy ? 0 : qty
            __m256d buy_q = _mm256_and_pd(q, mask);
            __m256d sell_q = _mm256_andnot_pd(mask, q);

            buy_vec = _mm256_add_pd(buy_vec, buy_q);
            sell_vec = _mm256_add_pd(sell_vec, sell_q);

            // VWAP accumulation: price * qty
            __m256d weighted = _mm256_mul_pd(p, q);
            vwap_vec = _mm256_add_pd(vwap_vec, weighted);
        }

        // Horizontal sum (reduce 4 doubles to 1)
        alignas(32) double buy_arr[4], sell_arr[4], vwap_arr[4];
        _mm256_store_pd(buy_arr, buy_vec);
        _mm256_store_pd(sell_arr, sell_vec);
        _mm256_store_pd(vwap_arr, vwap_vec);

        buy_volume = buy_arr[0] + buy_arr[1] + buy_arr[2] + buy_arr[3];
        sell_volume = sell_arr[0] + sell_arr[1] + sell_arr[2] + sell_arr[3];
        vwap_sum = vwap_arr[0] + vwap_arr[1] + vwap_arr[2] + vwap_arr[3];

        // Handle remaining elements (scalar fallback)
        for (; i < count; i++) {
            double qty = quantities[i];
            buy_volume += is_buy[i] ? qty : 0.0;
            sell_volume += is_buy[i] ? 0.0 : qty;
            vwap_sum += prices[i] * qty;
        }
    }

    /**
     * Horizontal sum of 4 doubles (AVX2).
     *
     * @param vec Array of 4 doubles
     * @return Sum of all elements
     */
    static double horizontal_sum_4d(const double vec[4]) { return vec[0] + vec[1] + vec[2] + vec[3]; }

    /**
     * Branchless blend operation (AVX2).
     *
     * @param mask Condition (all bits set = true)
     * @param a Value if true
     * @param b Value if false
     * @return Selected value
     */
    static double blend(int mask, double a, double b) {
        // Use multiplication: mask ? a : b = (mask & a) | (~mask & b)
        // For doubles, this requires type punning via union or reinterpret_cast
        return mask ? a : b; // Compiler optimizes this to branchless code
    }
};

} // namespace simd
} // namespace hft
