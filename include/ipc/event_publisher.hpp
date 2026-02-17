#pragma once

/**
 * EventPublisher - Publishes trading events to shared memory
 *
 * Used by HFT engine to send events to observer process.
 * Lock-free, ~5ns per publish, no allocation.
 */

#include "shared_ring_buffer.hpp"
#include "trade_event.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>

namespace hft {
namespace ipc {

class EventPublisher {
public:
    explicit EventPublisher(bool enabled = true) : enabled_(enabled) {
        if (enabled_) {
            try {
                // TODO: Use timestamp-based naming (e.g., /trader-events-20260217153000000)
                //       and organize shared memory files in a dedicated folder.
                //       Provide discovery API for other applications.
                buffer_ = std::make_unique<SharedRingBuffer<TradeEvent>>("/trader_events", true);
                std::cout << "[IPC] Event publisher initialized (buffer: "
                          << buffer_->capacity() << " events)\n";
            } catch (const std::exception& e) {
                std::cerr << "[IPC] Warning: Could not create shared memory: " << e.what() << "\n";
                enabled_ = false;
            }
        }
    }

    void fill(uint32_t sym, const char* ticker, uint8_t side, double price, double qty, uint32_t oid) {
        if (!enabled_) return;
        auto ts = now_ns();
        buffer_->push(TradeEvent::fill(seq_++, ts, sym, ticker, side, price, qty, oid));
    }

    void target_hit(uint32_t sym, const char* ticker, double entry, double exit, double qty) {
        if (!enabled_) return;
        auto ts = now_ns();
        double pnl = (exit - entry) * qty;
        buffer_->push(TradeEvent::target_hit(seq_++, ts, sym, ticker, entry, exit, qty, pnl));
    }

    void stop_loss(uint32_t sym, const char* ticker, double entry, double exit, double qty) {
        if (!enabled_) return;
        auto ts = now_ns();
        double pnl = (exit - entry) * qty;
        buffer_->push(TradeEvent::stop_loss(seq_++, ts, sym, ticker, entry, exit, qty, pnl));
    }

    void signal(uint32_t sym, const char* ticker, uint8_t side, uint8_t strength, double price) {
        if (!enabled_) return;
        auto ts = now_ns();
        buffer_->push(TradeEvent::signal(seq_++, ts, sym, ticker, side, strength, price));
    }

    void regime_change(uint32_t sym, const char* ticker, uint8_t new_regime) {
        if (!enabled_) return;
        auto ts = now_ns();
        buffer_->push(TradeEvent::regime_change(seq_++, ts, sym, ticker, new_regime));
    }

    void status(uint32_t sym, const char* ticker, StatusCode code,
                double price = 0, uint8_t sig_strength = 0, uint8_t regime = 0) {
        if (!enabled_) return;
        auto ts = now_ns();
        buffer_->push(TradeEvent::status(seq_++, ts, sym, ticker, code, price, sig_strength, regime));
    }

    void heartbeat() {
        if (!enabled_) return;
        auto ts = now_ns();
        buffer_->push(TradeEvent::status(seq_++, ts, 0, "SYS", StatusCode::Heartbeat));
    }

    bool enabled() const { return enabled_; }
    uint32_t sequence() const { return seq_; }

private:
    bool enabled_ = false;
    std::unique_ptr<SharedRingBuffer<TradeEvent>> buffer_;
    std::atomic<uint32_t> seq_{0};

    static uint64_t now_ns() {
        return std::chrono::steady_clock::now().time_since_epoch().count();
    }
};

}  // namespace ipc
}  // namespace hft
