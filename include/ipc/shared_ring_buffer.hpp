#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hft {
namespace ipc {

/**
 * SharedRingBuffer - Lock-free SPSC queue in shared memory
 *
 * Design:
 * - Single Producer Single Consumer (SPSC) - no locks needed
 * - Shared memory for inter-process communication
 * - Cache-line aligned head/tail to prevent false sharing
 * - Power-of-2 size for fast modulo (bitwise AND)
 *
 * Memory Layout:
 * [Header: 128 bytes] [Data: N * sizeof(T) bytes]
 *   - head (64 bytes, cache-line aligned)
 *   - tail (64 bytes, cache-line aligned)
 *   - data[N]
 *
 * Usage:
 *   Producer (HFT engine):
 *     SharedRingBuffer<TradeEvent> buf("/hft_events", SIZE, true);
 *     buf.push(event);  // ~5ns
 *
 *   Consumer (Observer):
 *     SharedRingBuffer<TradeEvent> buf("/hft_events", SIZE, false);
 *     TradeEvent e;
 *     if (buf.pop(e)) { process(e); }
 *
 * @tparam T Element type (must be POD, preferably cache-aligned)
 * @tparam N Buffer size (must be power of 2)
 */
template <typename T, size_t N = 65536>
class SharedRingBuffer {
    static_assert((N & (N - 1)) == 0, "N must be power of 2");
    static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");

public:
    static constexpr size_t CAPACITY = N;
    static constexpr const char* DEFAULT_NAME = "/hft_events";

    /**
     * Header stored at the beginning of shared memory
     * Cache-line aligned to prevent false sharing between head and tail
     */
    struct alignas(64) Header {
        alignas(64) std::atomic<uint64_t> head{0}; // Producer writes here
        alignas(64) std::atomic<uint64_t> tail{0}; // Consumer writes here
        uint64_t capacity{N};
        uint64_t element_size{sizeof(T)};
        uint64_t magic{0x4846544F425356}; // "HFTOBSV" in hex
        char padding[64 - 40];            // Pad to 64 bytes
    };

    static_assert(sizeof(Header) == 128, "Header should be 128 bytes (2 cache lines)");

private:
    const char* name_;
    int fd_ = -1;
    void* mapped_ = nullptr;
    size_t mapped_size_ = 0;
    bool is_producer_ = false;

    Header* header_ = nullptr;
    T* data_ = nullptr;

public:
    /**
     * Create or open shared ring buffer
     *
     * @param name Shared memory name (e.g., "/hft_events")
     * @param create If true, create new (producer). If false, open existing (consumer).
     */
    explicit SharedRingBuffer(const char* name = DEFAULT_NAME, bool create = false)
        : name_(name), is_producer_(create) {
        mapped_size_ = sizeof(Header) + N * sizeof(T);

        if (create) {
            // Producer: create and initialize
            shm_unlink(name_); // Remove if exists
            fd_ = shm_open(name_, O_CREAT | O_RDWR, 0666);
            if (fd_ < 0) {
                throw std::runtime_error("shm_open failed (create)");
            }

            if (ftruncate(fd_, mapped_size_) < 0) {
                close(fd_);
                shm_unlink(name_);
                throw std::runtime_error("ftruncate failed");
            }

            mapped_ = mmap(nullptr, mapped_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
            if (mapped_ == MAP_FAILED) {
                close(fd_);
                shm_unlink(name_);
                throw std::runtime_error("mmap failed");
            }

            // Initialize header
            header_ = new (mapped_) Header();
            header_->head.store(0, std::memory_order_relaxed);
            header_->tail.store(0, std::memory_order_relaxed);
            header_->capacity = N;
            header_->element_size = sizeof(T);
            header_->magic = 0x4846544F425356;

            data_ = reinterpret_cast<T*>(static_cast<char*>(mapped_) + sizeof(Header));

            // Zero-initialize data
            std::memset(data_, 0, N * sizeof(T));

        } else {
            // Consumer: open existing
            fd_ = shm_open(name_, O_RDWR, 0666);
            if (fd_ < 0) {
                throw std::runtime_error("shm_open failed (open) - is producer running?");
            }

            mapped_ = mmap(nullptr, mapped_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
            if (mapped_ == MAP_FAILED) {
                close(fd_);
                throw std::runtime_error("mmap failed");
            }

            header_ = static_cast<Header*>(mapped_);
            data_ = reinterpret_cast<T*>(static_cast<char*>(mapped_) + sizeof(Header));

            // Validate
            if (header_->magic != 0x4846544F425356) {
                munmap(mapped_, mapped_size_);
                close(fd_);
                throw std::runtime_error("Invalid shared memory (magic mismatch)");
            }
        }
    }

    ~SharedRingBuffer() {
        if (mapped_ && mapped_ != MAP_FAILED) {
            munmap(mapped_, mapped_size_);
        }
        if (fd_ >= 0) {
            close(fd_);
        }
        if (is_producer_) {
            shm_unlink(name_);
        }
    }

    // Non-copyable
    SharedRingBuffer(const SharedRingBuffer&) = delete;
    SharedRingBuffer& operator=(const SharedRingBuffer&) = delete;

    /**
     * Push element to buffer (producer only)
     * Lock-free, wait-free, ~5-10ns
     *
     * @param item Item to push
     * @return true if successful, false if buffer full
     */
    bool push(const T& item) {
        const uint64_t head = header_->head.load(std::memory_order_relaxed);
        const uint64_t tail = header_->tail.load(std::memory_order_acquire);

        // Check if full
        if (head - tail >= N) {
            return false; // Buffer full, drop event
        }

        // Write data
        const size_t idx = head & (N - 1); // Fast modulo
        data_[idx] = item;

        // Publish (release ensures data write is visible before head update)
        header_->head.store(head + 1, std::memory_order_release);

        return true;
    }

    /**
     * Pop element from buffer (consumer only)
     * Lock-free, wait-free
     *
     * @param item Output item
     * @return true if item was available, false if buffer empty
     */
    bool pop(T& item) {
        const uint64_t tail = header_->tail.load(std::memory_order_relaxed);
        const uint64_t head = header_->head.load(std::memory_order_acquire);

        // Check if empty
        if (tail >= head) {
            return false; // Buffer empty
        }

        // Read data
        const size_t idx = tail & (N - 1); // Fast modulo
        item = data_[idx];

        // Advance tail (release ensures read is complete before tail update)
        header_->tail.store(tail + 1, std::memory_order_release);

        return true;
    }

    /**
     * Peek at next element without removing (consumer only)
     */
    bool peek(T& item) const {
        const uint64_t tail = header_->tail.load(std::memory_order_relaxed);
        const uint64_t head = header_->head.load(std::memory_order_acquire);

        if (tail >= head) {
            return false;
        }

        const size_t idx = tail & (N - 1);
        item = data_[idx];
        return true;
    }

    // Stats
    size_t size() const {
        const uint64_t head = header_->head.load(std::memory_order_acquire);
        const uint64_t tail = header_->tail.load(std::memory_order_acquire);
        return static_cast<size_t>(head - tail);
    }

    bool empty() const { return size() == 0; }
    bool full() const { return size() >= N; }
    size_t capacity() const { return N; }

    uint64_t total_produced() const { return header_->head.load(std::memory_order_acquire); }

    uint64_t total_consumed() const { return header_->tail.load(std::memory_order_acquire); }

    uint64_t dropped() const {
        // Can't track directly without additional counter
        return 0;
    }
};

} // namespace ipc
} // namespace hft
