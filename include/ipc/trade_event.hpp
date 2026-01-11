#pragma once

#include <cstdint>
#include <cstring>

namespace hft {
namespace ipc {

/**
 * Event types that the HFT engine publishes
 */
enum class EventType : uint8_t {
    None = 0,
    Quote,          // Price update received
    Signal,         // Trading signal generated
    OrderSent,      // Order sent to exchange
    Fill,           // Order filled
    TargetHit,      // Take-profit triggered
    StopLoss,       // Stop-loss triggered
    RegimeChange,   // Market regime changed
    Error           // Error occurred
};

/**
 * TradeEvent - POD struct for lock-free IPC
 *
 * Requirements for shared memory:
 * - POD (Plain Old Data) - no virtual functions, no pointers
 * - Fixed size - no dynamic allocation
 * - Cache-line aligned (64 bytes) - prevent false sharing
 *
 * Size: 64 bytes (1 cache line)
 */
struct alignas(64) TradeEvent {
    // Timestamp (8 bytes)
    uint64_t timestamp_ns;      // Nanoseconds since epoch

    // Event identification (4 bytes)
    EventType type;             // What happened
    uint8_t padding1[3];        // Alignment padding

    // Symbol info (8 bytes)
    uint32_t symbol_id;         // Symbol ID
    char ticker[4];             // Short ticker (e.g., "BTC\0")

    // Price/quantity (24 bytes)
    double price;               // Price (or bid for quotes)
    double price2;              // Ask price (for quotes) or entry price (for fills)
    double quantity;            // Quantity

    // Additional info (16 bytes)
    int64_t pnl_cents;          // P&L in cents (to avoid floating point)
    uint32_t order_id;          // Order ID if applicable
    uint8_t side;               // 0=Buy, 1=Sell
    uint8_t regime;             // Market regime
    uint8_t signal_strength;    // Signal strength (0-3)
    uint8_t padding2;           // Alignment padding

    // Sequence for ordering (4 bytes) - total now 64 bytes
    uint32_t sequence;          // Monotonic sequence number

    // Helper methods
    void clear() {
        std::memset(this, 0, sizeof(TradeEvent));
    }

    void set_ticker(const char* t) {
        ticker[0] = t[0];
        ticker[1] = t[1];
        ticker[2] = t[2];
        ticker[3] = '\0';
    }

    static TradeEvent quote(uint32_t seq, uint64_t ts, uint32_t sym,
                            const char* tick, double bid, double ask) {
        TradeEvent e;
        e.clear();
        e.sequence = seq;
        e.timestamp_ns = ts;
        e.type = EventType::Quote;
        e.symbol_id = sym;
        e.set_ticker(tick);
        e.price = bid;
        e.price2 = ask;
        return e;
    }

    static TradeEvent fill(uint32_t seq, uint64_t ts, uint32_t sym,
                          const char* tick, uint8_t side, double price,
                          double qty, uint32_t oid) {
        TradeEvent e;
        e.clear();
        e.sequence = seq;
        e.timestamp_ns = ts;
        e.type = EventType::Fill;
        e.symbol_id = sym;
        e.set_ticker(tick);
        e.side = side;
        e.price = price;
        e.quantity = qty;
        e.order_id = oid;
        return e;
    }

    static TradeEvent target_hit(uint32_t seq, uint64_t ts, uint32_t sym,
                                 const char* tick, double entry, double exit,
                                 double qty, int64_t pnl) {
        TradeEvent e;
        e.clear();
        e.sequence = seq;
        e.timestamp_ns = ts;
        e.type = EventType::TargetHit;
        e.symbol_id = sym;
        e.set_ticker(tick);
        e.price = exit;
        e.price2 = entry;
        e.quantity = qty;
        e.pnl_cents = pnl;
        return e;
    }

    static TradeEvent stop_loss(uint32_t seq, uint64_t ts, uint32_t sym,
                                const char* tick, double entry, double exit,
                                double qty, int64_t pnl) {
        TradeEvent e;
        e.clear();
        e.sequence = seq;
        e.timestamp_ns = ts;
        e.type = EventType::StopLoss;
        e.symbol_id = sym;
        e.set_ticker(tick);
        e.price = exit;
        e.price2 = entry;
        e.quantity = qty;
        e.pnl_cents = pnl;
        return e;
    }

    static TradeEvent signal(uint32_t seq, uint64_t ts, uint32_t sym,
                            const char* tick, uint8_t side, uint8_t strength,
                            double price) {
        TradeEvent e;
        e.clear();
        e.sequence = seq;
        e.timestamp_ns = ts;
        e.type = EventType::Signal;
        e.symbol_id = sym;
        e.set_ticker(tick);
        e.side = side;
        e.signal_strength = strength;
        e.price = price;
        return e;
    }

    static TradeEvent regime_change(uint32_t seq, uint64_t ts, uint32_t sym,
                                    const char* tick, uint8_t new_regime) {
        TradeEvent e;
        e.clear();
        e.sequence = seq;
        e.timestamp_ns = ts;
        e.type = EventType::RegimeChange;
        e.symbol_id = sym;
        e.set_ticker(tick);
        e.regime = new_regime;
        return e;
    }
};

// Ensure cache-line alignment (128 bytes = 2 cache lines due to alignas padding)
static_assert(sizeof(TradeEvent) == 128, "TradeEvent must be 128 bytes");
static_assert(alignof(TradeEvent) == 64, "TradeEvent must be cache-line aligned");

}  // namespace ipc
}  // namespace hft
