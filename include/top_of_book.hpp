#pragma once

#include "types.hpp"
#include <array>
#include <algorithm>
#include <cstring>

namespace hft {

// Book state for snapshot handling
enum class BookState : uint8_t {
    Empty,      // No data
    Building,   // Receiving snapshot
    Ready       // Ready for trading
};

// L1 Snapshot - just BBO
struct L1Snapshot {
    Price bid_price = 0;
    Quantity bid_size = 0;
    Price ask_price = 0;
    Quantity ask_size = 0;
    uint64_t sequence = 0;
};

// L2 Snapshot - top N levels
template<size_t N = 10>
struct L2Snapshot {
    struct Level {
        Price price = 0;
        Quantity size = 0;
    };

    std::array<Level, N> bids{};
    std::array<Level, N> asks{};
    uint64_t sequence = 0;
    uint8_t bid_count = 0;  // Actual number of bid levels
    uint8_t ask_count = 0;  // Actual number of ask levels
};

/**
 * TopOfBook - Lightweight order book for aggressive trading
 *
 * Use cases:
 * - Signal generation (momentum, mean reversion)
 * - Aggressive order execution (market taking)
 * - Low-latency environments where cache efficiency matters
 *
 * NOT for:
 * - Market making (need to track own orders)
 * - Exchange matching engines (need full order tracking)
 *
 * Memory: ~88 bytes per symbol (fits in L1 cache line)
 * Access: O(1) for BBO, O(DEPTH) for level updates
 */
class TopOfBook {
public:
    static constexpr size_t DEPTH = 5;

    struct Level {
        Price price = 0;
        Quantity size = 0;

        bool empty() const { return size == 0; }
        void clear() { price = 0; size = 0; }
    };

    TopOfBook() = default;

    // === BBO Access (hot path - inlined) ===

    __attribute__((always_inline))
    Price best_bid() const {
        return bids_[0].price;
    }

    __attribute__((always_inline))
    Price best_ask() const {
        return asks_[0].price;
    }

    __attribute__((always_inline))
    Quantity best_bid_size() const {
        return bids_[0].size;
    }

    __attribute__((always_inline))
    Quantity best_ask_size() const {
        return asks_[0].size;
    }

    __attribute__((always_inline))
    Price mid_price() const {
        if (bids_[0].price == 0 || asks_[0].price == 0) return 0;
        return (bids_[0].price + asks_[0].price) / 2;
    }

    __attribute__((always_inline))
    Price spread() const {
        if (bids_[0].price == 0 || asks_[0].price == 0) return INVALID_PRICE;
        return asks_[0].price - bids_[0].price;
    }

    // === Level Access ===

    const Level& bid(size_t level) const { return bids_[level]; }
    const Level& ask(size_t level) const { return asks_[level]; }

    size_t bid_levels() const {
        size_t count = 0;
        for (const auto& lvl : bids_) {
            if (lvl.empty()) break;
            ++count;
        }
        return count;
    }

    size_t ask_levels() const {
        size_t count = 0;
        for (const auto& lvl : asks_) {
            if (lvl.empty()) break;
            ++count;
        }
        return count;
    }

    // Total depth on bid side (sum of all quantities)
    Quantity total_bid_depth() const {
        Quantity total = 0;
        for (const auto& lvl : bids_) {
            total += lvl.size;
        }
        return total;
    }

    // Total depth on ask side
    Quantity total_ask_depth() const {
        Quantity total = 0;
        for (const auto& lvl : asks_) {
            total += lvl.size;
        }
        return total;
    }

    // Book imbalance: (bid_depth - ask_depth) / (bid_depth + ask_depth)
    // Returns value in range [-1.0, 1.0], positive = more bids
    double imbalance() const {
        double bid_depth = static_cast<double>(total_bid_depth());
        double ask_depth = static_cast<double>(total_ask_depth());
        double total = bid_depth + ask_depth;
        if (total == 0) return 0.0;
        return (bid_depth - ask_depth) / total;
    }

    // === Updates from Market Data Feed ===

    // Set a price level (from ITCH Add/Execute/Cancel aggregated updates)
    void set_level(Side side, Price price, Quantity size) {
        if (side == Side::Buy) {
            set_bid_level(price, size);
        } else {
            set_ask_level(price, size);
        }
        last_update_++;
    }

    // Clear the entire book
    void clear() {
        for (auto& lvl : bids_) lvl.clear();
        for (auto& lvl : asks_) lvl.clear();
        last_update_ = 0;
        sequence_ = 0;
        state_ = BookState::Empty;
    }

    // Timestamp of last update
    Timestamp last_update() const { return last_update_; }

    // Sequence number for snapshot sync
    uint64_t sequence() const { return sequence_; }
    void set_sequence(uint64_t seq) { sequence_ = seq; }

    // Book state
    BookState state() const { return state_; }
    bool is_ready() const { return state_ == BookState::Ready; }
    void set_state(BookState state) { state_ = state; }

    // === Snapshot Handling ===

    // Apply L1 snapshot (just BBO)
    void apply_snapshot(const L1Snapshot& snap) {
        clear();
        if (snap.bid_price > 0) {
            bids_[0] = {snap.bid_price, snap.bid_size};
        }
        if (snap.ask_price > 0) {
            asks_[0] = {snap.ask_price, snap.ask_size};
        }
        sequence_ = snap.sequence;
        state_ = BookState::Ready;
        last_update_++;
    }

    // Apply L2 snapshot (multiple levels)
    template<size_t N>
    void apply_snapshot(const L2Snapshot<N>& snap) {
        clear();

        // Copy bid levels (up to DEPTH)
        size_t bid_copy = std::min(static_cast<size_t>(snap.bid_count), DEPTH);
        for (size_t i = 0; i < bid_copy; ++i) {
            bids_[i] = {snap.bids[i].price, snap.bids[i].size};
        }

        // Copy ask levels (up to DEPTH)
        size_t ask_copy = std::min(static_cast<size_t>(snap.ask_count), DEPTH);
        for (size_t i = 0; i < ask_copy; ++i) {
            asks_[i] = {snap.asks[i].price, snap.asks[i].size};
        }

        sequence_ = snap.sequence;
        state_ = BookState::Ready;
        last_update_++;
    }

    // Extract current state as L1 snapshot
    L1Snapshot to_l1_snapshot() const {
        L1Snapshot snap;
        snap.bid_price = bids_[0].price;
        snap.bid_size = bids_[0].size;
        snap.ask_price = asks_[0].price;
        snap.ask_size = asks_[0].size;
        snap.sequence = sequence_;
        return snap;
    }

    // Extract current state as L2 snapshot
    L2Snapshot<DEPTH> to_l2_snapshot() const {
        L2Snapshot<DEPTH> snap;
        for (size_t i = 0; i < DEPTH; ++i) {
            if (!bids_[i].empty()) {
                snap.bids[i] = {bids_[i].price, bids_[i].size};
                snap.bid_count++;
            }
            if (!asks_[i].empty()) {
                snap.asks[i] = {asks_[i].price, asks_[i].size};
                snap.ask_count++;
            }
        }
        snap.sequence = sequence_;
        return snap;
    }

private:
    // Bids: sorted descending (best = highest price at index 0)
    void set_bid_level(Price price, Quantity size) {
        if (size == 0) {
            remove_bid_level(price);
            return;
        }

        // Find position for this price
        for (size_t i = 0; i < DEPTH; ++i) {
            if (bids_[i].price == price) {
                // Update existing level
                bids_[i].size = size;
                return;
            }
            if (bids_[i].price < price || bids_[i].empty()) {
                // Insert here, shift others down
                for (size_t j = DEPTH - 1; j > i; --j) {
                    bids_[j] = bids_[j - 1];
                }
                bids_[i] = {price, size};
                return;
            }
        }
        // Price is worse than all tracked levels - ignore
    }

    void remove_bid_level(Price price) {
        for (size_t i = 0; i < DEPTH; ++i) {
            if (bids_[i].price == price) {
                // Shift up
                for (size_t j = i; j < DEPTH - 1; ++j) {
                    bids_[j] = bids_[j + 1];
                }
                bids_[DEPTH - 1].clear();
                return;
            }
        }
    }

    // Asks: sorted ascending (best = lowest price at index 0)
    void set_ask_level(Price price, Quantity size) {
        if (size == 0) {
            remove_ask_level(price);
            return;
        }

        // Find position for this price
        for (size_t i = 0; i < DEPTH; ++i) {
            if (asks_[i].price == price) {
                // Update existing level
                asks_[i].size = size;
                return;
            }
            if (asks_[i].price > price || asks_[i].empty()) {
                // Insert here, shift others down
                for (size_t j = DEPTH - 1; j > i; --j) {
                    asks_[j] = asks_[j - 1];
                }
                asks_[i] = {price, size};
                return;
            }
        }
        // Price is worse than all tracked levels - ignore
    }

    void remove_ask_level(Price price) {
        for (size_t i = 0; i < DEPTH; ++i) {
            if (asks_[i].price == price) {
                // Shift up
                for (size_t j = i; j < DEPTH - 1; ++j) {
                    asks_[j] = asks_[j + 1];
                }
                asks_[DEPTH - 1].clear();
                return;
            }
        }
    }

    std::array<Level, DEPTH> bids_{};   // 40 bytes
    std::array<Level, DEPTH> asks_{};   // 40 bytes
    Timestamp last_update_ = 0;          // 8 bytes
    uint64_t sequence_ = 0;              // 8 bytes
    BookState state_ = BookState::Empty; // 1 byte
    // Total: ~97 bytes - fits in 2 cache lines
};

// Compile-time size verification
static_assert(sizeof(TopOfBook::Level) == 8, "Level should be 8 bytes");
static_assert(sizeof(TopOfBook) <= 128, "TopOfBook should fit in 2 cache lines");

}  // namespace hft
