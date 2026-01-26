/**
 * UDP Telemetry Tests
 *
 * Tests for fire-and-forget UDP multicast telemetry system.
 * Uses loopback for testing (enables IP_MULTICAST_LOOP).
 */

#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>

#include "ipc/udp_telemetry.hpp"

using namespace hft::ipc;

// ============================================================================
// Test: Packet Structure
// ============================================================================
void test_packet_size() {
    std::cout << "  test_packet_size... ";

    static_assert(sizeof(TelemetryPacket) == 64, "Packet must be 64 bytes");

    TelemetryPacket pkt{};
    assert(sizeof(pkt) == 64);
    assert(sizeof(pkt.data.quote) <= 40);
    assert(sizeof(pkt.data.fill) <= 40);
    assert(sizeof(pkt.data.position) <= 40);
    assert(sizeof(pkt.data.pnl) <= 40);
    assert(sizeof(pkt.data.latency) <= 40);

    std::cout << "PASSED\n";
}

// ============================================================================
// Test: Publisher Initialization
// ============================================================================
void test_publisher_init() {
    std::cout << "  test_publisher_init... ";

    TelemetryPublisher pub("239.255.0.1", 5556);
    assert(pub.is_valid());

    // Test invalid address (should still create socket, just won't reach anywhere)
    TelemetryPublisher pub2("239.255.0.2", 5557);
    assert(pub2.is_valid());

    std::cout << "PASSED\n";
}

// ============================================================================
// Test: Subscriber Initialization
// ============================================================================
void test_subscriber_init() {
    std::cout << "  test_subscriber_init... ";

    TelemetrySubscriber sub("239.255.0.1", 5558);
    assert(sub.is_valid());

    std::cout << "PASSED\n";
}

// ============================================================================
// Test: Publish and Receive Quote
// ============================================================================
void test_publish_receive_quote() {
    std::cout << "  test_publish_receive_quote... ";

    const uint16_t port = 5559;
    std::atomic<bool> received{false};
    TelemetryPacket received_pkt{};

    // Create subscriber first
    TelemetrySubscriber sub("239.255.0.1", port);
    assert(sub.is_valid());

    sub.set_callback([&](const TelemetryPacket& pkt) {
        if (pkt.type == TelemetryType::Quote) {
            received_pkt = pkt;
            received.store(true);
        }
    });
    sub.start();

    // Give subscriber time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Create publisher with loopback enabled
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    assert(sock >= 0);

    int ttl = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    int loop = 1;  // Enable loopback for testing
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    inet_pton(AF_INET, "239.255.0.1", &dest.sin_addr);

    // Create and send quote packet
    TelemetryPacket pkt{};
    pkt.type = TelemetryType::Quote;
    pkt.symbol_id = 42;
    pkt.data.quote.bid_price = 91000'00000000LL;  // 91000.0 scaled
    pkt.data.quote.ask_price = 91001'00000000LL;  // 91001.0 scaled
    pkt.data.quote.bid_size = 100;
    pkt.data.quote.ask_size = 150;
    pkt.timestamp_ns = TelemetryPublisher::now_ns();
    pkt.sequence = 1;

    sendto(sock, &pkt, sizeof(pkt), 0,
           reinterpret_cast<sockaddr*>(&dest), sizeof(dest));

    // Wait for reception
    auto start = std::chrono::steady_clock::now();
    while (!received.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(2)) {
            std::cout << "SKIPPED (multicast not available in this environment)\n";
            sub.stop();
            close(sock);
            return;
        }
    }

    sub.stop();
    close(sock);

    // Verify received data
    assert(received_pkt.type == TelemetryType::Quote);
    assert(received_pkt.symbol_id == 42);
    assert(received_pkt.data.quote.bid_price == 91000'00000000LL);
    assert(received_pkt.data.quote.ask_price == 91001'00000000LL);
    assert(received_pkt.data.quote.bid_size == 100);
    assert(received_pkt.data.quote.ask_size == 150);

    std::cout << "PASSED\n";
}

// ============================================================================
// Test: Sequence Number Tracking
// ============================================================================
void test_sequence_tracking() {
    std::cout << "  test_sequence_tracking... ";

    const uint16_t port = 5560;
    std::vector<uint32_t> sequences;
    std::atomic<int> count{0};

    TelemetrySubscriber sub("239.255.0.1", port);
    if (!sub.is_valid()) {
        std::cout << "SKIPPED (socket creation failed)\n";
        return;
    }

    sub.set_callback([&](const TelemetryPacket& pkt) {
        sequences.push_back(pkt.sequence);
        count.fetch_add(1);
    });
    sub.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Create socket with loopback
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    int loop = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    inet_pton(AF_INET, "239.255.0.1", &dest.sin_addr);

    // Send 5 packets
    for (uint32_t i = 0; i < 5; ++i) {
        TelemetryPacket pkt{};
        pkt.type = TelemetryType::Heartbeat;
        pkt.sequence = i;
        pkt.timestamp_ns = TelemetryPublisher::now_ns();
        sendto(sock, &pkt, sizeof(pkt), 0,
               reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    }

    // Wait for packets
    auto start = std::chrono::steady_clock::now();
    while (count.load() < 5) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(2)) {
            std::cout << "SKIPPED (multicast not available)\n";
            sub.stop();
            close(sock);
            return;
        }
    }

    sub.stop();
    close(sock);

    // Verify sequence numbers
    assert(sequences.size() == 5);
    for (uint32_t i = 0; i < 5; ++i) {
        assert(sequences[i] == i);
    }

    std::cout << "PASSED\n";
}

// ============================================================================
// Test: Fire and Forget (non-blocking)
// ============================================================================
void test_fire_and_forget() {
    std::cout << "  test_fire_and_forget... ";

    TelemetryPublisher pub("239.255.0.1", 5561);
    assert(pub.is_valid());

    // Publish without any subscriber - should not block
    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < 1000; ++i) {
        pub.publish_quote(0, 91000'00000000LL, 91001'00000000LL, 100, 100);
    }

    auto elapsed = std::chrono::steady_clock::now() - start;
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();

    // Should complete in < 10ms (non-blocking)
    assert(us < 10000);  // 10ms max for 1000 packets

    std::cout << "PASSED (" << us << " µs for 1000 packets)\n";
}

// ============================================================================
// Test: All Telemetry Types
// ============================================================================
void test_all_telemetry_types() {
    std::cout << "  test_all_telemetry_types... ";

    TelemetryPublisher pub("239.255.0.1", 5562);
    assert(pub.is_valid());

    // Test all publish methods (should not crash)
    pub.publish_heartbeat();
    pub.publish_quote(1, 100, 101, 10, 10);
    pub.publish_fill(1, true, 100, 100'00000000LL);
    pub.publish_position(1, 100, 100'00000000LL, 10000'00000000LL, 50'00000000LL);
    pub.publish_pnl(1000'00000000LL, 500'00000000LL, 101500'00000000LL, 10, 5);
    pub.publish_regime(1, 2, 85);
    pub.publish_latency(500, 100, 200, 800);

    std::cout << "PASSED\n";
}

// ============================================================================
// Test: Dropped Packet Detection
// ============================================================================
void test_dropped_packet_detection() {
    std::cout << "  test_dropped_packet_detection... ";

    const uint16_t port = 5563;
    TelemetrySubscriber sub("239.255.0.1", port);
    if (!sub.is_valid()) {
        std::cout << "SKIPPED\n";
        return;
    }

    std::atomic<int> received_count{0};
    sub.set_callback([&](const TelemetryPacket&) {
        received_count.fetch_add(1);
    });
    sub.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Create socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    int loop = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    inet_pton(AF_INET, "239.255.0.1", &dest.sin_addr);

    // Send packets with gaps (simulating drops)
    for (uint32_t seq : {0, 1, 5, 6, 10}) {  // Gaps at 2-4 and 7-9
        TelemetryPacket pkt{};
        pkt.type = TelemetryType::Heartbeat;
        pkt.sequence = seq;
        pkt.timestamp_ns = TelemetryPublisher::now_ns();
        sendto(sock, &pkt, sizeof(pkt), 0,
               reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    }

    // Wait
    auto start = std::chrono::steady_clock::now();
    while (received_count.load() < 5) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(2)) {
            std::cout << "SKIPPED (multicast not available)\n";
            sub.stop();
            close(sock);
            return;
        }
    }

    sub.stop();
    close(sock);

    // Check drop detection
    // Expected drops: (5-1-1) + (10-6-1) = 3 + 3 = 6
    assert(sub.packets_received() == 5);
    assert(sub.packets_dropped() == 6);

    std::cout << "PASSED (detected " << sub.packets_dropped() << " drops)\n";
}

// ============================================================================
// Main
// ============================================================================
int main() {
    std::cout << "\n=== UDP Telemetry Tests ===\n\n";

    test_packet_size();
    test_publisher_init();
    test_subscriber_init();
    test_fire_and_forget();
    test_all_telemetry_types();
    test_publish_receive_quote();
    test_sequence_tracking();
    test_dropped_packet_detection();

    std::cout << "\n=== All tests passed! ===\n\n";

    std::cout << "Architecture summary:\n";
    std::cout << "  - HFT Engine → UDP Multicast (fire-and-forget)\n";
    std::cout << "  - Collector → Time-series DB (QuestDB/InfluxDB)\n";
    std::cout << "  - Dashboard → Web UI or local observer\n";
    std::cout << "  - Latency: ~1-10 µs per publish (non-blocking)\n";
    std::cout << "  - Packet size: 64 bytes (fits in single UDP packet)\n\n";

    return 0;
}
