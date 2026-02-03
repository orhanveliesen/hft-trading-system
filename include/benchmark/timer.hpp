#pragma once

/**
 * RdtscTimer - RDTSC-based high-resolution timer for nanosecond-level measurements
 *
 * Uses CPU timestamp counter for minimal overhead.
 *
 * NOTE: Header-only by design for HFT performance.
 * All methods inline to eliminate function call overhead.
 */

#include <cstdint>
#include <chrono>
#include <thread>
#include <x86intrin.h>

namespace hft {
namespace benchmark {

class RdtscTimer {
public:
    // Get current timestamp (CPU cycles)
    static inline uint64_t now() {
        return __rdtsc();
    }

    // Get current timestamp with memory fence (more accurate for benchmarks)
    static inline uint64_t now_serialized() {
        unsigned int aux;
        return __rdtscp(&aux);
    }

    // Measure CPU frequency by comparing with steady_clock
    static inline double measure_frequency_ghz() {
        // Warm up
        now_serialized();

        auto start_time = std::chrono::steady_clock::now();
        uint64_t start_cycles = now_serialized();

        // Wait ~100ms
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        uint64_t end_cycles = now_serialized();
        auto end_time = std::chrono::steady_clock::now();

        auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            end_time - start_time).count();

        uint64_t cycles = end_cycles - start_cycles;

        // freq_ghz = cycles / nanoseconds
        return static_cast<double>(cycles) / static_cast<double>(duration_ns);
    }

    // Convert cycles to nanoseconds (requires known frequency)
    static inline double cycles_to_ns(uint64_t cycles, double freq_ghz) {
        return static_cast<double>(cycles) / freq_ghz;
    }
};

// RAII timer for automatic measurement
class ScopedTimer {
public:
    ScopedTimer() : start_(RdtscTimer::now_serialized()) {}

    uint64_t elapsed_cycles() const {
        return RdtscTimer::now_serialized() - start_;
    }

    void reset() {
        start_ = RdtscTimer::now_serialized();
    }

private:
    uint64_t start_;
};

}  // namespace benchmark
}  // namespace hft
