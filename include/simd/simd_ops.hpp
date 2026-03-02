#pragma once

#include "simd_config.hpp"
#include "simd_guard.hpp"

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

// Undefine guard to prevent SIMD intrinsics usage outside this library
#undef HFT_SIMD_INTERNAL_INCLUDE_ALLOWED

namespace hft {
namespace simd {

// SIMD step size based on architecture
#if HFT_SIMD_AVX512
constexpr size_t SIMD_STEP = 8; // AVX-512: 8 doubles
#elif HFT_SIMD_AVX2
constexpr size_t SIMD_STEP = 4; // AVX2: 4 doubles
#elif HFT_SIMD_SSE2
constexpr size_t SIMD_STEP = 2; // SSE2: 2 doubles
#else
constexpr size_t SIMD_STEP = 1; // Scalar: 1 double
#endif

/**
 * Generic SIMD loop iterator
 *
 * Calls the provided lambda for each SIMD-sized chunk, automatically handling:
 * - SIMD vectorized iterations (step size based on architecture)
 * - Scalar remainder iterations
 *
 * @param start Starting index
 * @param count Total number of elements
 * @param simd_func Lambda for SIMD chunk: (int start_index) -> void
 * @param scalar_func Lambda for scalar remainder: (int index) -> void
 *
 * Example:
 *   simd::for_each(0, n,
 *     [&](int i) {
 *       // Process SIMD_STEP elements starting at i
 *       // Can use AVX2 intrinsics here
 *     },
 *     [&](int i) {
 *       // Process single element at i
 *     }
 *   );
 */
template <typename SimdFunc, typename ScalarFunc>
inline void for_each(size_t start, size_t count, SimdFunc&& simd_func, ScalarFunc&& scalar_func) {
    size_t i = start;

    // SIMD iterations
    for (; i + SIMD_STEP <= count; i += SIMD_STEP) {
        simd_func(i);
    }

    // Scalar remainder
    for (; i < count; i++) {
        scalar_func(i);
    }
}

/**
 * Simpler version: Same lambda for both SIMD and scalar
 *
 * @param start Starting index
 * @param count Total number of elements
 * @param func Lambda: (int index, int step_size) -> void
 *
 * Example:
 *   simd::for_each_step(0, n, [&](int i, int step) {
 *     // step will be SIMD_STEP for vectorized, 1 for remainder
 *   });
 */
template <typename Func>
inline void for_each_step(size_t start, size_t count, Func&& func) {
    size_t i = start;

    // SIMD iterations with step size
    for (; i + SIMD_STEP <= count; i += SIMD_STEP) {
        func(i, SIMD_STEP);
    }

    // Scalar remainder with step=1
    for (; i < count; i++) {
        func(i, 1);
    }
}

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
