#pragma once

#include <cstdint>
#include <cstring>

namespace hft {
namespace itch {

// ITCH 5.0 Message Types
constexpr char MSG_ADD_ORDER = 'A';
constexpr char MSG_ADD_ORDER_MPID = 'F';
constexpr char MSG_ORDER_EXECUTED = 'E';
constexpr char MSG_ORDER_EXECUTED_PRICE = 'C';
constexpr char MSG_ORDER_CANCEL = 'X';
constexpr char MSG_ORDER_DELETE = 'D';
constexpr char MSG_ORDER_REPLACE = 'U';
constexpr char MSG_TRADE = 'P';

// Parsed message structures
struct AddOrder {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp;
    uint64_t order_ref;
    char side;
    uint32_t shares;
    char stock[9]; // 8 chars + null terminator
    uint32_t price;
};

struct OrderExecuted {
    uint16_t stock_locate;
    uint64_t order_ref;
    uint32_t executed_shares;
    uint64_t match_number;
};

struct OrderCancel {
    uint16_t stock_locate;
    uint64_t order_ref;
    uint32_t cancelled_shares;
};

struct OrderDelete {
    uint16_t stock_locate;
    uint64_t order_ref;
};

struct OrderReplace {
    uint16_t stock_locate;
    uint64_t original_order_ref;
    uint64_t new_order_ref;
    uint32_t shares;
    uint32_t price;
};

// Big-endian parsing utilities (inline for hot path)
inline uint16_t read_be16(const uint8_t* buf) {
    return (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
}

inline uint32_t read_be32(const uint8_t* buf) {
    return (static_cast<uint32_t>(buf[0]) << 24) | (static_cast<uint32_t>(buf[1]) << 16) |
           (static_cast<uint32_t>(buf[2]) << 8) | buf[3];
}

inline uint64_t read_be48(const uint8_t* buf) {
    return (static_cast<uint64_t>(buf[0]) << 40) | (static_cast<uint64_t>(buf[1]) << 32) |
           (static_cast<uint64_t>(buf[2]) << 24) | (static_cast<uint64_t>(buf[3]) << 16) |
           (static_cast<uint64_t>(buf[4]) << 8) | buf[5];
}

inline uint64_t read_be64(const uint8_t* buf) {
    return (static_cast<uint64_t>(buf[0]) << 56) | (static_cast<uint64_t>(buf[1]) << 48) |
           (static_cast<uint64_t>(buf[2]) << 40) | (static_cast<uint64_t>(buf[3]) << 32) |
           (static_cast<uint64_t>(buf[4]) << 24) | (static_cast<uint64_t>(buf[5]) << 16) |
           (static_cast<uint64_t>(buf[6]) << 8) | buf[7];
}

} // namespace itch
} // namespace hft
