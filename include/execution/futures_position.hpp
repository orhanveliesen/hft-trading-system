#pragma once

#include "../trading/portfolio.hpp" // For hft::trading::MAX_SYMBOLS
#include "../types.hpp"
#include "order_request.hpp"

#include <array>
#include <cstdint>

namespace hft {
namespace execution {

/**
 * @brief Position source for futures positions
 *
 * Tracks why a position was opened to apply correct exit logic:
 * - Hedge: Cash-and-carry (spot long + futures short)
 * - Directional: Leveraged directional bet (Phase 5.2)
 * - Farming: Funding rate farming (contrarian to funding)
 */
enum class PositionSource {
    None = 0,    ///< No position (free slot)
    Hedge,       ///< Cash-and-carry hedge position
    Directional, ///< Leveraged directional position (Phase 5.2)
    Farming      ///< Funding rate farming position
};

/**
 * @brief Single futures position entry
 *
 * Futures allow both long AND short simultaneously (unlike spot).
 * Each symbol can have up to 4 positions (Hedge + Directional + 2x Farming).
 */
struct FuturesPositionEntry {
    PositionSource source = PositionSource::None;
    Side side = Side::Buy; ///< Long or Short
    double quantity = 0.0;
    Price entry_price = 0;     ///< Entry price (fixed-point)
    uint64_t open_time_ns = 0; ///< When position opened (for age calculation)

    bool is_active() const { return source != PositionSource::None && quantity > 0.0; }
    void clear() { *this = FuturesPositionEntry{}; }
};

/**
 * @brief Per-symbol futures position tracker
 *
 * Design:
 * - Symbol-indexed 2D array: positions_[symbol][0..3]
 * - O(4) = O(1) lookup per symbol (NOT O(256) flat array scan)
 * - Stack-allocated, no heap allocation, no dynamic memory
 * - Max 4 positions per symbol: Hedge + Directional + 2x Farming
 *
 * HFT Performance:
 * - NO heap allocation (all stack-allocated)
 * - NO O(n) linear scans (symbol-indexed lookup)
 * - Total: 64 symbols × 4 positions = 256 entries, accessed as positions_[symbol][0..3]
 *
 * Usage:
 *   FuturesPosition positions;
 *   int slot = positions.open_position(symbol, PositionSource::Hedge, Side::Sell, qty, price, now_ns);
 *   auto* pos = positions.get_position(symbol, PositionSource::Hedge, Side::Sell);
 *   positions.close_position(symbol, slot);
 */
class FuturesPosition {
public:
    static constexpr size_t MAX_POSITIONS_PER_SYMBOL = 4; ///< Hedge + Directional + 2x Farming

    /**
     * @brief Open a new position
     *
     * @param symbol Symbol ID
     * @param source Position source (Hedge, Directional, Farming)
     * @param side Long or Short
     * @param quantity Position size
     * @param entry_price Entry price (fixed-point)
     * @param open_time_ns Wall-clock time when position opened (passed from evaluator)
     * @return Slot index (0-3) if successful, -1 if no free slots
     */
    inline int open_position(Symbol symbol, PositionSource source, Side side, double quantity, Price entry_price,
                             uint64_t open_time_ns) {
        if (symbol >= hft::trading::MAX_SYMBOLS)
            return -1;

        // Find free slot in positions_[symbol][0..3]
        auto& slots = positions_[symbol];
        for (size_t i = 0; i < MAX_POSITIONS_PER_SYMBOL; ++i) {
            if (!slots[i].is_active()) {
                slots[i].source = source;
                slots[i].side = side;
                slots[i].quantity = quantity;
                slots[i].entry_price = entry_price;
                slots[i].open_time_ns = open_time_ns;
                return static_cast<int>(i);
            }
        }

        return -1; // No free slots
    }

    /**
     * @brief Close a position by symbol and slot index
     *
     * @param symbol Symbol ID
     * @param slot Slot index (0-3)
     * @return true if closed successfully
     */
    inline bool close_position(Symbol symbol, int slot) {
        if (symbol >= hft::trading::MAX_SYMBOLS || slot < 0 || slot >= static_cast<int>(MAX_POSITIONS_PER_SYMBOL))
            return false;

        auto& entry = positions_[symbol][slot];
        if (!entry.is_active())
            return false;

        entry.clear();
        return true;
    }

    /**
     * @brief Get total net position for a symbol (long - short)
     *
     * @param symbol Symbol ID
     * @return Net position (positive = net long, negative = net short, 0 = flat)
     */
    inline double get_net_position(Symbol symbol) const {
        if (symbol >= hft::trading::MAX_SYMBOLS)
            return 0.0;

        double net = 0.0;
        const auto& slots = positions_[symbol];
        for (const auto& entry : slots) {
            if (entry.is_active()) {
                if (entry.side == Side::Buy) {
                    net += entry.quantity;
                } else {
                    net -= entry.quantity;
                }
            }
        }
        return net;
    }

    /**
     * @brief Get position by source and side (O(4) lookup per symbol)
     *
     * @param symbol Symbol ID
     * @param source Position source to find
     * @param side Position side to find
     * @return Pointer to position or nullptr if not found
     */
    inline const FuturesPositionEntry* get_position(Symbol symbol, PositionSource source, Side side) const {
        if (symbol >= hft::trading::MAX_SYMBOLS)
            return nullptr;

        // Linear search through 4 slots (O(4) = O(1))
        const auto& slots = positions_[symbol];
        for (const auto& entry : slots) {
            if (entry.is_active() && entry.source == source && entry.side == side) {
                return &entry;
            }
        }
        return nullptr;
    }

    /**
     * @brief Check if can open a new position for symbol (have free slot)
     *
     * @param symbol Symbol ID
     * @return true if at least one free slot available (0-3)
     */
    inline bool can_open_position(Symbol symbol) const {
        if (symbol >= hft::trading::MAX_SYMBOLS)
            return false;

        const auto& slots = positions_[symbol];
        for (const auto& entry : slots) {
            if (!entry.is_active()) {
                return true; // Found free slot
            }
        }
        return false; // All 4 slots occupied
    }

private:
    // Symbol-indexed 2D array: positions_[symbol][0..3]
    // O(4) lookup per symbol instead of O(256) flat array scan
    // Total: 64 symbols × 4 positions = 256 entries
    std::array<std::array<FuturesPositionEntry, MAX_POSITIONS_PER_SYMBOL>, hft::trading::MAX_SYMBOLS> positions_;
};

} // namespace execution
} // namespace hft
