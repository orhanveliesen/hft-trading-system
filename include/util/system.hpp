#pragma once

/**
 * System utilities for HFT system
 *
 * Provides OS-level utilities for CPU affinity, process management,
 * and signal handling. Linux-specific implementations.
 */

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <sched.h>

namespace hft {
namespace util {

// ============================================================================
// Signal Handler
// ============================================================================

namespace detail {
inline std::atomic<bool>* g_running_flag = nullptr;
inline void (*g_pre_shutdown_callback)() = nullptr;
} // namespace detail

/**
 * Graceful shutdown signal handler.
 *
 * Sets running flag to false and optionally calls pre-shutdown callback.
 * Installed via install_shutdown_handler().
 */
inline void graceful_shutdown_handler(int sig) {
    if (detail::g_pre_shutdown_callback) {
        detail::g_pre_shutdown_callback();
    }
    std::cout << "\n\n[SHUTDOWN] Received signal " << sig << ", stopping gracefully...\n";
    if (detail::g_running_flag) {
        detail::g_running_flag->store(false);
    }
}

/**
 * Install graceful shutdown handler for SIGINT and SIGTERM.
 *
 * @param running Atomic flag to set to false on signal
 * @param pre_shutdown Optional callback to invoke before setting flag (e.g., update shared config)
 */
inline void install_shutdown_handler(std::atomic<bool>& running, void (*pre_shutdown)() = nullptr) {
    detail::g_running_flag = &running;
    detail::g_pre_shutdown_callback = pre_shutdown;
    std::signal(SIGINT, graceful_shutdown_handler);
    std::signal(SIGTERM, graceful_shutdown_handler);
}

// ============================================================================
// CPU Affinity
// ============================================================================

/**
 * Pin current thread to a specific CPU core.
 *
 * CPU pinning reduces context switching and improves cache locality,
 * critical for low-latency trading applications.
 *
 * @param cpu CPU core number to pin to (0-indexed), or -1 to skip pinning
 * @return true if pinning succeeded or was skipped, false on error
 */
inline bool set_cpu_affinity(int cpu) {
    if (cpu < 0)
        return true; // No pinning requested

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);

    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) != 0) {
        std::cerr << "[WARN] Could not pin to CPU " << cpu << ": " << strerror(errno) << "\n";
        return false;
    }
    std::cout << "[CPU] Pinned to core " << cpu << "\n";
    return true;
}

} // namespace util
} // namespace hft
