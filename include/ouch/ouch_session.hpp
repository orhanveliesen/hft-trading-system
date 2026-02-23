#pragma once

#include "../types.hpp"
#include "ouch_messages.hpp"

#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace hft {
namespace ouch {

/**
 * SoupBinTCP Protocol
 *
 * Framing protocol used by OUCH.
 * Each message has a 2-byte big-endian length prefix + 1-byte packet type.
 *
 * Packet types:
 *   '+' Debug packet (client/server)
 *   'A' Login Accepted (server)
 *   'J' Login Rejected (server)
 *   'S' Sequenced Data (server)
 *   'H' Server Heartbeat (server)
 *   'Z' End of Session (server)
 *   'L' Login Request (client)
 *   'U' Unsequenced Data (client)
 *   'O' Logout Request (client)
 *   'R' Client Heartbeat (client)
 */

// SoupBinTCP packet types
constexpr char SOUP_DEBUG = '+';
constexpr char SOUP_LOGIN_ACCEPTED = 'A';
constexpr char SOUP_LOGIN_REJECTED = 'J';
constexpr char SOUP_SEQUENCED_DATA = 'S';
constexpr char SOUP_SERVER_HEARTBEAT = 'H';
constexpr char SOUP_END_OF_SESSION = 'Z';
constexpr char SOUP_LOGIN_REQUEST = 'L';
constexpr char SOUP_UNSEQUENCED_DATA = 'U';
constexpr char SOUP_LOGOUT_REQUEST = 'O';
constexpr char SOUP_CLIENT_HEARTBEAT = 'R';

// Login reject reasons
constexpr char LOGIN_REJECT_NOT_AUTHORIZED = 'A';
constexpr char LOGIN_REJECT_SESSION_UNAVAILABLE = 'S';

/**
 * Login Request Packet
 */
#pragma pack(push, 1)
struct LoginRequest {
    char packet_type;            // 1:  'L'
    char username[6];            // 6:  Username (space-padded)
    char password[10];           // 10: Password (space-padded)
    char requested_session[10];  // 10: Session ID (space-padded, blank for any)
    char requested_sequence[20]; // 20: Sequence number (space-padded, 0 or blank for next)
    // Total: 47 bytes

    void init() {
        packet_type = SOUP_LOGIN_REQUEST;
        std::memset(username, ' ', sizeof(username));
        std::memset(password, ' ', sizeof(password));
        std::memset(requested_session, ' ', sizeof(requested_session));
        std::memset(requested_sequence, ' ', sizeof(requested_sequence));
    }

    void set_username(const char* u) {
        std::memset(username, ' ', sizeof(username));
        std::memcpy(username, u, std::min(std::strlen(u), sizeof(username)));
    }

    void set_password(const char* p) {
        std::memset(password, ' ', sizeof(password));
        std::memcpy(password, p, std::min(std::strlen(p), sizeof(password)));
    }
};
static_assert(sizeof(LoginRequest) == 47, "LoginRequest must be 47 bytes");
#pragma pack(pop)

/**
 * Login Accepted Packet
 */
#pragma pack(push, 1)
struct LoginAccepted {
    char packet_type;         // 1:  'A'
    char session[10];         // 10: Session ID
    char sequence_number[20]; // 20: Next sequence number
    // Total: 31 bytes
};
static_assert(sizeof(LoginAccepted) == 31, "LoginAccepted must be 31 bytes");
#pragma pack(pop)

/**
 * Login Rejected Packet
 */
#pragma pack(push, 1)
struct LoginRejected {
    char packet_type; // 1:  'J'
    char reason;      // 1:  Reject reason
    // Total: 2 bytes
};
static_assert(sizeof(LoginRejected) == 2, "LoginRejected must be 2 bytes");
#pragma pack(pop)

/**
 * Session configuration
 */
struct OuchSessionConfig {
    const char* host = "127.0.0.1";
    uint16_t port = 15000;
    const char* username = "";
    const char* password = "";
    const char* firm = "TEST";
    uint32_t heartbeat_interval_ms = 1000;
    uint32_t connect_timeout_ms = 5000;
    bool tcp_nodelay = true;
};

/**
 * Session state
 */
enum class SessionState : uint8_t { Disconnected = 0, Connecting, LoggingIn, LoggedIn, Disconnecting };

/**
 * OUCH Session Handler
 *
 * Manages TCP connection with SoupBinTCP framing.
 * Provides low-level send/receive for OUCH messages.
 *
 * Usage:
 *   OuchSession session(config);
 *   session.connect();
 *   session.send_order(order);
 *   // ... in event loop:
 *   session.process_incoming();
 */
class OuchSession {
public:
    // Callback types for responses
    using AcceptedCallback = std::function<void(const Accepted&)>;
    using ExecutedCallback = std::function<void(const Executed&)>;
    using CanceledCallback = std::function<void(const Canceled&)>;
    using RejectedCallback = std::function<void(const Rejected&)>;
    using ReplacedCallback = std::function<void(const Replaced&)>;

    explicit OuchSession(const OuchSessionConfig& config)
        : config_(config), socket_fd_(-1), state_(SessionState::Disconnected), next_token_id_(1), bytes_sent_(0),
          bytes_received_(0), messages_sent_(0), messages_received_(0) {
        std::memset(recv_buffer_.data(), 0, recv_buffer_.size());
        recv_pos_ = 0;
    }

    ~OuchSession() { disconnect(); }

    // Non-copyable
    OuchSession(const OuchSession&) = delete;
    OuchSession& operator=(const OuchSession&) = delete;

    // Connect to OUCH server
    bool connect() {
        if (state_ != SessionState::Disconnected) {
            return false;
        }

        state_ = SessionState::Connecting;

        // Create socket
        socket_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_fd_ < 0) {
            state_ = SessionState::Disconnected;
            return false;
        }

        // Set TCP_NODELAY for low latency
        if (config_.tcp_nodelay) {
            int flag = 1;
            setsockopt(socket_fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        }

        // Connect
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(config_.port);
        inet_pton(AF_INET, config_.host, &addr.sin_addr);

        if (::connect(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(socket_fd_);
            socket_fd_ = -1;
            state_ = SessionState::Disconnected;
            return false;
        }

        state_ = SessionState::LoggingIn;

        // Send login request
        LoginRequest login;
        login.init();
        login.set_username(config_.username);
        login.set_password(config_.password);

        if (!send_packet(SOUP_LOGIN_REQUEST, &login.username, sizeof(LoginRequest) - 1)) {
            disconnect();
            return false;
        }

        return true;
    }

    // Disconnect from server
    void disconnect() {
        if (socket_fd_ >= 0) {
            // Send logout request
            if (state_ == SessionState::LoggedIn) {
                state_ = SessionState::Disconnecting;
                send_packet(SOUP_LOGOUT_REQUEST, nullptr, 0);
            }

            close(socket_fd_);
            socket_fd_ = -1;
        }
        state_ = SessionState::Disconnected;
    }

    // Send Enter Order
    bool send_enter_order(const EnterOrder& order) {
        if (state_ != SessionState::LoggedIn) {
            return false;
        }
        return send_ouch_message(&order, sizeof(order));
    }

    // Send Cancel Order
    bool send_cancel_order(const CancelOrder& cancel) {
        if (state_ != SessionState::LoggedIn) {
            return false;
        }
        return send_ouch_message(&cancel, sizeof(cancel));
    }

    // Send Replace Order
    bool send_replace_order(const ReplaceOrder& replace) {
        if (state_ != SessionState::LoggedIn) {
            return false;
        }
        return send_ouch_message(&replace, sizeof(replace));
    }

    // Send heartbeat (should be called periodically)
    bool send_heartbeat() { return send_packet(SOUP_CLIENT_HEARTBEAT, nullptr, 0); }

    // Process incoming data (call in event loop)
    // Returns number of messages processed
    int process_incoming() {
        if (socket_fd_ < 0) {
            return -1;
        }

        // Read available data
        ssize_t bytes =
            recv(socket_fd_, recv_buffer_.data() + recv_pos_, recv_buffer_.size() - recv_pos_, MSG_DONTWAIT);

        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0; // No data available
            }
            return -1; // Error
        }

        if (bytes == 0) {
            // Connection closed
            disconnect();
            return -1;
        }

        recv_pos_ += bytes;
        bytes_received_ += bytes;

        // Process complete messages
        int count = 0;
        size_t offset = 0;

        while (offset + 3 <= recv_pos_) {
            // Read 2-byte length prefix
            uint16_t msg_len = read_be16(reinterpret_cast<const uint8_t*>(recv_buffer_.data() + offset));

            // Check if we have complete message
            if (offset + 2 + msg_len > recv_pos_) {
                break; // Need more data
            }

            // Process message
            const char* msg_data = recv_buffer_.data() + offset + 2;
            process_soup_packet(msg_data[0], msg_data + 1, msg_len - 1);

            offset += 2 + msg_len;
            ++count;
            ++messages_received_;
        }

        // Compact buffer
        if (offset > 0) {
            std::memmove(recv_buffer_.data(), recv_buffer_.data() + offset, recv_pos_ - offset);
            recv_pos_ -= offset;
        }

        return count;
    }

    // Generate unique order token
    void generate_token(char* token, size_t size = 14) {
        uint64_t id = next_token_id_.fetch_add(1, std::memory_order_relaxed);
        std::snprintf(token, size + 1, "%014lu", id);
    }

    // Set callbacks
    void set_accepted_callback(AcceptedCallback cb) { on_accepted_ = std::move(cb); }
    void set_executed_callback(ExecutedCallback cb) { on_executed_ = std::move(cb); }
    void set_canceled_callback(CanceledCallback cb) { on_canceled_ = std::move(cb); }
    void set_rejected_callback(RejectedCallback cb) { on_rejected_ = std::move(cb); }
    void set_replaced_callback(ReplacedCallback cb) { on_replaced_ = std::move(cb); }

    // State queries
    SessionState state() const { return state_; }
    bool is_connected() const { return state_ == SessionState::LoggedIn; }
    int socket_fd() const { return socket_fd_; }

    // Statistics
    uint64_t bytes_sent() const { return bytes_sent_; }
    uint64_t bytes_received() const { return bytes_received_; }
    uint64_t messages_sent() const { return messages_sent_; }
    uint64_t messages_received() const { return messages_received_; }

    const OuchSessionConfig& config() const { return config_; }

private:
    // Send SoupBinTCP packet
    bool send_packet(char packet_type, const void* data, size_t len) {
        // Build packet: 2-byte length + packet type + data
        uint8_t header[3];
        uint16_t total_len = static_cast<uint16_t>(1 + len); // packet type + data
        write_be16(header, total_len);
        header[2] = static_cast<uint8_t>(packet_type);

        // Send header
        if (send(socket_fd_, header, 3, MSG_NOSIGNAL) != 3) {
            return false;
        }

        // Send data
        if (len > 0 && data != nullptr) {
            if (send(socket_fd_, data, len, MSG_NOSIGNAL) != static_cast<ssize_t>(len)) {
                return false;
            }
        }

        bytes_sent_ += 3 + len;
        ++messages_sent_;
        return true;
    }

    // Send OUCH message (wrapped in Unsequenced Data packet)
    bool send_ouch_message(const void* msg, size_t len) { return send_packet(SOUP_UNSEQUENCED_DATA, msg, len); }

    // Process received SoupBinTCP packet
    void process_soup_packet(char packet_type, const char* data, size_t len) {
        switch (packet_type) {
        case SOUP_LOGIN_ACCEPTED:
            state_ = SessionState::LoggedIn;
            break;

        case SOUP_LOGIN_REJECTED:
            state_ = SessionState::Disconnected;
            break;

        case SOUP_SERVER_HEARTBEAT:
            // Nothing to do
            break;

        case SOUP_END_OF_SESSION:
            disconnect();
            break;

        case SOUP_SEQUENCED_DATA:
            if (len > 0) {
                process_ouch_message(data, len);
            }
            break;

        default:
            // Unknown packet type
            break;
        }
    }

    // Process OUCH message
    void process_ouch_message(const char* data, size_t len) {
        if (len == 0)
            return;

        char msg_type = data[0];

        switch (msg_type) {
        case MSG_ACCEPTED:
            if (len >= sizeof(Accepted) && on_accepted_) {
                on_accepted_(*reinterpret_cast<const Accepted*>(data));
            }
            break;

        case MSG_EXECUTED:
            if (len >= sizeof(Executed) && on_executed_) {
                on_executed_(*reinterpret_cast<const Executed*>(data));
            }
            break;

        case MSG_CANCELED:
            if (len >= sizeof(Canceled) && on_canceled_) {
                on_canceled_(*reinterpret_cast<const Canceled*>(data));
            }
            break;

        case MSG_REJECTED:
            if (len >= sizeof(Rejected) && on_rejected_) {
                on_rejected_(*reinterpret_cast<const Rejected*>(data));
            }
            break;

        case MSG_REPLACED:
            if (len >= sizeof(Replaced) && on_replaced_) {
                on_replaced_(*reinterpret_cast<const Replaced*>(data));
            }
            break;

        case MSG_SYSTEM_EVENT:
            // Handle system events if needed
            break;

        default:
            // Unknown message type
            break;
        }
    }

    OuchSessionConfig config_;
    int socket_fd_;
    SessionState state_;

    // Receive buffer
    std::array<char, 65536> recv_buffer_;
    size_t recv_pos_;

    // Token generation
    std::atomic<uint64_t> next_token_id_;

    // Callbacks
    AcceptedCallback on_accepted_;
    ExecutedCallback on_executed_;
    CanceledCallback on_canceled_;
    RejectedCallback on_rejected_;
    ReplacedCallback on_replaced_;

    // Statistics
    uint64_t bytes_sent_;
    uint64_t bytes_received_;
    uint64_t messages_sent_;
    uint64_t messages_received_;
};

} // namespace ouch
} // namespace hft
