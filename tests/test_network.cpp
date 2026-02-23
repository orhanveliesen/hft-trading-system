#include "../include/network/packet_buffer.hpp"
#include "../include/network/udp_receiver.hpp"
#include "../include/types.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

using namespace hft;
using namespace hft::network;

#define TEST(name) void name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "Running " << #name << "... ";                                                                    \
        name();                                                                                                        \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

#define ASSERT_EQ(a, b) assert((a) == (b))
#define ASSERT_TRUE(x) assert(x)
#define ASSERT_FALSE(x) assert(!(x))

// Test: Packet buffer initialization
TEST(test_packet_buffer_init) {
    PacketBuffer<1024, 64> buffer; // 1KB packets, 64 slots

    ASSERT_TRUE(buffer.empty());
    ASSERT_FALSE(buffer.full());
    ASSERT_EQ(buffer.size(), 0);
}

// Test: Packet buffer write and read
TEST(test_packet_buffer_write_read) {
    PacketBuffer<256, 8> buffer;

    uint8_t data[] = {1, 2, 3, 4, 5};
    ASSERT_TRUE(buffer.push(data, 5));

    ASSERT_FALSE(buffer.empty());
    ASSERT_EQ(buffer.size(), 1);

    auto* packet = buffer.front();
    ASSERT_TRUE(packet != nullptr);
    ASSERT_EQ(packet->len, 5);
    ASSERT_EQ(packet->data[0], 1);
    ASSERT_EQ(packet->data[4], 5);

    buffer.pop();
    ASSERT_TRUE(buffer.empty());
}

// Test: Packet buffer FIFO order
TEST(test_packet_buffer_fifo) {
    PacketBuffer<256, 8> buffer;

    uint8_t data1[] = {1, 1, 1};
    uint8_t data2[] = {2, 2, 2};
    uint8_t data3[] = {3, 3, 3};

    buffer.push(data1, 3);
    buffer.push(data2, 3);
    buffer.push(data3, 3);

    ASSERT_EQ(buffer.front()->data[0], 1);
    buffer.pop();
    ASSERT_EQ(buffer.front()->data[0], 2);
    buffer.pop();
    ASSERT_EQ(buffer.front()->data[0], 3);
    buffer.pop();
    ASSERT_TRUE(buffer.empty());
}

// Test: Packet buffer full condition
// Note: Ring buffer keeps one slot empty to distinguish full from empty
// So capacity N means N-1 usable slots
TEST(test_packet_buffer_full) {
    PacketBuffer<256, 4> buffer; // 4 slots = 3 usable

    uint8_t data[] = {0};
    ASSERT_TRUE(buffer.push(data, 1)); // 1
    ASSERT_TRUE(buffer.push(data, 1)); // 2
    ASSERT_TRUE(buffer.push(data, 1)); // 3 - now full

    ASSERT_TRUE(buffer.full());
    ASSERT_FALSE(buffer.push(data, 1)); // Should fail when full
}

// Test: Packet buffer wraparound
TEST(test_packet_buffer_wraparound) {
    PacketBuffer<256, 4> buffer; // 3 usable slots

    uint8_t data[] = {0};

    // Fill and drain multiple times to test wraparound
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 3; ++i) { // Only 3 slots usable
            data[0] = round * 10 + i;
            ASSERT_TRUE(buffer.push(data, 1));
        }
        for (int i = 0; i < 3; ++i) {
            ASSERT_EQ(buffer.front()->data[0], round * 10 + i);
            buffer.pop();
        }
    }
}

// Test: MoldUDP64 header parsing
TEST(test_moldudp_header_parse) {
    // MoldUDP64 header: session(10) + seq(8) + count(2) = 20 bytes
    uint8_t header[20] = {0};

    // Session ID (10 bytes)
    std::memcpy(header, "SESSION001", 10);

    // Sequence number (big-endian uint64)
    header[10] = 0;
    header[11] = 0;
    header[12] = 0;
    header[13] = 0;
    header[14] = 0;
    header[15] = 0;
    header[16] = 0;
    header[17] = 42; // seq = 42

    // Message count (big-endian uint16)
    header[18] = 0;
    header[19] = 5; // count = 5

    MoldUDP64Header parsed = parse_moldudp_header(header);

    ASSERT_EQ(parsed.sequence_number, 42);
    ASSERT_EQ(parsed.message_count, 5);
}

// Test: UDP socket creation (may fail without proper permissions)
TEST(test_udp_socket_create) {
    // This test just verifies the API compiles
    // Actual socket tests require network access
    UdpConfig config;
    config.interface = "127.0.0.1";
    config.multicast_group = "239.1.1.1";
    config.port = 12345;
    config.recv_buffer_size = 1024 * 1024; // 1MB

    // Just verify config struct works
    ASSERT_EQ(config.port, 12345);
}

int main() {
    std::cout << "=== Network Tests ===\n";

    RUN_TEST(test_packet_buffer_init);
    RUN_TEST(test_packet_buffer_write_read);
    RUN_TEST(test_packet_buffer_fifo);
    RUN_TEST(test_packet_buffer_full);
    RUN_TEST(test_packet_buffer_wraparound);
    RUN_TEST(test_moldudp_header_parse);
    RUN_TEST(test_udp_socket_create);

    std::cout << "\nAll tests PASSED!\n";
    return 0;
}
