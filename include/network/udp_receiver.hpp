#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <functional>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

namespace hft {
namespace network {

// MoldUDP64 header structure (NASDAQ market data transport)
struct MoldUDP64Header {
    char session[10];
    uint64_t sequence_number;
    uint16_t message_count;
};

// Parse MoldUDP64 header from raw bytes (big-endian)
inline MoldUDP64Header parse_moldudp_header(const uint8_t* data) {
    MoldUDP64Header header;
    std::memcpy(header.session, data, 10);

    // Sequence number: big-endian uint64 at offset 10
    header.sequence_number =
        (static_cast<uint64_t>(data[10]) << 56) |
        (static_cast<uint64_t>(data[11]) << 48) |
        (static_cast<uint64_t>(data[12]) << 40) |
        (static_cast<uint64_t>(data[13]) << 32) |
        (static_cast<uint64_t>(data[14]) << 24) |
        (static_cast<uint64_t>(data[15]) << 16) |
        (static_cast<uint64_t>(data[16]) << 8) |
        data[17];

    // Message count: big-endian uint16 at offset 18
    header.message_count = (static_cast<uint16_t>(data[18]) << 8) | data[19];

    return header;
}

// UDP receiver configuration
struct UdpConfig {
    std::string interface;        // Local interface IP
    std::string multicast_group;  // Multicast group IP
    uint16_t port;
    int recv_buffer_size;         // SO_RCVBUF size
};

// High-performance UDP multicast receiver using epoll
class UdpReceiver {
public:
    static constexpr size_t MAX_PACKET_SIZE = 1500;
    static constexpr int MAX_EVENTS = 16;

    UdpReceiver() : socket_fd_(-1), epoll_fd_(-1), running_(false) {}

    ~UdpReceiver() {
        stop();
    }

    // Initialize socket and join multicast group
    bool init(const UdpConfig& config) {
        config_ = config;

        // Create UDP socket
        socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_fd_ < 0) return false;

        // Set non-blocking
        int flags = fcntl(socket_fd_, F_GETFL, 0);
        fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK);

        // Set receive buffer size
        setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF,
                   &config.recv_buffer_size, sizeof(config.recv_buffer_size));

        // Allow address reuse
        int reuse = 1;
        setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        // Bind to port
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(config.port);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }

        // Join multicast group
        ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = inet_addr(config.multicast_group.c_str());
        mreq.imr_interface.s_addr = inet_addr(config.interface.c_str());

        if (setsockopt(socket_fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       &mreq, sizeof(mreq)) < 0) {
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }

        // Create epoll instance
        epoll_fd_ = epoll_create1(0);
        if (epoll_fd_ < 0) {
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }

        // Add socket to epoll
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;  // Edge-triggered
        ev.data.fd = socket_fd_;

        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, socket_fd_, &ev) < 0) {
            close(epoll_fd_);
            close(socket_fd_);
            epoll_fd_ = -1;
            socket_fd_ = -1;
            return false;
        }

        return true;
    }

    // Poll for packets with timeout (microseconds)
    // Calls callback for each received packet
    template <typename Callback>
    int poll(Callback&& callback, int timeout_us = 0) {
        if (socket_fd_ < 0) return -1;

        epoll_event events[MAX_EVENTS];
        int timeout_ms = timeout_us / 1000;

        int nfds = epoll_wait(epoll_fd_, events, MAX_EVENTS, timeout_ms);

        int packets_received = 0;

        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == socket_fd_) {
                // Read all available packets (edge-triggered)
                while (true) {
                    ssize_t len = recv(socket_fd_, recv_buffer_, MAX_PACKET_SIZE, 0);
                    if (len <= 0) break;

                    callback(recv_buffer_, static_cast<size_t>(len));
                    ++packets_received;
                }
            }
        }

        return packets_received;
    }

    void stop() {
        running_ = false;
        if (epoll_fd_ >= 0) {
            close(epoll_fd_);
            epoll_fd_ = -1;
        }
        if (socket_fd_ >= 0) {
            close(socket_fd_);
            socket_fd_ = -1;
        }
    }

    bool is_initialized() const { return socket_fd_ >= 0; }

private:
    int socket_fd_;
    int epoll_fd_;
    bool running_;
    UdpConfig config_;
    uint8_t recv_buffer_[MAX_PACKET_SIZE];
};

}  // namespace network
}  // namespace hft
