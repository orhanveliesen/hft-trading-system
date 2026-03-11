#include "../include/network/packet_buffer.hpp"

#include <cassert>
#include <cstring>
#include <iostream>

using namespace hft::network;

void test_push_oversized_packet() {
    // Test pushing a packet larger than MaxPacketSize (covers line 32)
    constexpr size_t MaxPacketSize = 1500;
    constexpr size_t Capacity = 1024;
    PacketBuffer<MaxPacketSize, Capacity> buffer;

    // Create a packet that exceeds MaxPacketSize
    uint8_t oversized[MaxPacketSize + 100];
    std::memset(oversized, 0xAA, sizeof(oversized));

    // Should return false for oversized packet
    bool result = buffer.push(oversized, sizeof(oversized));
    assert(!result);

    // Buffer should still be empty
    assert(buffer.empty());
}

void test_front_on_empty_buffer() {
    // Test calling front() on empty buffer (covers line 51)
    constexpr size_t MaxPacketSize = 1500;
    constexpr size_t Capacity = 1024;
    PacketBuffer<MaxPacketSize, Capacity> buffer;

    // front() should return nullptr on empty buffer
    const auto* packet = buffer.front();
    assert(packet == nullptr);
}

void test_normal_operations() {
    // Test normal push/pop operations
    constexpr size_t MaxPacketSize = 1500;
    constexpr size_t Capacity = 1024;
    PacketBuffer<MaxPacketSize, Capacity> buffer;

    // Push a packet
    uint8_t data[100];
    std::memset(data, 0x42, sizeof(data));
    bool result = buffer.push(data, sizeof(data));
    assert(result);
    assert(!buffer.empty());

    // Front should return the packet
    const auto* packet = buffer.front();
    assert(packet != nullptr);
    assert(packet->len == sizeof(data));
    assert(std::memcmp(packet->data, data, sizeof(data)) == 0);

    // Pop the packet
    buffer.pop();
    assert(buffer.empty());
}

int main() {
    test_push_oversized_packet();
    test_front_on_empty_buffer();
    test_normal_operations();

    std::cout << "All packet_buffer tests passed!\n";
    return 0;
}
