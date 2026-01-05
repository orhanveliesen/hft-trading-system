#pragma once

#include "types.hpp"
#include <memory>
#include <cstring>

namespace hft {

// Compare functors - zero overhead, fully inlined
struct BidCompare {
    bool operator()(Price a, Price b) const { return a > b; }
};

struct AskCompare {
    bool operator()(Price a, Price b) const { return a < b; }
};

/**
 * BookSide - Manages one side of the order book (bid or ask)
 *
 * Responsibilities:
 * - Price level lookup (O(1) via array)
 * - Sorted level list maintenance
 * - Best price tracking
 *
 * NOT responsible for:
 * - Memory allocation (parent provides pre-allocated levels)
 * - Pool management
 */
template<typename Compare>
class BookSide {
public:
    BookSide(Price base_price, size_t price_range)
        : base_price_(base_price)
        , price_range_(price_range)
        , levels_(new PriceLevel*[price_range])
        , best_level_(nullptr)
    {
        std::memset(levels_.get(), 0, price_range * sizeof(PriceLevel*));
    }

    // === Queries ===

    PriceLevel* find_level(Price price) const {
        if (!is_in_range(price)) {
            return nullptr;
        }
        return levels_[price - base_price_];
    }

    Quantity quantity_at(Price price) const {
        PriceLevel* level = find_level(price);
        return level ? level->total_quantity : 0;
    }

    Price best_price() const {
        return best_level_ ? best_level_->price : INVALID_PRICE;
    }

    // === Level Management ===

    // Insert a pre-allocated and initialized level into the book
    // Caller must set level->price before calling
    void insert_level(PriceLevel* level) {
        Price price = level->price;

        // Add to O(1) lookup array
        if (is_in_range(price)) {
            levels_[price - base_price_] = level;
        }

        // Insert into sorted linked list
        if (!best_level_) {
            best_level_ = level;
            return;
        }

        // Find insertion point
        PriceLevel* curr = best_level_;
        PriceLevel* prev_level = nullptr;

        while (curr && compare_(curr->price, price)) {
            prev_level = curr;
            curr = curr->next;
        }

        level->next = curr;
        level->prev = prev_level;

        if (curr) {
            curr->prev = level;
        }

        if (prev_level) {
            prev_level->next = level;
        } else {
            best_level_ = level;
        }
    }

    // Remove level if empty, returns the removed level for deallocation
    // Returns nullptr if level was not empty (not removed)
    PriceLevel* remove_level_if_empty(PriceLevel* level) {
        if (!level->is_empty()) {
            return nullptr;
        }

        remove_level(level);
        return level;
    }

private:
    Price base_price_;
    size_t price_range_;

    std::unique_ptr<PriceLevel*[]> levels_;
    PriceLevel* best_level_;
    Compare compare_;  // Stored instance for readability

    bool is_in_range(Price price) const {
        return price >= base_price_ && price < base_price_ + price_range_;
    }

    void remove_level(PriceLevel* level) {
        Price price = level->price;

        // Remove from O(1) lookup array
        if (is_in_range(price)) {
            levels_[price - base_price_] = nullptr;
        }

        // Remove from linked list
        if (level->prev) {
            level->prev->next = level->next;
        } else {
            best_level_ = level->next;
        }

        if (level->next) {
            level->next->prev = level->prev;
        }
    }
};

// Type aliases for convenience
using BidSide = BookSide<BidCompare>;
using AskSide = BookSide<AskCompare>;

}  // namespace hft
