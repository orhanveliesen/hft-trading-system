#pragma once

#include "../types.hpp"
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdexcept>

namespace hft {
namespace config {

/**
 * Shared Memory Config
 *
 * Gerçek HFT sistemlerinde config değişiklikleri shared memory üzerinden yapılır.
 * Avantajları:
 *   - Zero latency: Değişiklik anında görünür (no syscall on read)
 *   - Lock-free: Atomic operations
 *   - Cross-process: Ops tool'u ayrı process olarak çalışabilir
 *
 * Memory layout:
 *   ┌─────────────────────────────────────────┐
 *   │  magic (8 bytes) - doğrulama           │
 *   │  version (4 bytes) - schema version    │
 *   │  ─────────────────────────────────────  │
 *   │  kill_switch (1 byte) - acil durdurma  │
 *   │  trading_enabled (1 byte)              │
 *   │  ─────────────────────────────────────  │
 *   │  max_position (8 bytes)                │
 *   │  order_size (4 bytes)                  │
 *   │  risk_limit (8 bytes)                  │
 *   │  ─────────────────────────────────────  │
 *   │  threshold_bps (4 bytes)               │
 *   │  lookback_ticks (4 bytes)              │
 *   │  ─────────────────────────────────────  │
 *   │  sequence (8 bytes) - config version   │
 *   └─────────────────────────────────────────┘
 */

struct alignas(64) SharedConfig {  // Cache line aligned
    // Header
    uint64_t magic;                          // 0x4846545F434F4E46 ("HFT_CONF")
    uint32_t version;                        // Schema version
    uint32_t _pad0;

    // Kill switches (en kritik, en üstte)
    std::atomic<bool> kill_switch;           // true = TÜM işlemleri durdur
    std::atomic<bool> trading_enabled;       // false = yeni pozisyon açma
    char _pad1[6];

    // Position limits
    std::atomic<int64_t> max_position;       // Maksimum net pozisyon
    std::atomic<Quantity> order_size;        // Her işlemde lot
    std::atomic<int64_t> max_daily_loss;     // Günlük max zarar (cent)

    // Strategy parameters
    std::atomic<uint32_t> threshold_bps;     // Sinyal eşiği (basis points)
    std::atomic<uint32_t> lookback_ticks;    // Geriye bakış penceresi
    std::atomic<uint32_t> cooldown_ms;       // İşlemler arası bekleme
    uint32_t _pad2;

    // Metadata
    std::atomic<uint64_t> sequence;          // Her değişiklikte artır
    std::atomic<uint64_t> last_update_ns;    // Son güncelleme timestamp

    // Defaults
    static constexpr uint64_t MAGIC = 0x4846545F434F4E46ULL;  // "HFT_CONF"
    static constexpr uint32_t VERSION = 1;

    void init_defaults() {
        magic = MAGIC;
        version = VERSION;
        kill_switch.store(false);
        trading_enabled.store(true);
        max_position.store(1000);
        order_size.store(100);
        max_daily_loss.store(100000);  // $1000
        threshold_bps.store(5);
        lookback_ticks.store(10);
        cooldown_ms.store(0);
        sequence.store(0);
        last_update_ns.store(0);
    }

    bool is_valid() const {
        return magic == MAGIC && version == VERSION;
    }
};

static_assert(sizeof(SharedConfig) <= 128, "SharedConfig should fit in 2 cache lines");

/**
 * SharedConfigManager
 *
 * Shared memory oluşturma/açma/kapatma işlemleri
 */
class SharedConfigManager {
public:
    static constexpr const char* DEFAULT_SHM_NAME = "/hft_config";

    // Yeni shared memory oluştur (server/ana uygulama)
    static SharedConfig* create(const char* name = DEFAULT_SHM_NAME) {
        // Varsa önce sil
        shm_unlink(name);

        int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
        if (fd < 0) {
            throw std::runtime_error("shm_open failed");
        }

        if (ftruncate(fd, sizeof(SharedConfig)) < 0) {
            ::close(fd);
            throw std::runtime_error("ftruncate failed");
        }

        void* ptr = mmap(nullptr, sizeof(SharedConfig),
                         PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);

        if (ptr == MAP_FAILED) {
            throw std::runtime_error("mmap failed");
        }

        auto* config = static_cast<SharedConfig*>(ptr);
        config->init_defaults();

        return config;
    }

    // Mevcut shared memory'e bağlan (client/control tool)
    static SharedConfig* open(const char* name = DEFAULT_SHM_NAME) {
        int fd = shm_open(name, O_RDWR, 0666);
        if (fd < 0) {
            return nullptr;  // Henüz oluşturulmamış
        }

        void* ptr = mmap(nullptr, sizeof(SharedConfig),
                         PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);

        if (ptr == MAP_FAILED) {
            return nullptr;
        }

        auto* config = static_cast<SharedConfig*>(ptr);
        if (!config->is_valid()) {
            munmap(ptr, sizeof(SharedConfig));
            return nullptr;
        }

        return config;
    }

    // Read-only erişim (monitoring için)
    static const SharedConfig* open_readonly(const char* name = DEFAULT_SHM_NAME) {
        int fd = shm_open(name, O_RDONLY, 0);
        if (fd < 0) {
            return nullptr;
        }

        void* ptr = mmap(nullptr, sizeof(SharedConfig),
                         PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);

        if (ptr == MAP_FAILED) {
            return nullptr;
        }

        auto* config = static_cast<const SharedConfig*>(ptr);
        if (!config->is_valid()) {
            munmap(const_cast<void*>(static_cast<const void*>(ptr)), sizeof(SharedConfig));
            return nullptr;
        }

        return config;
    }

    // Shared memory kapat
    static void close(SharedConfig* config) {
        if (config) {
            munmap(config, sizeof(SharedConfig));
        }
    }

    static void close(const SharedConfig* config) {
        if (config) {
            munmap(const_cast<void*>(static_cast<const void*>(config)), sizeof(SharedConfig));
        }
    }

    // Shared memory sil
    static void destroy(const char* name = DEFAULT_SHM_NAME) {
        shm_unlink(name);
    }
};

/**
 * RAII wrapper for SharedConfig
 */
class ScopedSharedConfig {
public:
    explicit ScopedSharedConfig(bool create = false,
                                const char* name = SharedConfigManager::DEFAULT_SHM_NAME)
        : config_(create ? SharedConfigManager::create(name)
                         : SharedConfigManager::open(name))
        , is_owner_(create)
        , name_(name)
    {}

    ~ScopedSharedConfig() {
        if (config_) {
            SharedConfigManager::close(config_);
            if (is_owner_) {
                SharedConfigManager::destroy(name_);
            }
        }
    }

    // Non-copyable
    ScopedSharedConfig(const ScopedSharedConfig&) = delete;
    ScopedSharedConfig& operator=(const ScopedSharedConfig&) = delete;

    // Movable
    ScopedSharedConfig(ScopedSharedConfig&& other) noexcept
        : config_(other.config_)
        , is_owner_(other.is_owner_)
        , name_(other.name_)
    {
        other.config_ = nullptr;
        other.is_owner_ = false;
    }

    SharedConfig* get() { return config_; }
    const SharedConfig* get() const { return config_; }
    SharedConfig* operator->() { return config_; }
    const SharedConfig* operator->() const { return config_; }
    explicit operator bool() const { return config_ != nullptr; }

private:
    SharedConfig* config_;
    bool is_owner_;
    const char* name_;
};

}  // namespace config
}  // namespace hft
