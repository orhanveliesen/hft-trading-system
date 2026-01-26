#pragma once

/**
 * SharedPaperConfig - Paper trading simulation settings
 *
 * Extracted from SharedConfig to follow Single Responsibility Principle.
 * Contains ONLY paper-trading specific settings.
 *
 * Usage:
 *   Trader (creator):
 *     auto paper_cfg = SharedPaperConfig::create("/trader_paper_config");
 *     paper_exchange.set_paper_config(paper_cfg);
 *
 *   Dashboard (modifier):
 *     auto paper_cfg = SharedPaperConfig::open_rw("/trader_paper_config");
 *     paper_cfg->set_slippage_bps(10.0);  // 10 bps
 */

#include <atomic>
#include <cstdint>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

namespace hft {
namespace ipc {

struct SharedPaperConfig {
    static constexpr uint64_t MAGIC = 0x50415045524346ULL;  // "PAPERCF\0"
    static constexpr uint32_t VERSION = 1;

    // Header
    uint64_t magic;
    uint32_t version;
    std::atomic<uint32_t> sequence;  // Incremented on each change

    // Slippage simulation
    // Simulates market impact and execution slippage
    // Applied adversely: BUY pays more, SELL receives less
    std::atomic<int32_t> slippage_bps_x100;  // Default: 500 (5 bps = 0.05%)

    // Simulated exchange latency (future use)
    // Adds artificial delay to order fills to simulate network latency
    std::atomic<int64_t> simulated_latency_ns;  // Default: 0 (no latency)

    // Fill probability (future use)
    // Simulates partial fills and order rejections
    // 10000 = 100% fill rate, 9000 = 90% fill rate
    std::atomic<int32_t> fill_probability_x10000;  // Default: 10000 (100%)

    // Padding for cache alignment and future expansion
    std::atomic<int64_t> reserved1;
    std::atomic<int64_t> reserved2;

    // === Accessors ===
    double slippage_bps() const {
        return slippage_bps_x100.load(std::memory_order_relaxed) / 100.0;
    }

    int64_t get_simulated_latency_ns() const {
        return simulated_latency_ns.load(std::memory_order_relaxed);
    }

    double fill_probability() const {
        return fill_probability_x10000.load(std::memory_order_relaxed) / 10000.0;
    }

    // === Mutators ===
    void set_slippage_bps(double val) {
        slippage_bps_x100.store(static_cast<int32_t>(val * 100), std::memory_order_relaxed);
        sequence.fetch_add(1, std::memory_order_relaxed);
    }

    void set_simulated_latency_ns(int64_t val) {
        simulated_latency_ns.store(val, std::memory_order_relaxed);
        sequence.fetch_add(1, std::memory_order_relaxed);
    }

    void set_fill_probability(double val) {
        fill_probability_x10000.store(static_cast<int32_t>(val * 10000), std::memory_order_relaxed);
        sequence.fetch_add(1, std::memory_order_relaxed);
    }

    // === Initialization ===
    void init() {
        magic = MAGIC;
        version = VERSION;
        sequence.store(0, std::memory_order_relaxed);

        // Defaults
        slippage_bps_x100.store(500, std::memory_order_relaxed);        // 5 bps (realistic)
        simulated_latency_ns.store(0, std::memory_order_relaxed);       // No artificial latency
        fill_probability_x10000.store(10000, std::memory_order_relaxed); // 100% fill rate
        reserved1.store(0, std::memory_order_relaxed);
        reserved2.store(0, std::memory_order_relaxed);
    }

    bool is_valid() const {
        return magic == MAGIC && version == VERSION;
    }

    // === Shared Memory Factory ===
    static SharedPaperConfig* create(const char* name) {
        int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
        if (fd < 0) return nullptr;

        if (ftruncate(fd, sizeof(SharedPaperConfig)) < 0) {
            close(fd);
            return nullptr;
        }

        void* ptr = mmap(nullptr, sizeof(SharedPaperConfig),
                         PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);

        if (ptr == MAP_FAILED) return nullptr;

        auto* cfg = static_cast<SharedPaperConfig*>(ptr);
        cfg->init();
        return cfg;
    }

    static SharedPaperConfig* open(const char* name) {
        int fd = shm_open(name, O_RDONLY, 0666);
        if (fd < 0) return nullptr;

        void* ptr = mmap(nullptr, sizeof(SharedPaperConfig),
                         PROT_READ, MAP_SHARED, fd, 0);
        close(fd);

        if (ptr == MAP_FAILED) return nullptr;

        auto* cfg = static_cast<SharedPaperConfig*>(ptr);
        if (!cfg->is_valid()) {
            munmap(ptr, sizeof(SharedPaperConfig));
            return nullptr;
        }
        return cfg;
    }

    static SharedPaperConfig* open_rw(const char* name) {
        int fd = shm_open(name, O_RDWR, 0666);
        if (fd < 0) return nullptr;

        void* ptr = mmap(nullptr, sizeof(SharedPaperConfig),
                         PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);

        if (ptr == MAP_FAILED) return nullptr;

        auto* cfg = static_cast<SharedPaperConfig*>(ptr);
        if (!cfg->is_valid()) {
            munmap(ptr, sizeof(SharedPaperConfig));
            return nullptr;
        }
        return cfg;
    }

    static void destroy(const char* name) {
        shm_unlink(name);
    }
};

}  // namespace ipc
}  // namespace hft
