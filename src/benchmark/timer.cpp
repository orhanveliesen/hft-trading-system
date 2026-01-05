#include "../../include/benchmark/timer.hpp"
#include <chrono>
#include <thread>

namespace hft {
namespace benchmark {

double RdtscTimer::measure_frequency_ghz() {
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

}  // namespace benchmark
}  // namespace hft
