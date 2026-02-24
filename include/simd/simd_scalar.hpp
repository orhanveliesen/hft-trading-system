#pragma once

#include <cstddef>

namespace hft {
namespace simd {

/**
 * Scalar backend for SIMD operations (fallback when AVX2 is not available).
 *
 * Provides the same interface as AVX2 backend but uses scalar operations.
 * Performance is ~4x slower than AVX2 but ensures portability.
 */
struct scalar_backend {
    /**
     * Accumulate buy/sell volumes and VWAP using scalar operations.
     *
     * @param prices Array of prices
     * @param quantities Array of quantities
     * @param is_buy Array of buy flags (int: -1 for buy, 0 for sell)
     * @param count Number of elements
     * @param[out] buy_volume Total buy volume
     * @param[out] sell_volume Total sell volume
     * @param[out] vwap_sum Sum of price * quantity
     */
    static void accumulate_volumes(const double* prices, const double* quantities, const int* is_buy, size_t count,
                                   double& buy_volume, double& sell_volume, double& vwap_sum) {
        buy_volume = 0.0;
        sell_volume = 0.0;
        vwap_sum = 0.0;

        for (size_t i = 0; i < count; i++) {
            double qty = quantities[i];
            double price = prices[i];

            // Branchless accumulation
            buy_volume += is_buy[i] ? qty : 0.0;
            sell_volume += is_buy[i] ? 0.0 : qty;
            vwap_sum += price * qty;
        }
    }

    /**
     * Horizontal sum of 4 doubles (scalar).
     *
     * @param vec Array of 4 doubles
     * @return Sum of all elements
     */
    static double horizontal_sum_4d(const double vec[4]) { return vec[0] + vec[1] + vec[2] + vec[3]; }

    /**
     * Branchless blend operation (scalar).
     *
     * @param mask Condition (non-zero = true)
     * @param a Value if true
     * @param b Value if false
     * @return Selected value
     */
    static double blend(int mask, double a, double b) { return mask ? a : b; }
};

} // namespace simd
} // namespace hft
