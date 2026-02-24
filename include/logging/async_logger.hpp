#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>

namespace hft {
namespace logging {

/**
 * Log Level
 */
enum class LogLevel : uint8_t { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Fatal = 5 };

inline const char* level_to_string(LogLevel level) {
    switch (level) {
    case LogLevel::Trace:
        return "TRACE";
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO ";
    case LogLevel::Warn:
        return "WARN ";
    case LogLevel::Error:
        return "ERROR";
    case LogLevel::Fatal:
        return "FATAL";
    default:
        return "?????";
    }
}

/**
 * Log Entry - Fixed size for predictable latency
 */
struct alignas(64) LogEntry {
    uint64_t timestamp_ns; // 8 bytes
    LogLevel level;        // 1 byte
    uint8_t category;      // 1 byte (user-defined)
    uint16_t reserved;     // 2 bytes padding
    uint32_t thread_id;    // 4 bytes
    char message[48];      // 48 bytes (null-terminated)
    // Total: 64 bytes (one cache line)

    void set_message(const char* msg) {
        size_t len = std::strlen(msg);
        if (len >= sizeof(message))
            len = sizeof(message) - 1;
        std::memcpy(message, msg, len);
        message[len] = '\0';
    }
};
static_assert(sizeof(LogEntry) == 64, "LogEntry must be 64 bytes");

/**
 * Lock-Free SPSC Ring Buffer
 *
 * Single Producer, Single Consumer - no locks needed.
 * Cache-line aligned to prevent false sharing.
 */
template <size_t Capacity = 16384> // 16K entries = 1MB buffer
class alignas(64) LogRingBuffer {
public:
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

    LogRingBuffer() : head_(0), tail_(0) { std::memset(buffer_.data(), 0, sizeof(buffer_)); }

    /**
     * Try to push a log entry (producer side)
     * Returns true if successful, false if buffer is full.
     * ~20-50ns on modern CPUs.
     */
    bool try_push(const LogEntry& entry) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next_head = (head + 1) & (Capacity - 1);

        // Check if buffer is full
        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false; // Buffer full
        }

        // Write entry
        buffer_[head] = entry;

        // Publish
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    /**
     * Try to pop a log entry (consumer side)
     * Returns true if entry was available, false if empty.
     */
    bool try_pop(LogEntry& entry) {
        size_t tail = tail_.load(std::memory_order_relaxed);

        // Check if buffer is empty
        if (tail == head_.load(std::memory_order_acquire)) {
            return false; // Buffer empty
        }

        // Read entry
        entry = buffer_[tail];

        // Consume
        tail_.store((tail + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }

    size_t size() const {
        size_t head = head_.load(std::memory_order_acquire);
        size_t tail = tail_.load(std::memory_order_acquire);
        return (head - tail + Capacity) & (Capacity - 1);
    }

    bool empty() const { return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire); }

    bool full() const { return size() == Capacity - 1; }

private:
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
    alignas(64) std::array<LogEntry, Capacity> buffer_;
};

/**
 * Async Logger
 *
 * Hot path (log call) is lock-free and very fast (~50ns).
 * Background thread handles actual I/O.
 *
 * Usage:
 *   AsyncLogger logger;
 *   logger.start();
 *   LOG_INFO(logger, "Order filled: %d @ %.2f", qty, price);
 *   logger.stop();
 */
class AsyncLogger {
public:
    using OutputCallback = std::function<void(const LogEntry&)>;

    AsyncLogger() : running_(false), min_level_(LogLevel::Info), dropped_count_(0), total_logged_(0) {}

    ~AsyncLogger() { stop(); }

    // Non-copyable
    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    /**
     * Start the background consumer thread
     */
    void start() {
        if (running_.exchange(true))
            return; // Already running

        consumer_thread_ = std::thread([this]() { consume_loop(); });
    }

    /**
     * Stop the logger and flush remaining entries
     */
    void stop() {
        if (!running_.exchange(false))
            return; // Already stopped

        if (consumer_thread_.joinable()) {
            consumer_thread_.join();
        }

        // Flush remaining entries
        LogEntry entry;
        while (buffer_.try_pop(entry)) {
            output_entry(entry);
        }
    }

    /**
     * Log a message (hot path - ~50ns)
     */
    void log(LogLevel level, uint8_t category, const char* message) {
        if (level < min_level_)
            return;

        LogEntry entry;
        entry.timestamp_ns = get_timestamp_ns();
        entry.level = level;
        entry.category = category;
        entry.thread_id = get_thread_id();
        entry.set_message(message);

        if (!buffer_.try_push(entry)) {
            dropped_count_.fetch_add(1, std::memory_order_relaxed);
        } else {
            total_logged_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    /**
     * Log with printf-style formatting
     * Note: Formatting happens on hot path, use sparingly
     */
    template <typename... Args>
    void logf(LogLevel level, uint8_t category, const char* fmt, Args... args) {
        if (level < min_level_)
            return;

        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), fmt, args...);
        log(level, category, buffer);
    }

    void set_min_level(LogLevel level) { min_level_ = level; }
    void set_output_callback(OutputCallback cb) { output_callback_ = std::move(cb); }

    // Statistics
    uint64_t dropped_count() const { return dropped_count_.load(); }
    uint64_t total_logged() const { return total_logged_.load(); }
    size_t pending_count() const { return buffer_.size(); }

private:
    LogRingBuffer<16384> buffer_;
    std::atomic<bool> running_;
    std::thread consumer_thread_;
    LogLevel min_level_;
    OutputCallback output_callback_;

    std::atomic<uint64_t> dropped_count_;
    std::atomic<uint64_t> total_logged_;

    void consume_loop() {
        LogEntry entry;
        while (running_.load(std::memory_order_relaxed)) {
            while (buffer_.try_pop(entry)) {
                output_entry(entry);
            }
            // Sleep briefly to avoid busy spinning
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    void output_entry(const LogEntry& entry) {
        if (output_callback_) {
            output_callback_(entry);
        } else {
            // Default: print to stderr
            auto ts_ms = entry.timestamp_ns / 1000000;
            std::fprintf(stderr, "[%lu.%03lu] [%s] [cat:%d] %s\n", ts_ms / 1000, ts_ms % 1000,
                         level_to_string(entry.level), entry.category, entry.message);
        }
    }

    static uint64_t get_timestamp_ns() {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    }

    static uint32_t get_thread_id() {
        static thread_local uint32_t id = 0;
        if (id == 0) {
            std::hash<std::thread::id> hasher;
            id = static_cast<uint32_t>(hasher(std::this_thread::get_id()));
        }
        return id;
    }
};

// Category constants for trading system
namespace LogCategory {
constexpr uint8_t System = 0;
constexpr uint8_t Market = 1;
constexpr uint8_t Order = 2;
constexpr uint8_t Strategy = 3;
constexpr uint8_t Risk = 4;
constexpr uint8_t Position = 5;
constexpr uint8_t Latency = 6;
} // namespace LogCategory

// Convenience macros
#define LOG_TRACE(logger, msg) logger.log(hft::logging::LogLevel::Trace, 0, msg)
#define LOG_DEBUG(logger, msg) logger.log(hft::logging::LogLevel::Debug, 0, msg)
#define LOG_INFO(logger, msg) logger.log(hft::logging::LogLevel::Info, 0, msg)
#define LOG_WARN(logger, msg) logger.log(hft::logging::LogLevel::Warn, 0, msg)
#define LOG_ERROR(logger, msg) logger.log(hft::logging::LogLevel::Error, 0, msg)

#define LOG_CATEGORY(logger, level, cat, msg) logger.log(level, hft::logging::LogCategory::cat, msg)

// Printf-style variants
#define LOGF_INFO(logger, fmt, ...) logger.logf(hft::logging::LogLevel::Info, 0, fmt, ##__VA_ARGS__)
#define LOGF_WARN(logger, fmt, ...) logger.logf(hft::logging::LogLevel::Warn, 0, fmt, ##__VA_ARGS__)

} // namespace logging
} // namespace hft
