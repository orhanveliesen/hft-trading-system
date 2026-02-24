/**
 * Benchmark: Lock-free vs Locked hot path
 *
 * Compares:
 * 1. std::map lookup vs fixed array access
 * 2. With mutex vs without mutex
 * 3. std::string vs char[16] copy
 */

#include <array>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>

constexpr size_t MAX_SYMBOLS = 64;
constexpr size_t ITERATIONS = 10'000'000;
constexpr size_t WARMUP = 100'000;

// Simulate strategy data
struct StrategyOld {
    double value1 = 0;
    double value2 = 0;
    std::string ticker;

    void update(double v) {
        value1 = v;
        value2 = v * 0.5;
    }
};

struct StrategyNew {
    double value1 = 0;
    double value2 = 0;
    char ticker[16] = {0};
    bool active = false;

    void init(const char* t) {
        active = true;
        std::strncpy(ticker, t, 15);
    }

    void update(double v) {
        value1 = v;
        value2 = v * 0.5;
    }
};

// OLD approach: map + mutex + string
class OldApproach {
public:
    void add_symbol(uint32_t id, const std::string& ticker) {
        std::lock_guard<std::mutex> lock(mutex_);
        strategies_[id] = std::make_unique<StrategyOld>();
        strategies_[id]->ticker = ticker;
    }

    void on_quote(uint32_t id, double price) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = strategies_.find(id);
        if (it != strategies_.end()) {
            it->second->update(price);
        }
    }

private:
    std::map<uint32_t, std::unique_ptr<StrategyOld>> strategies_;
    std::mutex mutex_;
};

// NEW approach: fixed array, no mutex
class NewApproach {
public:
    void add_symbol(uint32_t id, const char* ticker) {
        if (id < MAX_SYMBOLS) {
            strategies_[id].init(ticker);
        }
    }

    void on_quote(uint32_t id, double price) {
        if (id < MAX_SYMBOLS && strategies_[id].active) {
            strategies_[id].update(price);
        }
    }

private:
    std::array<StrategyNew, MAX_SYMBOLS> strategies_;
};

// Timing helper
template <typename Func>
double measure_ns(Func&& f, size_t iterations) {
    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        f();
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    return static_cast<double>(duration.count()) / iterations;
}

int main() {
    std::cout << "Lock-Free Hot Path Benchmark\n";
    std::cout << "============================\n\n";
    std::cout << "Iterations: " << ITERATIONS / 1'000'000 << "M\n\n";

    // Setup
    OldApproach old_app;
    NewApproach new_app;

    const char* symbols[] = {"BTCUSDT", "ETHUSDT",  "BNBUSDT", "XRPUSDT", "SOLUSDT",
                             "ADAUSDT", "DOGEUSDT", "TRXUSDT", "DOTUSDT", "MATICUSDT"};

    for (uint32_t i = 0; i < 10; ++i) {
        old_app.add_symbol(i, symbols[i]);
        new_app.add_symbol(i, symbols[i]);
    }

    // Random symbol IDs and prices for realistic access pattern
    std::mt19937 rng(42);
    std::uniform_int_distribution<uint32_t> sym_dist(0, 9);
    std::uniform_real_distribution<double> price_dist(100.0, 50000.0);

    std::vector<uint32_t> symbol_ids(ITERATIONS);
    std::vector<double> prices(ITERATIONS);
    for (size_t i = 0; i < ITERATIONS; ++i) {
        symbol_ids[i] = sym_dist(rng);
        prices[i] = price_dist(rng);
    }

    // Warmup
    std::cout << "Warming up...\n";
    for (size_t i = 0; i < WARMUP; ++i) {
        old_app.on_quote(symbol_ids[i], prices[i]);
        new_app.on_quote(symbol_ids[i], prices[i]);
    }

    // Benchmark OLD (map + mutex)
    std::cout << "Benchmarking OLD (map + mutex)...\n";
    size_t idx = 0;
    double old_ns = measure_ns(
        [&]() {
            old_app.on_quote(symbol_ids[idx % ITERATIONS], prices[idx % ITERATIONS]);
            ++idx;
        },
        ITERATIONS);

    // Benchmark NEW (array, no mutex)
    std::cout << "Benchmarking NEW (array, no lock)...\n";
    idx = 0;
    double new_ns = measure_ns(
        [&]() {
            new_app.on_quote(symbol_ids[idx % ITERATIONS], prices[idx % ITERATIONS]);
            ++idx;
        },
        ITERATIONS);

    // Results
    std::cout << "\n";
    std::cout << "┌────────────────────────────────────────────────────┐\n";
    std::cout << "│                    RESULTS                         │\n";
    std::cout << "├────────────────────────────────────────────────────┤\n";
    std::cout << "│  OLD (map + mutex):    " << std::fixed << std::setprecision(1) << std::setw(8) << old_ns
              << " ns/op             │\n";
    std::cout << "│  NEW (array, no lock): " << std::setw(8) << new_ns << " ns/op             │\n";
    std::cout << "├────────────────────────────────────────────────────┤\n";

    double speedup = old_ns / new_ns;
    double saved_ns = old_ns - new_ns;

    std::cout << "│  Speedup:              " << std::setw(8) << std::setprecision(2) << speedup
              << "x                  │\n";
    std::cout << "│  Saved per tick:       " << std::setw(8) << std::setprecision(1) << saved_ns
              << " ns               │\n";
    std::cout << "├────────────────────────────────────────────────────┤\n";

    // Throughput
    double old_throughput = 1e9 / old_ns;
    double new_throughput = 1e9 / new_ns;

    std::cout << "│  OLD throughput:       " << std::setw(8) << std::setprecision(2) << old_throughput / 1e6
              << " M/sec            │\n";
    std::cout << "│  NEW throughput:       " << std::setw(8) << new_throughput / 1e6 << " M/sec            │\n";
    std::cout << "└────────────────────────────────────────────────────┘\n";

    // Extra: isolated benchmarks
    std::cout << "\nIsolated Benchmarks:\n";
    std::cout << "────────────────────\n";

    // Map lookup only
    std::map<uint32_t, int> test_map;
    for (uint32_t i = 0; i < 10; ++i)
        test_map[i] = i;

    idx = 0;
    double map_ns = measure_ns(
        [&]() {
            volatile auto it = test_map.find(symbol_ids[idx++ % ITERATIONS] % 10);
            (void)it;
        },
        ITERATIONS);

    // Array access only
    std::array<int, MAX_SYMBOLS> test_array{};
    for (size_t i = 0; i < 10; ++i)
        test_array[i] = i;

    idx = 0;
    double arr_ns = measure_ns(
        [&]() {
            volatile auto val = test_array[symbol_ids[idx++ % ITERATIONS] % 10];
            (void)val;
        },
        ITERATIONS);

    std::cout << "  map.find():      " << std::setw(6) << map_ns << " ns\n";
    std::cout << "  array[]:         " << std::setw(6) << arr_ns << " ns\n";
    std::cout << "  Difference:      " << std::setw(6) << (map_ns - arr_ns) << " ns\n\n";

    // Mutex overhead
    std::mutex mtx;
    double mutex_ns = measure_ns([&]() { std::lock_guard<std::mutex> lock(mtx); }, ITERATIONS);

    std::cout << "  mutex lock/unlock: " << std::setw(4) << mutex_ns << " ns\n";

    return 0;
}
