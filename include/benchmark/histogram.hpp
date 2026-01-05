#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <algorithm>
#include <cmath>
#include <limits>

namespace hft {
namespace benchmark {

// Fixed-bucket histogram for latency measurement
// Pre-allocated, no heap allocation during recording
template <size_t NumBuckets = 1000, uint64_t MaxValue = 10000>
class Histogram {
public:
    static constexpr uint64_t BUCKET_SIZE = MaxValue / NumBuckets;

    Histogram() { reset(); }

    void reset() {
        buckets_.fill(0);
        count_ = 0;
        sum_ = 0;
        min_ = std::numeric_limits<uint64_t>::max();
        max_ = 0;
    }

    // Record a value
    void record(uint64_t value) {
        size_t bucket = std::min(value / BUCKET_SIZE, NumBuckets - 1);
        ++buckets_[bucket];
        ++count_;
        sum_ += value;
        min_ = std::min(min_, value);
        max_ = std::max(max_, value);
    }

    // Statistics
    uint64_t count() const { return count_; }
    uint64_t sum() const { return sum_; }
    uint64_t min() const { return count_ > 0 ? min_ : 0; }
    uint64_t max() const { return max_; }

    double mean() const {
        return count_ > 0 ? static_cast<double>(sum_) / count_ : 0.0;
    }

    // Get percentile (0-100)
    uint64_t percentile(double p) const {
        if (count_ == 0) return 0;

        uint64_t target = static_cast<uint64_t>(count_ * p / 100.0);
        uint64_t cumulative = 0;

        for (size_t i = 0; i < NumBuckets; ++i) {
            cumulative += buckets_[i];
            if (cumulative >= target) {
                return i * BUCKET_SIZE + BUCKET_SIZE / 2;  // Bucket midpoint
            }
        }
        return MaxValue;
    }

    uint64_t p50() const { return percentile(50); }
    uint64_t p90() const { return percentile(90); }
    uint64_t p99() const { return percentile(99); }
    uint64_t p999() const { return percentile(99.9); }

private:
    std::array<uint64_t, NumBuckets> buckets_;
    uint64_t count_;
    uint64_t sum_;
    uint64_t min_;
    uint64_t max_;
};

// Throughput measurement
class ThroughputMeter {
public:
    ThroughputMeter() : count_(0), start_cycles_(0) {}

    void start(uint64_t current_cycles) {
        start_cycles_ = current_cycles;
        count_ = 0;
    }

    void record() { ++count_; }
    void record(uint64_t n) { count_ += n; }

    uint64_t count() const { return count_; }

    // Get throughput in operations per second
    double ops_per_second(uint64_t current_cycles, double freq_ghz) const {
        uint64_t elapsed = current_cycles - start_cycles_;
        double seconds = static_cast<double>(elapsed) / (freq_ghz * 1e9);
        return count_ / seconds;
    }

private:
    uint64_t count_;
    uint64_t start_cycles_;
};

}  // namespace benchmark
}  // namespace hft
