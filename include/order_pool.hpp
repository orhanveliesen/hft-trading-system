#pragma once

#include "types.hpp"
#include "strategy/halt_manager.hpp"
#include <array>
#include <memory>
#include <atomic>
#include <functional>

namespace hft {

/**
 * OrderPool - Pre-allocated order pool with monitoring
 *
 * Features:
 * - Pre-allocated pool (zero runtime allocation)
 * - Free list management
 * - Pool level monitoring with thresholds
 * - Halt integration when critical
 */
class OrderPool {
public:
    static constexpr size_t DEFAULT_POOL_SIZE = 1'000'000;
    static constexpr size_t WARNING_THRESHOLD_PERCENT = 10;   // 10% remaining
    static constexpr size_t CRITICAL_THRESHOLD_PERCENT = 1;   // 1% remaining

    using WarningCallback = std::function<void(size_t remaining, size_t total)>;

    explicit OrderPool(size_t pool_size = DEFAULT_POOL_SIZE)
        : pool_size_(pool_size)
        , free_count_(pool_size)
        , warning_threshold_(pool_size * WARNING_THRESHOLD_PERCENT / 100)
        , critical_threshold_(pool_size * CRITICAL_THRESHOLD_PERCENT / 100)
        , halt_manager_(nullptr)
    {
        pool_ = std::make_unique<Order[]>(pool_size);

        // Build free list
        for (size_t i = 0; i < pool_size - 1; ++i) {
            pool_[i].next = &pool_[i + 1];
        }
        pool_[pool_size - 1].next = nullptr;

        free_list_ = &pool_[0];
    }

    // Set halt manager for critical situations
    void set_halt_manager(strategy::HaltManager* manager) {
        halt_manager_ = manager;
    }

    // Set warning callback
    void set_warning_callback(WarningCallback cb) {
        warning_callback_ = std::move(cb);
    }

    // Allocate an order from the pool
    __attribute__((always_inline))
    Order* allocate() {
        if (!free_list_) {
            // FATAL: Pool exhausted
            if (halt_manager_) {
                halt_manager_->halt(
                    strategy::HaltReason::PoolExhausted,
                    "Order pool exhausted - no orders available"
                );
            }
            return nullptr;
        }

        Order* order = free_list_;
        free_list_ = free_list_->next;

        size_t remaining = free_count_.fetch_sub(1, std::memory_order_relaxed) - 1;

        // Check thresholds
        if (remaining == critical_threshold_) {
            if (halt_manager_) {
                halt_manager_->halt(
                    strategy::HaltReason::PoolCritical,
                    "Order pool critically low - initiating halt"
                );
            }
        } else if (remaining == warning_threshold_) {
            if (warning_callback_) {
                warning_callback_(remaining, pool_size_);
            }
        }

        order->reset();
        return order;
    }

    // Return an order to the pool
    __attribute__((always_inline))
    void deallocate(Order* order) {
        if (!order) return;

        order->next = free_list_;
        free_list_ = order;
        free_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // Pool statistics
    size_t free_count() const {
        return free_count_.load(std::memory_order_relaxed);
    }

    size_t used_count() const {
        return pool_size_ - free_count();
    }

    size_t pool_size() const {
        return pool_size_;
    }

    double utilization() const {
        return static_cast<double>(used_count()) / pool_size_ * 100.0;
    }

    bool is_critical() const {
        return free_count() <= critical_threshold_;
    }

    bool is_warning() const {
        return free_count() <= warning_threshold_;
    }

private:
    std::unique_ptr<Order[]> pool_;
    Order* free_list_;

    size_t pool_size_;
    std::atomic<size_t> free_count_;

    size_t warning_threshold_;
    size_t critical_threshold_;

    strategy::HaltManager* halt_manager_;
    WarningCallback warning_callback_;
};

}  // namespace hft
