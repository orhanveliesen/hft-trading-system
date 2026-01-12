#pragma once

/**
 * UDP Telemetry Publisher/Subscriber
 *
 * Production HFT architecture:
 * - HFT engine publishes telemetry via UDP multicast
 * - Fire-and-forget: no blocking, no acknowledgment
 * - Packet loss acceptable (monitoring, not critical)
 * - Multiple subscribers can listen (dashboard, logging, alerting)
 *
 * Usage (Publisher - HFT Engine):
 *   TelemetryPublisher pub("239.255.0.1", 5555);
 *   pub.publish_quote(symbol_id, bid, ask, timestamp);
 *   pub.publish_fill(symbol_id, side, qty, price);
 *
 * Usage (Subscriber - Collector):
 *   TelemetrySubscriber sub("239.255.0.1", 5555);
 *   sub.set_callback([](const TelemetryPacket& pkt) { ... });
 *   sub.start();  // Background thread
 */

#include <cstdint>
#include <cstring>
#include <functional>
#include <thread>
#include <atomic>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

namespace hft {
namespace ipc {

// Packet types
enum class TelemetryType : uint8_t {
    Heartbeat = 0,
    Quote = 1,
    Fill = 2,
    Order = 3,
    Position = 4,
    PnL = 5,
    Regime = 6,
    Risk = 7,
    Latency = 8
};

// Fixed-size packet for network efficiency (64 bytes, fits in one UDP packet)
struct alignas(8) TelemetryPacket {
    uint64_t timestamp_ns;      // 8 bytes - nanosecond timestamp
    uint32_t sequence;          // 4 bytes - packet sequence number
    uint16_t symbol_id;         // 2 bytes - symbol identifier
    TelemetryType type;         // 1 byte  - packet type
    uint8_t flags;              // 1 byte  - additional flags

    union {
        // Quote data (type = Quote)
        struct {
            int64_t bid_price;
            int64_t ask_price;
            uint32_t bid_size;
            uint32_t ask_size;
        } quote;

        // Fill data (type = Fill)
        struct {
            int64_t price;
            uint32_t quantity;
            uint8_t side;       // 0 = Buy, 1 = Sell
            uint8_t fill_type;  // 0 = Full, 1 = Partial
            uint8_t padding[18];
        } fill;

        // Position data (type = Position)
        struct {
            int64_t quantity;   // Scaled by 1e8
            int64_t avg_price;  // Scaled by 1e8
            int64_t market_value;
            int64_t unrealized_pnl;
        } position;

        // PnL data (type = PnL)
        struct {
            int64_t realized_pnl;
            int64_t unrealized_pnl;
            int64_t total_equity;
            uint32_t win_count;
            uint32_t loss_count;
        } pnl;

        // Regime data (type = Regime)
        struct {
            uint8_t regime;     // MarketRegime enum
            uint8_t confidence; // 0-100
            int64_t volatility; // Scaled
            uint8_t padding[22];
        } regime;

        // Latency data (type = Latency)
        struct {
            uint32_t tick_to_decision_ns;
            uint32_t decision_to_order_ns;
            uint32_t order_to_ack_ns;
            uint32_t total_roundtrip_ns;
            uint8_t padding[16];
        } latency;

        // Raw bytes for custom data
        uint8_t raw[40];
    } data;

    // Total: 16 + 40 = 56 bytes, padded to 64
    uint8_t padding[8];
};

static_assert(sizeof(TelemetryPacket) == 64, "TelemetryPacket must be 64 bytes");

/**
 * UDP Multicast Publisher (for HFT engine)
 *
 * Zero-copy, non-blocking, fire-and-forget
 */
class TelemetryPublisher {
public:
    TelemetryPublisher(const char* multicast_addr = "239.255.0.1",
                       uint16_t port = 5555)
        : sequence_(0)
        , socket_(-1)
    {
        socket_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_ < 0) return;

        // Set multicast TTL (1 = local network only)
        int ttl = 1;
        setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

        // Disable loopback (don't receive our own packets)
        int loop = 0;
        setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

        std::memset(&dest_addr_, 0, sizeof(dest_addr_));
        dest_addr_.sin_family = AF_INET;
        dest_addr_.sin_port = htons(port);
        inet_pton(AF_INET, multicast_addr, &dest_addr_.sin_addr);
    }

    ~TelemetryPublisher() {
        if (socket_ >= 0) close(socket_);
    }

    // Non-copyable
    TelemetryPublisher(const TelemetryPublisher&) = delete;
    TelemetryPublisher& operator=(const TelemetryPublisher&) = delete;

    bool is_valid() const { return socket_ >= 0; }

    // Get current timestamp in nanoseconds
    static uint64_t now_ns() {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec * 1'000'000'000ULL + ts.tv_nsec;
    }

    // Low-level publish (for custom packets)
    void publish(TelemetryPacket& pkt) {
        pkt.timestamp_ns = now_ns();
        pkt.sequence = sequence_++;

        // Fire and forget - don't check return value
        sendto(socket_, &pkt, sizeof(pkt), MSG_DONTWAIT,
               reinterpret_cast<sockaddr*>(&dest_addr_), sizeof(dest_addr_));
    }

    // Convenience methods
    void publish_heartbeat() {
        TelemetryPacket pkt{};
        pkt.type = TelemetryType::Heartbeat;
        publish(pkt);
    }

    void publish_quote(uint16_t symbol_id, int64_t bid, int64_t ask,
                       uint32_t bid_size = 0, uint32_t ask_size = 0) {
        TelemetryPacket pkt{};
        pkt.type = TelemetryType::Quote;
        pkt.symbol_id = symbol_id;
        pkt.data.quote.bid_price = bid;
        pkt.data.quote.ask_price = ask;
        pkt.data.quote.bid_size = bid_size;
        pkt.data.quote.ask_size = ask_size;
        publish(pkt);
    }

    void publish_fill(uint16_t symbol_id, bool is_buy, uint32_t qty, int64_t price) {
        TelemetryPacket pkt{};
        pkt.type = TelemetryType::Fill;
        pkt.symbol_id = symbol_id;
        pkt.data.fill.side = is_buy ? 0 : 1;
        pkt.data.fill.quantity = qty;
        pkt.data.fill.price = price;
        publish(pkt);
    }

    void publish_position(uint16_t symbol_id, int64_t qty, int64_t avg_price,
                          int64_t market_value, int64_t unrealized_pnl) {
        TelemetryPacket pkt{};
        pkt.type = TelemetryType::Position;
        pkt.symbol_id = symbol_id;
        pkt.data.position.quantity = qty;
        pkt.data.position.avg_price = avg_price;
        pkt.data.position.market_value = market_value;
        pkt.data.position.unrealized_pnl = unrealized_pnl;
        publish(pkt);
    }

    void publish_pnl(int64_t realized, int64_t unrealized, int64_t equity,
                     uint32_t wins, uint32_t losses) {
        TelemetryPacket pkt{};
        pkt.type = TelemetryType::PnL;
        pkt.data.pnl.realized_pnl = realized;
        pkt.data.pnl.unrealized_pnl = unrealized;
        pkt.data.pnl.total_equity = equity;
        pkt.data.pnl.win_count = wins;
        pkt.data.pnl.loss_count = losses;
        publish(pkt);
    }

    void publish_regime(uint16_t symbol_id, uint8_t regime, uint8_t confidence = 0) {
        TelemetryPacket pkt{};
        pkt.type = TelemetryType::Regime;
        pkt.symbol_id = symbol_id;
        pkt.data.regime.regime = regime;
        pkt.data.regime.confidence = confidence;
        publish(pkt);
    }

    void publish_latency(uint32_t tick_to_decision, uint32_t decision_to_order,
                         uint32_t order_to_ack, uint32_t total) {
        TelemetryPacket pkt{};
        pkt.type = TelemetryType::Latency;
        pkt.data.latency.tick_to_decision_ns = tick_to_decision;
        pkt.data.latency.decision_to_order_ns = decision_to_order;
        pkt.data.latency.order_to_ack_ns = order_to_ack;
        pkt.data.latency.total_roundtrip_ns = total;
        publish(pkt);
    }

private:
    uint32_t sequence_;
    int socket_;
    sockaddr_in dest_addr_;
};

/**
 * UDP Multicast Subscriber (for collector/dashboard)
 *
 * Runs in background thread, calls callback for each packet
 */
class TelemetrySubscriber {
public:
    using Callback = std::function<void(const TelemetryPacket&)>;

    TelemetrySubscriber(const char* multicast_addr = "239.255.0.1",
                        uint16_t port = 5555)
        : running_(false)
        , socket_(-1)
    {
        socket_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_ < 0) return;

        // Allow multiple subscribers on same port
        int reuse = 1;
        setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        // Bind to port
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(socket_);
            socket_ = -1;
            return;
        }

        // Join multicast group
        ip_mreq mreq{};
        inet_pton(AF_INET, multicast_addr, &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = INADDR_ANY;

        if (setsockopt(socket_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       &mreq, sizeof(mreq)) < 0) {
            close(socket_);
            socket_ = -1;
            return;
        }

        // Set receive timeout (for clean shutdown)
        timeval tv{};
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    ~TelemetrySubscriber() {
        stop();
        if (socket_ >= 0) close(socket_);
    }

    bool is_valid() const { return socket_ >= 0; }

    void set_callback(Callback cb) { callback_ = std::move(cb); }

    void start() {
        if (!is_valid() || running_) return;
        running_ = true;
        thread_ = std::thread(&TelemetrySubscriber::run, this);
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

    // Statistics
    uint64_t packets_received() const { return packets_received_; }
    uint64_t packets_dropped() const { return packets_dropped_; }

private:
    void run() {
        TelemetryPacket pkt;
        uint32_t last_seq = 0;
        bool first_packet = true;

        while (running_) {
            ssize_t n = recv(socket_, &pkt, sizeof(pkt), 0);

            if (n == sizeof(pkt)) {
                packets_received_++;

                // Check for dropped packets
                if (!first_packet && pkt.sequence != last_seq + 1) {
                    uint32_t dropped = pkt.sequence - last_seq - 1;
                    packets_dropped_ += dropped;
                }
                last_seq = pkt.sequence;
                first_packet = false;

                if (callback_) callback_(pkt);
            }
        }
    }

    std::atomic<bool> running_;
    int socket_;
    Callback callback_;
    std::thread thread_;
    std::atomic<uint64_t> packets_received_{0};
    std::atomic<uint64_t> packets_dropped_{0};
};

}  // namespace ipc
}  // namespace hft
