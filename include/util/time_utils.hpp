#pragma once

/**
 * Time utilities for HFT system
 *
 * Provides consistent timestamp generation across all components.
 * Uses steady_clock for monotonic timestamps suitable for latency measurement.
 */

#include <cstdint>
#include <chrono>

namespace hft {
namespace util {

/**
 * Returns current time in nanoseconds since steady_clock epoch.
 * Used for timestamps in trade recording, position tracking, and latency measurement.
 *
 * Note: steady_clock is monotonic (never goes backwards) and suitable for
 * measuring elapsed time. Not suitable for wall-clock time.
 */
inline uint64_t now_ns() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()
    ).count();
}

/**
 * Returns current wall-clock time in nanoseconds since Unix epoch.
 * Use for timestamps that need to correlate with external systems.
 */
inline uint64_t wall_clock_ns() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()
    ).count();
}

}  // namespace util
}  // namespace hft
