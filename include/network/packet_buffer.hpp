#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace hft {
namespace network {

// Fixed-size packet for zero-copy buffer
template <size_t MaxPacketSize>
struct Packet {
    uint8_t data[MaxPacketSize];
    size_t len;
};

// Lock-free single-producer single-consumer ring buffer for packets
// - Pre-allocated, no heap allocation on hot path
// - Cache-line aligned for performance
template <size_t MaxPacketSize, size_t Capacity>
class PacketBuffer {
public:
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

    PacketBuffer() : head_(0), tail_(0) {}

    // Producer: Push packet into buffer
    // Returns false if buffer is full
    bool push(const uint8_t* data, size_t len) {
        if (len > MaxPacketSize)
            return false;

        size_t head = head_;
        size_t next_head = (head + 1) & (Capacity - 1);

        if (next_head == tail_) {
            return false; // Buffer full
        }

        packets_[head].len = len;
        std::memcpy(packets_[head].data, data, len);

        head_ = next_head;
        return true;
    }

    // Consumer: Get front packet (does not remove)
    const Packet<MaxPacketSize>* front() const {
        if (empty())
            return nullptr;
        return &packets_[tail_];
    }

    // Consumer: Remove front packet
    void pop() {
        if (!empty()) {
            tail_ = (tail_ + 1) & (Capacity - 1);
        }
    }

    bool empty() const { return head_ == tail_; }

    bool full() const { return ((head_ + 1) & (Capacity - 1)) == tail_; }

    size_t size() const {
        size_t h = head_;
        size_t t = tail_;
        return (h >= t) ? (h - t) : (Capacity - t + h);
    }

    size_t capacity() const { return Capacity - 1; }

private:
    alignas(64) size_t head_; // Cache-line aligned
    alignas(64) size_t tail_; // Separate cache line to avoid false sharing
    std::array<Packet<MaxPacketSize>, Capacity> packets_;
};

} // namespace network
} // namespace hft
