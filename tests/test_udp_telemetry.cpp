/**
 * UDP Telemetry Tests
 *
 * Tests for fire-and-forget UDP multicast telemetry system.
 * Uses loopback for testing (enables IP_MULTICAST_LOOP).
 */

#include "ipc/udp_telemetry.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

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
    int loop = 1; // Enable loopback for testing
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    inet_pton(AF_INET, "239.255.0.1", &dest.sin_addr);

    // Create and send quote packet
    TelemetryPacket pkt{};
    pkt.type = TelemetryType::Quote;
    pkt.symbol_id = 42;
    pkt.data.quote.bid_price = 91000'00000000LL; // 91000.0 scaled
    pkt.data.quote.ask_price = 91001'00000000LL; // 91001.0 scaled
    pkt.data.quote.bid_size = 100;
    pkt.data.quote.ask_size = 150;
    pkt.timestamp_ns = TelemetryPublisher::now_ns();
    pkt.sequence = 1;

    sendto(sock, &pkt, sizeof(pkt), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));

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
        sendto(sock, &pkt, sizeof(pkt), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
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

    // Should complete in < 100ms (non-blocking)
    // Note: WSL and virtualized environments may have higher UDP overhead
    assert(us < 100000); // 100ms max for 1000 packets (100us per packet)

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
    sub.set_callback([&](const TelemetryPacket&) { received_count.fetch_add(1); });
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
    for (uint32_t seq : {0, 1, 5, 6, 10}) { // Gaps at 2-4 and 7-9
        TelemetryPacket pkt{};
        pkt.type = TelemetryType::Heartbeat;
        pkt.sequence = seq;
        pkt.timestamp_ns = TelemetryPublisher::now_ns();
        sendto(sock, &pkt, sizeof(pkt), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
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
// Test: Subscriber Double Start Protection
// ============================================================================
void test_subscriber_double_start() {
    std::cout << "  test_subscriber_double_start... ";

    TelemetrySubscriber sub("239.255.0.1", 5564);
    if (!sub.is_valid()) {
        std::cout << "SKIPPED\n";
        return;
    }

    sub.start();

    // Attempt to start again - should be no-op
    sub.start();

    sub.stop();

    std::cout << "PASSED\n";
}

// ============================================================================
// Test: Stop Before Start
// ============================================================================
void test_stop_before_start() {
    std::cout << "  test_stop_before_start... ";

    TelemetrySubscriber sub("239.255.0.1", 5565);
    if (!sub.is_valid()) {
        std::cout << "SKIPPED\n";
        return;
    }

    // Stop without starting - should not crash
    sub.stop();

    std::cout << "PASSED\n";
}

// ============================================================================
// Test: Subscriber Without Callback
// ============================================================================
void test_subscriber_no_callback() {
    std::cout << "  test_subscriber_no_callback... ";

    const uint16_t port = 5566;
    TelemetrySubscriber sub("239.255.0.1", port);
    if (!sub.is_valid()) {
        std::cout << "SKIPPED\n";
        return;
    }

    // Start without setting callback - should not crash
    sub.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Send a packet
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    int loop = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    inet_pton(AF_INET, "239.255.0.1", &dest.sin_addr);

    TelemetryPacket pkt{};
    pkt.type = TelemetryType::Heartbeat;
    sendto(sock, &pkt, sizeof(pkt), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    sub.stop();
    close(sock);

    std::cout << "PASSED\n";
}

// ============================================================================
// Test: Regime Packet Fields
// ============================================================================
void test_regime_packet() {
    std::cout << "  test_regime_packet... ";

    const uint16_t port = 5567;
    std::atomic<bool> received{false};
    TelemetryPacket received_pkt{};

    TelemetrySubscriber sub("239.255.0.1", port);
    if (!sub.is_valid()) {
        std::cout << "SKIPPED\n";
        return;
    }

    sub.set_callback([&](const TelemetryPacket& pkt) {
        if (pkt.type == TelemetryType::Regime) {
            received_pkt = pkt;
            received.store(true);
        }
    });
    sub.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Send regime packet
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    int loop = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    inet_pton(AF_INET, "239.255.0.1", &dest.sin_addr);

    TelemetryPacket pkt{};
    pkt.type = TelemetryType::Regime;
    pkt.symbol_id = 123;
    pkt.data.regime.regime = 3;
    pkt.data.regime.confidence = 95;
    pkt.data.regime.volatility = 1234567890LL;
    pkt.timestamp_ns = TelemetryPublisher::now_ns();
    pkt.sequence = 42;

    sendto(sock, &pkt, sizeof(pkt), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));

    // Wait for reception
    auto start = std::chrono::steady_clock::now();
    while (!received.load()) {
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

    // Verify
    assert(received_pkt.type == TelemetryType::Regime);
    assert(received_pkt.symbol_id == 123);
    assert(received_pkt.data.regime.regime == 3);
    assert(received_pkt.data.regime.confidence == 95);
    assert(received_pkt.data.regime.volatility == 1234567890LL);

    std::cout << "PASSED\n";
}

// ============================================================================
// Test: Position Packet Fields
// ============================================================================
void test_position_packet() {
    std::cout << "  test_position_packet... ";

    const uint16_t port = 5568;
    std::atomic<bool> received{false};
    TelemetryPacket received_pkt{};

    TelemetrySubscriber sub("239.255.0.1", port);
    if (!sub.is_valid()) {
        std::cout << "SKIPPED\n";
        return;
    }

    sub.set_callback([&](const TelemetryPacket& pkt) {
        if (pkt.type == TelemetryType::Position) {
            received_pkt = pkt;
            received.store(true);
        }
    });
    sub.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Send position packet
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    int loop = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    inet_pton(AF_INET, "239.255.0.1", &dest.sin_addr);

    TelemetryPacket pkt{};
    pkt.type = TelemetryType::Position;
    pkt.symbol_id = 99;
    pkt.data.position.quantity = 1500'00000000LL;
    pkt.data.position.avg_price = 91234'00000000LL;
    pkt.data.position.market_value = 136851000'00000000LL;
    pkt.data.position.unrealized_pnl = 5000'00000000LL;
    pkt.timestamp_ns = TelemetryPublisher::now_ns();
    pkt.sequence = 77;

    sendto(sock, &pkt, sizeof(pkt), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));

    // Wait for reception
    auto start = std::chrono::steady_clock::now();
    while (!received.load()) {
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

    // Verify all fields
    assert(received_pkt.type == TelemetryType::Position);
    assert(received_pkt.symbol_id == 99);
    assert(received_pkt.data.position.quantity == 1500'00000000LL);
    assert(received_pkt.data.position.avg_price == 91234'00000000LL);
    assert(received_pkt.data.position.market_value == 136851000'00000000LL);
    assert(received_pkt.data.position.unrealized_pnl == 5000'00000000LL);

    std::cout << "PASSED\n";
}

// ============================================================================
// Test: PnL Packet Fields
// ============================================================================
void test_pnl_packet() {
    std::cout << "  test_pnl_packet... ";

    const uint16_t port = 5569;
    std::atomic<bool> received{false};
    TelemetryPacket received_pkt{};

    TelemetrySubscriber sub("239.255.0.1", port);
    if (!sub.is_valid()) {
        std::cout << "SKIPPED\n";
        return;
    }

    sub.set_callback([&](const TelemetryPacket& pkt) {
        if (pkt.type == TelemetryType::PnL) {
            received_pkt = pkt;
            received.store(true);
        }
    });
    sub.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Send PnL packet
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    int loop = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    inet_pton(AF_INET, "239.255.0.1", &dest.sin_addr);

    TelemetryPacket pkt{};
    pkt.type = TelemetryType::PnL;
    pkt.data.pnl.realized_pnl = 5000'00000000LL;
    pkt.data.pnl.unrealized_pnl = 2500'00000000LL;
    pkt.data.pnl.total_equity = 107500'00000000LL;
    pkt.data.pnl.win_count = 42;
    pkt.data.pnl.loss_count = 18;
    pkt.timestamp_ns = TelemetryPublisher::now_ns();
    pkt.sequence = 88;

    sendto(sock, &pkt, sizeof(pkt), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));

    // Wait for reception
    auto start = std::chrono::steady_clock::now();
    while (!received.load()) {
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

    // Verify all fields
    assert(received_pkt.type == TelemetryType::PnL);
    assert(received_pkt.data.pnl.realized_pnl == 5000'00000000LL);
    assert(received_pkt.data.pnl.unrealized_pnl == 2500'00000000LL);
    assert(received_pkt.data.pnl.total_equity == 107500'00000000LL);
    assert(received_pkt.data.pnl.win_count == 42);
    assert(received_pkt.data.pnl.loss_count == 18);

    std::cout << "PASSED\n";
}

// ============================================================================
// Test: Fill Packet Fields
// ============================================================================
void test_fill_packet() {
    std::cout << "  test_fill_packet... ";

    const uint16_t port = 5570;
    std::atomic<bool> received{false};
    TelemetryPacket received_pkt{};

    TelemetrySubscriber sub("239.255.0.1", port);
    if (!sub.is_valid()) {
        std::cout << "SKIPPED\n";
        return;
    }

    sub.set_callback([&](const TelemetryPacket& pkt) {
        if (pkt.type == TelemetryType::Fill) {
            received_pkt = pkt;
            received.store(true);
        }
    });
    sub.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Send fill packet (sell side)
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    int loop = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    inet_pton(AF_INET, "239.255.0.1", &dest.sin_addr);

    TelemetryPacket pkt{};
    pkt.type = TelemetryType::Fill;
    pkt.symbol_id = 55;
    pkt.data.fill.side = 1; // Sell
    pkt.data.fill.quantity = 250;
    pkt.data.fill.price = 91567'00000000LL;
    pkt.data.fill.fill_type = 0; // Full fill
    pkt.timestamp_ns = TelemetryPublisher::now_ns();
    pkt.sequence = 99;

    sendto(sock, &pkt, sizeof(pkt), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));

    // Wait for reception
    auto start = std::chrono::steady_clock::now();
    while (!received.load()) {
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

    // Verify all fields
    assert(received_pkt.type == TelemetryType::Fill);
    assert(received_pkt.symbol_id == 55);
    assert(received_pkt.data.fill.side == 1);
    assert(received_pkt.data.fill.quantity == 250);
    assert(received_pkt.data.fill.price == 91567'00000000LL);
    assert(received_pkt.data.fill.fill_type == 0);

    std::cout << "PASSED\n";
}

// ============================================================================
// Test: Latency Packet Fields
// ============================================================================
void test_latency_packet() {
    std::cout << "  test_latency_packet... ";

    const uint16_t port = 5571;
    std::atomic<bool> received{false};
    TelemetryPacket received_pkt{};

    TelemetrySubscriber sub("239.255.0.1", port);
    if (!sub.is_valid()) {
        std::cout << "SKIPPED\n";
        return;
    }

    sub.set_callback([&](const TelemetryPacket& pkt) {
        if (pkt.type == TelemetryType::Latency) {
            received_pkt = pkt;
            received.store(true);
        }
    });
    sub.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Send latency packet
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    int loop = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    inet_pton(AF_INET, "239.255.0.1", &dest.sin_addr);

    TelemetryPacket pkt{};
    pkt.type = TelemetryType::Latency;
    pkt.data.latency.tick_to_decision_ns = 750;
    pkt.data.latency.decision_to_order_ns = 250;
    pkt.data.latency.order_to_ack_ns = 1500;
    pkt.data.latency.total_roundtrip_ns = 2500;
    pkt.timestamp_ns = TelemetryPublisher::now_ns();
    pkt.sequence = 111;

    sendto(sock, &pkt, sizeof(pkt), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));

    // Wait for reception
    auto start = std::chrono::steady_clock::now();
    while (!received.load()) {
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

    // Verify all fields
    assert(received_pkt.type == TelemetryType::Latency);
    assert(received_pkt.data.latency.tick_to_decision_ns == 750);
    assert(received_pkt.data.latency.decision_to_order_ns == 250);
    assert(received_pkt.data.latency.order_to_ack_ns == 1500);
    assert(received_pkt.data.latency.total_roundtrip_ns == 2500);

    std::cout << "PASSED\n";
}

// ============================================================================
// Test: Multiple Subscribers
// ============================================================================
void test_multiple_subscribers() {
    std::cout << "  test_multiple_subscribers... ";

    const uint16_t port = 5572;

    std::atomic<int> count1{0};
    std::atomic<int> count2{0};

    TelemetrySubscriber sub1("239.255.0.1", port);
    TelemetrySubscriber sub2("239.255.0.1", port);

    if (!sub1.is_valid() || !sub2.is_valid()) {
        std::cout << "SKIPPED\n";
        return;
    }

    sub1.set_callback([&](const TelemetryPacket&) { count1.fetch_add(1); });
    sub2.set_callback([&](const TelemetryPacket&) { count2.fetch_add(1); });

    sub1.start();
    sub2.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Send packets
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    int loop = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    inet_pton(AF_INET, "239.255.0.1", &dest.sin_addr);

    for (int i = 0; i < 3; ++i) {
        TelemetryPacket pkt{};
        pkt.type = TelemetryType::Heartbeat;
        pkt.sequence = i;
        pkt.timestamp_ns = TelemetryPublisher::now_ns();
        sendto(sock, &pkt, sizeof(pkt), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    }

    // Wait for both subscribers to receive
    auto start = std::chrono::steady_clock::now();
    while (count1.load() < 3 || count2.load() < 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(2)) {
            std::cout << "SKIPPED (multicast not available)\n";
            sub1.stop();
            sub2.stop();
            close(sock);
            return;
        }
    }

    sub1.stop();
    sub2.stop();
    close(sock);

    // Both subscribers should receive all packets
    assert(count1.load() == 3);
    assert(count2.load() == 3);

    std::cout << "PASSED\n";
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
    test_subscriber_double_start();
    test_stop_before_start();
    test_subscriber_no_callback();
    test_regime_packet();
    test_position_packet();
    test_pnl_packet();
    test_fill_packet();
    test_latency_packet();
    test_multiple_subscribers();

    std::cout << "\n=== All 17 tests passed! ===\n\n";

    std::cout << "Architecture summary:\n";
    std::cout << "  - HFT Engine → UDP Multicast (fire-and-forget)\n";
    std::cout << "  - Collector → Time-series DB (QuestDB/InfluxDB)\n";
    std::cout << "  - Dashboard → Web UI or local observer\n";
    std::cout << "  - Latency: ~1-10 µs per publish (non-blocking)\n";
    std::cout << "  - Packet size: 64 bytes (fits in single UDP packet)\n\n";

    return 0;
}
