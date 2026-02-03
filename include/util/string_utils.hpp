#pragma once

/**
 * String utilities for HFT system
 *
 * Provides compile-time string conversion utilities used for
 * shared memory versioning and other string operations.
 */

#include <cstdint>

namespace hft {
namespace util {

/**
 * Convert a single hex character to its numeric value (0-15).
 * Returns 0 for invalid characters.
 */
constexpr uint32_t hex_char_to_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return 0;
}

/**
 * Convert first 8 hex characters of a string to uint32_t at compile time.
 * Used to convert git commit hashes to shared memory version numbers.
 *
 * Example: hex_to_u32("deadbeef") -> 0xDEADBEEF
 */
constexpr uint32_t hex_to_u32(const char* s) {
    uint32_t result = 0;
    for (int i = 0; i < 8 && s[i]; ++i) {
        result <<= 4;
        result |= hex_char_to_val(s[i]);
    }
    return result;
}

}  // namespace util
}  // namespace hft
