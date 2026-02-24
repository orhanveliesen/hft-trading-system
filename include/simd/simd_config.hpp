#pragma once

#include <cstddef> // for size_t

// SIMD architecture detection and automatic backend selection
// Selects the most effective SIMD instruction set available at compile time

// Detect SIMD support (from most advanced to least)
#if defined(__AVX512F__) && defined(__AVX512DQ__)
#define HFT_SIMD_AVX512 1
#define HFT_SIMD_AVX2 1
#define HFT_SIMD_SSE2 1
#define HFT_SIMD_BACKEND "AVX-512"
#define HFT_SIMD_WIDTH 8  // 8 doubles (512 bits / 64 bits)
#define HFT_SIMD_ALIGN 64 // 64-byte alignment
#elif defined(__AVX2__)
#define HFT_SIMD_AVX512 0
#define HFT_SIMD_AVX2 1
#define HFT_SIMD_SSE2 1
#define HFT_SIMD_BACKEND "AVX2"
#define HFT_SIMD_WIDTH 4  // 4 doubles (256 bits / 64 bits)
#define HFT_SIMD_ALIGN 32 // 32-byte alignment
#elif defined(__SSE2__)
#define HFT_SIMD_AVX512 0
#define HFT_SIMD_AVX2 0
#define HFT_SIMD_SSE2 1
#define HFT_SIMD_BACKEND "SSE2"
#define HFT_SIMD_WIDTH 2  // 2 doubles (128 bits / 64 bits)
#define HFT_SIMD_ALIGN 16 // 16-byte alignment
#else
#define HFT_SIMD_AVX512 0
#define HFT_SIMD_AVX2 0
#define HFT_SIMD_SSE2 0
#define HFT_SIMD_BACKEND "Scalar"
#define HFT_SIMD_WIDTH 1 // Scalar (no vectorization)
#define HFT_SIMD_ALIGN 8 // 8-byte alignment (double)
#endif

// Feature detection macros
#define HFT_HAS_SIMD (HFT_SIMD_WIDTH > 1)

namespace hft {
namespace simd {

// Compile-time constants
inline constexpr size_t simd_width = HFT_SIMD_WIDTH;
inline constexpr size_t simd_align = HFT_SIMD_ALIGN;
inline constexpr const char* simd_backend = HFT_SIMD_BACKEND;

// Check SIMD availability at compile time
inline constexpr bool has_simd() {
    return HFT_HAS_SIMD;
}

inline constexpr bool has_avx512() {
#if HFT_SIMD_AVX512
    return true;
#else
    return false;
#endif
}

inline constexpr bool has_avx2() {
#if HFT_SIMD_AVX2
    return true;
#else
    return false;
#endif
}

inline constexpr bool has_sse2() {
#if HFT_SIMD_SSE2
    return true;
#else
    return false;
#endif
}

} // namespace simd
} // namespace hft
