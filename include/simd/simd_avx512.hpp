#pragma once

#include <cstddef>

// Prevent direct inclusion of this header outside SIMD library
#ifndef HFT_SIMD_INTERNAL_INCLUDE_ALLOWED
#error "Do not include simd_avx512.hpp directly. Use simd_ops.hpp instead."
#endif

#include <immintrin.h>

namespace hft {
namespace simd {

/**
 * AVX-512 backend for SIMD operations (512-bit, 8 doubles at once).
 *
 * Requires: CPU with AVX-512F and AVX-512DQ support
 *   - Intel: Ice Lake+, Sapphire Rapids+
 *   - AMD: Zen 4+ (Ryzen 7000+, EPYC Genoa+)
 * Compile with: -mavx512f -mavx512dq
 *
 * Performance: ~8x faster than scalar, ~2x faster than AVX2
 */
struct avx512_backend {
    /**
     * Accumulate buy/sell volumes and VWAP using AVX-512 vectorization.
     *
     * Processes 8 doubles per iteration (512-bit SIMD).
     * Uses AVX-512 mask registers for branchless conditional operations.
     *
     * @param prices Array of prices (64-byte aligned for best performance)
     * @param quantities Array of quantities
     * @param is_buy Array of buy flags (int: -1 = all bits set for buy, 0 for sell)
     * @param count Number of elements
     * @param[out] buy_volume Total buy volume
     * @param[out] sell_volume Total sell volume
     * @param[out] vwap_sum Sum of price * quantity
     */
    static void accumulate_volumes(const double* prices, const double* quantities, const int* is_buy, size_t count,
                                   double& buy_volume, double& sell_volume, double& vwap_sum) {
        __m512d buy_vec = _mm512_setzero_pd();
        __m512d sell_vec = _mm512_setzero_pd();
        __m512d vwap_vec = _mm512_setzero_pd();

        size_t i = 0;

        // Process 8 doubles at a time
        for (; i + 7 < count; i += 8) {
            __m512d p = _mm512_loadu_pd(&prices[i]);
            __m512d q = _mm512_loadu_pd(&quantities[i]);

            // Load 8 ints and create mask register
            __m256i is_buy_i = _mm256_loadu_si256((__m256i*)&is_buy[i]);
            __mmask8 mask = _mm256_movepi32_mask(is_buy_i); // Convert to mask register

            // Branchless conditional accumulation using mask operations
            // buy_qty = is_buy ? qty : 0
            __m512d buy_q = _mm512_maskz_mov_pd(mask, q);
            // sell_qty = !is_buy ? qty : 0
            __m512d sell_q = _mm512_maskz_mov_pd(~mask, q);

            buy_vec = _mm512_add_pd(buy_vec, buy_q);
            sell_vec = _mm512_add_pd(sell_vec, sell_q);

            // VWAP accumulation: price * qty
            __m512d weighted = _mm512_mul_pd(p, q);
            vwap_vec = _mm512_add_pd(vwap_vec, weighted);
        }

        // Horizontal sum (reduce 8 doubles to 1) using AVX-512 reduction
        buy_volume = _mm512_reduce_add_pd(buy_vec);
        sell_volume = _mm512_reduce_add_pd(sell_vec);
        vwap_sum = _mm512_reduce_add_pd(vwap_vec);

        // Handle remaining elements (scalar fallback)
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
