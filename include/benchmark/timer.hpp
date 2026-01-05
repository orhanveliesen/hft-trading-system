#pragma once

#include <cstdint>
#include <x86intrin.h>

namespace hft {
namespace benchmark {

// RDTSC-based high-resolution timer for nanosecond-level measurements
// Uses CPU timestamp counter for minimal overhead
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
    static double measure_frequency_ghz();

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
