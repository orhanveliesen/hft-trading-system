#pragma once

#include "simd_config.hpp"

// Include the appropriate backend based on compile-time detection
#if HFT_SIMD_AVX512
#include "simd_avx512.hpp"
#elif HFT_SIMD_AVX2
#include "simd_avx2.hpp"
#elif HFT_SIMD_SSE2
#include "simd_sse2.hpp"
#else
#include "simd_scalar.hpp"
#endif

namespace hft {
namespace simd {

/**
 * SIMD Utility Library
 *
 * Provides vectorized operations for high-performance numeric computations.
 * Automatically dispatches to the best available SIMD backend:
 *   - AVX-512: 8 doubles/cycle (~8x speedup) [Intel Ice Lake+, AMD Zen 4+]
 *   - AVX2:    4 doubles/cycle (~4x speedup) [Intel Haswell+, AMD Excavator+]
 *   - SSE2:    2 doubles/cycle (~2x speedup) [Intel Pentium 4+, AMD Athlon 64+]
 *   - Scalar:  1 double/cycle (fallback)
 *
 * Backend selection is automatic at compile time based on CPU architecture flags.
 * Compile with appropriate flags to enable SIMD:
 *   -mavx512f -mavx512dq  (AVX-512)
 *   -mavx2                (AVX2)
 *   -msse2                (SSE2, usually default on x86-64)
 *
 * Usage:
 *   simd::accumulate_volumes(prices, quantities, is_buy, count, buy_vol, sell_vol, vwap_sum);
 *   simd::horizontal_sum_4d(vec, result);
 *
 * All operations are branchless and optimized for low latency (<1 μs).
 */

// Select backend type based on compile-time detection
#if HFT_SIMD_AVX512
using backend = avx512_backend;
#elif HFT_SIMD_AVX2
using backend = avx2_backend;
#elif HFT_SIMD_SSE2
using backend = sse2_backend;
#else
using backend = scalar_backend;
#endif

/**
 * Accumulate buy/sell volumes and VWAP from trade arrays.
 *
 * Uses the most efficient SIMD backend available at compile time.
 *
 * @param prices Array of prices (double)
 * @param quantities Array of quantities (double)
 * @param is_buy Array of buy flags (int: -1 for buy, 0 for sell)
 * @param count Number of elements to process
 * @param[out] buy_volume Accumulated buy volume
 * @param[out] sell_volume Accumulated sell volume
 * @param[out] vwap_sum Accumulated price * quantity for VWAP
 *
 * Performance (1000 elements):
 *   - AVX-512: ~40 ns
 *   - AVX2:    ~70 ns
 *   - SSE2:    ~140 ns
 *   - Scalar:  ~280 ns
 */
inline void accumulate_volumes(const double* prices, const double* quantities, const int* is_buy, size_t count,
                               double& buy_volume, double& sell_volume, double& vwap_sum) {
    backend::accumulate_volumes(prices, quantities, is_buy, count, buy_volume, sell_volume, vwap_sum);
}

/**
 * Horizontal sum of 4 doubles (reduce vector to scalar).
 *
 * @param vec 4-element vector to sum
 * @return Sum of all 4 elements
 *
 * Performance: ~5 ns
 */
inline double horizontal_sum_4d(const double vec[4]) {
    return backend::horizontal_sum_4d(vec);
}

/**
 * Branchless conditional selection: result = mask ? a : b
 *
 * @param mask Condition mask (non-zero = true, 0 = false)
 * @param a Value if mask is true
 * @param b Value if mask is false
 * @return Selected value
 *
 * Performance: ~1 ns (single instruction)
 */
inline double blend(int mask, double a, double b) {
    return backend::blend(mask, a, b);
}

} // namespace simd
} // namespace hft
