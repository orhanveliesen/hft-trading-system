#pragma once

/**
 * TradingStateSHM - Shared Memory Factory for TradingState
 *
 * Provides shared memory creation, opening, and lifecycle management
 * for TradingState structures. Designed for IPC between:
 * - trader (writer)
 * - trader_dashboard (reader)
 * - trader_tuner (reader/writer for signals)
 * - trader_observer (reader)
 *
 * Usage:
 *   Writer (trader):
 *     TradingState* state = TradingStateSHM::create("/hft_trading_state");
 *     state->positions.quantity[0] = 0.5;
 *     // ... use state ...
 *     TradingStateSHM::close(state);
 *     TradingStateSHM::destroy("/hft_trading_state");  // Only owner calls this
 *
 *   Reader (dashboard):
 *     TradingState* state = TradingStateSHM::open("/hft_trading_state");
 *     double qty = state->positions.quantity[0];
 *     TradingStateSHM::close(state);
 *
 *   RAII Wrapper:
 *     ScopedTradingState owner(true, "/hft_trading_state", 100000.0);
 *     owner->positions.quantity[0] = 0.5;
 *     // Automatically cleaned up when scope exits
 */

#include "trading_state.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hft {
namespace trading {

/**
 * TradingStateSHM - Factory for shared memory TradingState
 */
class TradingStateSHM {
public:
    /**
     * Create new shared memory segment with initialized TradingState.
     *
     * @param name Shared memory name (e.g., "/hft_trading_state")
     * @param starting_cash Initial cash amount
     * @return Pointer to TradingState, or nullptr on failure
     */
    static TradingState* create(const char* name, double starting_cash = 100000.0) {
        int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
        if (fd < 0) {
            return nullptr;
        }

        if (ftruncate(fd, sizeof(TradingState)) < 0) {
            ::close(fd);
            return nullptr;
        }

        void* ptr = mmap(nullptr, sizeof(TradingState), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);

        if (ptr == MAP_FAILED) {
            return nullptr;
        }

        auto* state = static_cast<TradingState*>(ptr);
        state->init(starting_cash);
        return state;
    }

    /**
     * Open existing shared memory for read-write access.
     *
     * @param name Shared memory name
     * @return Pointer to TradingState, or nullptr if not found/invalid
     */
    static TradingState* open(const char* name) {
        int fd = shm_open(name, O_RDWR, 0666);
        if (fd < 0) {
            return nullptr;
        }

        void* ptr = mmap(nullptr, sizeof(TradingState), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);

        if (ptr == MAP_FAILED) {
            return nullptr;
        }

        auto* state = static_cast<TradingState*>(ptr);
        if (!state->is_valid()) {
            munmap(ptr, sizeof(TradingState));
            return nullptr;
        }
        return state;
    }

    /**
     * Open existing shared memory for read-only access.
     *
     * @param name Shared memory name
     * @return Const pointer to TradingState, or nullptr if not found/invalid
     */
    static const TradingState* open_readonly(const char* name) {
        int fd = shm_open(name, O_RDONLY, 0666);
        if (fd < 0) {
            return nullptr;
        }

        void* ptr = mmap(nullptr, sizeof(TradingState), PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);

        if (ptr == MAP_FAILED) {
            return nullptr;
        }

        auto* state = static_cast<const TradingState*>(ptr);
        if (!state->is_valid()) {
            munmap(const_cast<void*>(static_cast<const void*>(ptr)), sizeof(TradingState));
            return nullptr;
        }
        return state;
    }

    /**
     * Close (unmap) shared memory from current process.
     * Does not destroy the shared memory - other processes can still access it.
     */
    static void close(const TradingState* state) {
        if (state) {
            munmap(const_cast<TradingState*>(state), sizeof(TradingState));
        }
    }

    /**
     * Destroy shared memory segment.
     * Only the owner (creator) should call this.
     */
    static void destroy(const char* name) { shm_unlink(name); }
};

/**
 * ScopedTradingState - RAII wrapper for TradingState shared memory.
 *
 * Automatically handles cleanup when scope exits.
 */
class ScopedTradingState {
public:
    /**
     * Create or open shared memory.
     *
     * @param is_owner If true, creates new shm and destroys on destruction
     * @param name Shared memory name
     * @param starting_cash Initial cash (only used if is_owner)
     */
    ScopedTradingState(bool is_owner, const char* name, double starting_cash = 100000.0)
        : name_(name), state_(nullptr), is_owner_(is_owner) {
        if (is_owner) {
            state_ = TradingStateSHM::create(name, starting_cash);
        } else {
            state_ = TradingStateSHM::open(name);
        }
    }

    ~ScopedTradingState() {
        if (state_) {
            TradingStateSHM::close(state_);
        }
        if (is_owner_ && !name_.empty()) {
            TradingStateSHM::destroy(name_.c_str());
        }
    }

    // Non-copyable
    ScopedTradingState(const ScopedTradingState&) = delete;
    ScopedTradingState& operator=(const ScopedTradingState&) = delete;

    // Movable
    ScopedTradingState(ScopedTradingState&& other) noexcept
        : name_(std::move(other.name_)), state_(other.state_), is_owner_(other.is_owner_) {
        other.state_ = nullptr;
        other.is_owner_ = false;
    }

    ScopedTradingState& operator=(ScopedTradingState&& other) noexcept {
        if (this != &other) {
            if (state_) {
                TradingStateSHM::close(state_);
            }
            if (is_owner_ && !name_.empty()) {
                TradingStateSHM::destroy(name_.c_str());
            }

            name_ = std::move(other.name_);
            state_ = other.state_;
            is_owner_ = other.is_owner_;

            other.state_ = nullptr;
            other.is_owner_ = false;
        }
        return *this;
    }

    // Access operators
    TradingState* operator->() { return state_; }
    const TradingState* operator->() const { return state_; }

    TradingState& operator*() { return *state_; }
    const TradingState& operator*() const { return *state_; }

    TradingState* get() { return state_; }
    const TradingState* get() const { return state_; }

    explicit operator bool() const { return state_ != nullptr; }

private:
    std::string name_;
    TradingState* state_;
    bool is_owner_;
};

} // namespace trading
} // namespace hft
