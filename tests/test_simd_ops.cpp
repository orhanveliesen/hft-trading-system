#include "../include/simd/simd_ops.hpp"

#include <cassert>
#include <iostream>
#include <vector>

using namespace hft::simd;

void test_for_each_step_early_exit_simd() {
    // Test early exit during SIMD loop (covers line 107)
    // Create data larger than SIMD_STEP to ensure SIMD loop runs
    std::vector<int> data(100, 0);

    int iterations = 0;
    for_each_step(0, data.size(), [&](size_t i, size_t step) {
        iterations++;
        if (iterations == 2) {
            // Exit early on second SIMD iteration (covers line 107: return)
            return false;
        }
        return true;
    });

    assert(iterations == 2);
}

void test_for_each_step_early_exit_scalar() {
    // Test early exit during scalar remainder loop (covers line 113)
    // Use count that leaves remainder after SIMD chunks
    size_t count = SIMD_STEP * 2 + 1; // Ensures at least 1 scalar remainder element

    int iterations = 0;
    bool hit_scalar = false;
    for_each_step(0, count, [&](size_t i, size_t step) {
        iterations++;
        if (step == 1) {
            // We're in scalar remainder
            hit_scalar = true;
            // Exit early during scalar remainder (covers line 113: return)
            return false;
        }
        return true; // Continue through SIMD chunks
    });

    // Should have completed SIMD chunks and hit at least one scalar iteration
    assert(hit_scalar);
    assert(iterations >= 2); // At least 2 SIMD chunks + 1 scalar
}

void test_for_each_step_no_early_exit() {
    // Test normal completion without early exit
    std::vector<int> data(20, 0);

    int count = 0;
    for_each_step(0, data.size(), [&](size_t i, size_t step) {
        count++;
        return true; // Never exit early
    });

    // Should have processed all elements
    assert(count > 0);
}

void test_for_each_basic() {
    // Test the basic for_each function (without early exit)
    // Use size that guarantees scalar remainder for all architectures
    size_t size = SIMD_STEP * 3 + 1; // Always leaves 1 element for scalar
    std::vector<double> data(size, 1.0);
    std::vector<double> result(size, 0.0);

    for_each(
        0, data.size(),
        // SIMD func
        [&](size_t i) {
            for (size_t j = 0; j < SIMD_STEP; ++j) {
                result[i + j] = data[i + j] * 2.0;
            }
        },
        // Scalar func
        [&](size_t i) { result[i] = data[i] * 2.0; });

    // Check all elements were processed
    for (size_t i = 0; i < data.size(); ++i) {
        assert(result[i] == 2.0);
    }
}

int main() {
    test_for_each_step_early_exit_simd();
    test_for_each_step_early_exit_scalar();
    test_for_each_step_no_early_exit();
    test_for_each_basic();

    std::cout << "All simd_ops tests passed!\n";
    return 0;
}
