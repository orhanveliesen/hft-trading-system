#pragma once

/**
 * SIMD Guard Header
 *
 * This header MUST be included before any SIMD backend headers.
 * It prevents direct inclusion of SIMD intrinsics outside the SIMD library.
 *
 * Usage:
 *   Include simd_ops.hpp (which includes this) - DON'T include backends directly
 *   DON'T include <immintrin.h>, <xmmintrin.h>, etc. directly in non-SIMD code
 */

// Define guard to allow SIMD backend headers to include intrinsics
#define HFT_SIMD_INTERNAL_INCLUDE_ALLOWED

// After including all SIMD code, this guard will be undefined
// Any attempt to use SIMD intrinsics outside the library will fail
