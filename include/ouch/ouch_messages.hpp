#pragma once

#include <cstdint>
#include <cstring>

namespace hft {
namespace ouch {

/**
 * OUCH 4.2 Protocol Messages
 *
 * Binary protocol for order entry (counterpart to ITCH for market data).
 * Used by NASDAQ, BIST, and other exchanges.
 *
 * Features:
 *   - Big-endian byte ordering
 *   - Fixed-size messages for predictable latency
 *   - SoupBinTCP framing (2-byte length prefix)
 *
 * References:
 *   - NASDAQ OUCH 4.2: https://www.nasdaqtrader.com/content/technicalsupport/specifications/TradingProducts/OUCH4.2.pdf
 *   - BIST OUCH: Similar structure with minor variations
 */

// ============================================
// Constants
// ============================================

// Message type identifiers (outbound: client -> exchange)
constexpr char MSG_ENTER_ORDER       = 'O';
constexpr char MSG_REPLACE_ORDER     = 'U';
constexpr char MSG_CANCEL_ORDER      = 'X';
constexpr char MSG_MODIFY_ORDER      = 'M';

// Message type identifiers (inbound: exchange -> client)
constexpr char MSG_SYSTEM_EVENT      = 'S';
constexpr char MSG_ACCEPTED          = 'A';
constexpr char MSG_REPLACED          = 'U';
constexpr char MSG_CANCELED          = 'C';
constexpr char MSG_AIQ_CANCELED      = 'D';
constexpr char MSG_EXECUTED          = 'E';
constexpr char MSG_BROKEN_TRADE      = 'B';
constexpr char MSG_REJECTED          = 'J';
constexpr char MSG_CANCEL_PENDING    = 'P';
constexpr char MSG_CANCEL_REJECT     = 'I';
constexpr char MSG_PRIORITY_UPDATE   = 'T';

// Side indicators
constexpr char SIDE_BUY  = 'B';
constexpr char SIDE_SELL = 'S';
constexpr char SIDE_SHORT = 'T';
constexpr char SIDE_SHORT_EXEMPT = 'E';

// Time in Force
constexpr uint32_t TIF_DAY = 0;           // Day order
constexpr uint32_t TIF_IOC = 99998;       // Immediate or Cancel
constexpr uint32_t TIF_GTX = 99999;       // Good till extended (market hours)

// Display types
constexpr char DISPLAY_VISIBLE = 'Y';
constexpr char DISPLAY_HIDDEN  = 'N';
constexpr char DISPLAY_POST_ONLY = 'P';
constexpr char DISPLAY_IMBALANCE_ONLY = 'I';
constexpr char DISPLAY_MIDPOINT = 'M';

// Capacity (NASDAQ specific)
constexpr char CAPACITY_AGENCY = 'A';
constexpr char CAPACITY_PRINCIPAL = 'P';
constexpr char CAPACITY_RISKLESS = 'R';
constexpr char CAPACITY_OTHER = 'O';

// Order state (for responses)
constexpr char ORDER_STATE_LIVE = 'L';
constexpr char ORDER_STATE_DEAD = 'D';

// Token size (configurable for different exchanges)
constexpr size_t NASDAQ_TOKEN_SIZE = 14;
constexpr size_t BIST_TOKEN_SIZE = 14;  // Verify with BIST spec
constexpr size_t DEFAULT_TOKEN_SIZE = 14;

// Stock symbol size
constexpr size_t STOCK_SIZE = 8;

// ============================================
// Big-Endian Write Utilities
// ============================================

inline void write_be16(uint8_t* buf, uint16_t val) {
    buf[0] = static_cast<uint8_t>(val >> 8);
    buf[1] = static_cast<uint8_t>(val);
}

inline void write_be32(uint8_t* buf, uint32_t val) {
    buf[0] = static_cast<uint8_t>(val >> 24);
    buf[1] = static_cast<uint8_t>(val >> 16);
    buf[2] = static_cast<uint8_t>(val >> 8);
    buf[3] = static_cast<uint8_t>(val);
}

inline void write_be64(uint8_t* buf, uint64_t val) {
    buf[0] = static_cast<uint8_t>(val >> 56);
    buf[1] = static_cast<uint8_t>(val >> 48);
    buf[2] = static_cast<uint8_t>(val >> 40);
    buf[3] = static_cast<uint8_t>(val >> 32);
    buf[4] = static_cast<uint8_t>(val >> 24);
    buf[5] = static_cast<uint8_t>(val >> 16);
    buf[6] = static_cast<uint8_t>(val >> 8);
    buf[7] = static_cast<uint8_t>(val);
}

// Big-Endian Read Utilities (from ITCH, for response parsing)
inline uint16_t read_be16(const uint8_t* buf) {
    return (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
}

inline uint32_t read_be32(const uint8_t* buf) {
    return (static_cast<uint32_t>(buf[0]) << 24) |
           (static_cast<uint32_t>(buf[1]) << 16) |
           (static_cast<uint32_t>(buf[2]) << 8) |
           buf[3];
}

inline uint64_t read_be64(const uint8_t* buf) {
    return (static_cast<uint64_t>(buf[0]) << 56) |
           (static_cast<uint64_t>(buf[1]) << 48) |
           (static_cast<uint64_t>(buf[2]) << 40) |
           (static_cast<uint64_t>(buf[3]) << 32) |
           (static_cast<uint64_t>(buf[4]) << 24) |
           (static_cast<uint64_t>(buf[5]) << 16) |
           (static_cast<uint64_t>(buf[6]) << 8) |
           buf[7];
}

// ============================================
// Outbound Messages (Client -> Exchange)
// ============================================

/**
 * Enter Order Message ('O')
 * Size: 48 bytes
 */
#pragma pack(push, 1)
struct EnterOrder {
    char type;                    // 1:  'O'
    char token[14];               // 14: Client order token
    char side;                    // 1:  'B' or 'S'
    uint32_t quantity;            // 4:  Number of shares (big-endian)
    char stock[8];                // 8:  Stock symbol (space-padded)
    uint32_t price;               // 4:  Price (4 decimal places, big-endian)
    uint32_t time_in_force;       // 4:  TIF (big-endian)
    char firm[4];                 // 4:  Firm identifier
    char display;                 // 1:  Display type
    char capacity;                // 1:  Order capacity
    char intermarket_sweep;       // 1:  'Y' or 'N'
    uint32_t min_quantity;        // 4:  Minimum quantity (big-endian)
    char cross_type;              // 1:  Cross type
    // Total: 48 bytes

    void init() {
        std::memset(this, ' ', sizeof(*this));
        type = MSG_ENTER_ORDER;
        display = DISPLAY_VISIBLE;
        capacity = CAPACITY_AGENCY;
        intermarket_sweep = 'N';
        cross_type = 'N';
    }

    void set_token(const char* t) {
        std::memset(token, ' ', sizeof(token));
        std::memcpy(token, t, std::min(std::strlen(t), sizeof(token)));
    }

    void set_stock(const char* s) {
        std::memset(stock, ' ', sizeof(stock));
        std::memcpy(stock, s, std::min(std::strlen(s), sizeof(stock)));
    }

    void set_firm(const char* f) {
        std::memset(firm, ' ', sizeof(firm));
        std::memcpy(firm, f, std::min(std::strlen(f), sizeof(firm)));
    }

    void set_quantity(uint32_t qty) {
        write_be32(reinterpret_cast<uint8_t*>(&quantity), qty);
    }

    void set_price(uint32_t p) {
        write_be32(reinterpret_cast<uint8_t*>(&price), p);
    }

    void set_time_in_force(uint32_t tif) {
        write_be32(reinterpret_cast<uint8_t*>(&time_in_force), tif);
    }

    void set_min_quantity(uint32_t min_qty) {
        write_be32(reinterpret_cast<uint8_t*>(&min_quantity), min_qty);
    }
};
static_assert(sizeof(EnterOrder) == 48, "EnterOrder must be 48 bytes");
#pragma pack(pop)

/**
 * Replace Order Message ('U')
 * Size: 47 bytes
 */
#pragma pack(push, 1)
struct ReplaceOrder {
    char type;                    // 1:  'U'
    char existing_token[14];      // 14: Token of order to replace
    char replacement_token[14];   // 14: New order token
    uint32_t quantity;            // 4:  New quantity (big-endian)
    uint32_t price;               // 4:  New price (big-endian)
    uint32_t time_in_force;       // 4:  TIF (big-endian)
    char display;                 // 1:  Display type
    char intermarket_sweep;       // 1:  'Y' or 'N'
    uint32_t min_quantity;        // 4:  Minimum quantity (big-endian)
    // Total: 47 bytes

    void init() {
        std::memset(this, ' ', sizeof(*this));
        type = MSG_REPLACE_ORDER;
        display = DISPLAY_VISIBLE;
        intermarket_sweep = 'N';
    }

    void set_existing_token(const char* t) {
        std::memset(existing_token, ' ', sizeof(existing_token));
        std::memcpy(existing_token, t, std::min(std::strlen(t), sizeof(existing_token)));
    }

    void set_replacement_token(const char* t) {
        std::memset(replacement_token, ' ', sizeof(replacement_token));
        std::memcpy(replacement_token, t, std::min(std::strlen(t), sizeof(replacement_token)));
    }

    void set_quantity(uint32_t qty) {
        write_be32(reinterpret_cast<uint8_t*>(&quantity), qty);
    }

    void set_price(uint32_t p) {
        write_be32(reinterpret_cast<uint8_t*>(&price), p);
    }

    void set_time_in_force(uint32_t tif) {
        write_be32(reinterpret_cast<uint8_t*>(&time_in_force), tif);
    }

    void set_min_quantity(uint32_t min_qty) {
        write_be32(reinterpret_cast<uint8_t*>(&min_quantity), min_qty);
    }
};
static_assert(sizeof(ReplaceOrder) == 47, "ReplaceOrder must be 47 bytes");
#pragma pack(pop)

/**
 * Cancel Order Message ('X')
 * Size: 19 bytes
 */
#pragma pack(push, 1)
struct CancelOrder {
    char type;                    // 1:  'X'
    char token[14];               // 14: Token of order to cancel
    uint32_t quantity;            // 4:  Quantity to cancel (0 = full cancel)
    // Total: 19 bytes

    void init() {
        type = MSG_CANCEL_ORDER;
        std::memset(token, ' ', sizeof(token));
        quantity = 0;
    }

    void set_token(const char* t) {
        std::memset(token, ' ', sizeof(token));
        std::memcpy(token, t, std::min(std::strlen(t), sizeof(token)));
    }

    void set_quantity(uint32_t qty) {
        write_be32(reinterpret_cast<uint8_t*>(&quantity), qty);
    }
};
static_assert(sizeof(CancelOrder) == 19, "CancelOrder must be 19 bytes");
#pragma pack(pop)

/**
 * Modify Order Message ('M')
 * Size: 20 bytes
 */
#pragma pack(push, 1)
struct ModifyOrder {
    char type;                    // 1:  'M'
    char token[14];               // 14: Token of order to modify
    char side;                    // 1:  New side
    uint32_t quantity;            // 4:  New quantity (big-endian)
    // Total: 20 bytes

    void init() {
        type = MSG_MODIFY_ORDER;
        std::memset(token, ' ', sizeof(token));
        side = ' ';
    }

    void set_token(const char* t) {
        std::memset(token, ' ', sizeof(token));
        std::memcpy(token, t, std::min(std::strlen(t), sizeof(token)));
    }

    void set_quantity(uint32_t qty) {
        write_be32(reinterpret_cast<uint8_t*>(&quantity), qty);
    }
};
static_assert(sizeof(ModifyOrder) == 20, "ModifyOrder must be 20 bytes");
#pragma pack(pop)

// ============================================
// Inbound Messages (Exchange -> Client)
// ============================================

/**
 * Accepted Message ('A')
 * Size: 66 bytes
 */
#pragma pack(push, 1)
struct Accepted {
    char type;                    // 1:  'A'
    uint64_t timestamp;           // 8:  Nanoseconds since midnight
    char token[14];               // 14: Order token
    char side;                    // 1:  Side
    uint32_t quantity;            // 4:  Shares
    char stock[8];                // 8:  Stock symbol
    uint32_t price;               // 4:  Price
    uint32_t time_in_force;       // 4:  TIF
    char firm[4];                 // 4:  Firm
    char display;                 // 1:  Display
    uint64_t order_ref;           // 8:  Exchange order reference
    char capacity;                // 1:  Capacity
    char intermarket_sweep;       // 1:  ISO flag
    uint32_t min_quantity;        // 4:  Min quantity
    char cross_type;              // 1:  Cross type
    char order_state;             // 1:  'L' = Live
    char bbo_weight;              // 1:  BBO weight indicator
    // Total: 66 bytes

    uint64_t get_timestamp() const { return read_be64(reinterpret_cast<const uint8_t*>(&timestamp)); }
    uint32_t get_quantity() const { return read_be32(reinterpret_cast<const uint8_t*>(&quantity)); }
    uint32_t get_price() const { return read_be32(reinterpret_cast<const uint8_t*>(&price)); }
    uint64_t get_order_ref() const { return read_be64(reinterpret_cast<const uint8_t*>(&order_ref)); }
};
static_assert(sizeof(Accepted) == 66, "Accepted must be 66 bytes");
#pragma pack(pop)

/**
 * Executed Message ('E')
 * Size: 40 bytes
 */
#pragma pack(push, 1)
struct Executed {
    char type;                    // 1:  'E'
    uint64_t timestamp;           // 8:  Nanoseconds since midnight
    char token[14];               // 14: Order token
    uint32_t executed_quantity;   // 4:  Executed shares
    uint32_t execution_price;     // 4:  Execution price
    char liquidity_flag;          // 1:  Liquidity indicator
    uint64_t match_number;        // 8:  Match number
    // Total: 40 bytes

    uint64_t get_timestamp() const { return read_be64(reinterpret_cast<const uint8_t*>(&timestamp)); }
    uint32_t get_executed_quantity() const { return read_be32(reinterpret_cast<const uint8_t*>(&executed_quantity)); }
    uint32_t get_execution_price() const { return read_be32(reinterpret_cast<const uint8_t*>(&execution_price)); }
    uint64_t get_match_number() const { return read_be64(reinterpret_cast<const uint8_t*>(&match_number)); }
};
static_assert(sizeof(Executed) == 40, "Executed must be 40 bytes");
#pragma pack(pop)

/**
 * Canceled Message ('C')
 * Size: 28 bytes
 */
#pragma pack(push, 1)
struct Canceled {
    char type;                    // 1:  'C'
    uint64_t timestamp;           // 8:  Nanoseconds since midnight
    char token[14];               // 14: Order token
    uint32_t decrement_quantity;  // 4:  Quantity canceled
    char reason;                  // 1:  Cancel reason
    // Total: 28 bytes

    uint64_t get_timestamp() const { return read_be64(reinterpret_cast<const uint8_t*>(&timestamp)); }
    uint32_t get_decrement_quantity() const { return read_be32(reinterpret_cast<const uint8_t*>(&decrement_quantity)); }
};
static_assert(sizeof(Canceled) == 28, "Canceled must be 28 bytes");
#pragma pack(pop)

/**
 * Rejected Message ('J')
 * Size: 24 bytes
 */
#pragma pack(push, 1)
struct Rejected {
    char type;                    // 1:  'J'
    uint64_t timestamp;           // 8:  Nanoseconds since midnight
    char token[14];               // 14: Order token
    char reason;                  // 1:  Reject reason code
    // Total: 24 bytes

    uint64_t get_timestamp() const { return read_be64(reinterpret_cast<const uint8_t*>(&timestamp)); }
};
static_assert(sizeof(Rejected) == 24, "Rejected must be 24 bytes");
#pragma pack(pop)

/**
 * Replaced Message ('U')
 * Size: 80 bytes
 */
#pragma pack(push, 1)
struct Replaced {
    char type;                    // 1:  'U'
    uint64_t timestamp;           // 8:  Nanoseconds since midnight
    char replacement_token[14];   // 14: New order token
    char side;                    // 1:  Side
    uint32_t quantity;            // 4:  New quantity
    char stock[8];                // 8:  Stock symbol
    uint32_t price;               // 4:  New price
    uint32_t time_in_force;       // 4:  TIF
    char firm[4];                 // 4:  Firm
    char display;                 // 1:  Display
    uint64_t order_ref;           // 8:  Exchange order reference
    char capacity;                // 1:  Capacity
    char intermarket_sweep;       // 1:  ISO flag
    uint32_t min_quantity;        // 4:  Min quantity
    char cross_type;              // 1:  Cross type
    char order_state;             // 1:  Order state
    char previous_token[14];      // 14: Original order token
    char bbo_weight;              // 1:  BBO weight
    // Total: 80 bytes

    uint64_t get_timestamp() const { return read_be64(reinterpret_cast<const uint8_t*>(&timestamp)); }
    uint32_t get_quantity() const { return read_be32(reinterpret_cast<const uint8_t*>(&quantity)); }
    uint32_t get_price() const { return read_be32(reinterpret_cast<const uint8_t*>(&price)); }
    uint64_t get_order_ref() const { return read_be64(reinterpret_cast<const uint8_t*>(&order_ref)); }
};
static_assert(sizeof(Replaced) == 80, "Replaced must be 80 bytes");
#pragma pack(pop)

/**
 * System Event Message ('S')
 */
#pragma pack(push, 1)
struct SystemEvent {
    char type;                    // 1:  'S'
    uint64_t timestamp;           // 8:  Nanoseconds since midnight
    char event_code;              // 1:  Event type
    // Total: 10 bytes

    uint64_t get_timestamp() const { return read_be64(reinterpret_cast<const uint8_t*>(&timestamp)); }
};
static_assert(sizeof(SystemEvent) == 10, "SystemEvent must be 10 bytes");
#pragma pack(pop)

// System event codes
constexpr char EVENT_START_OF_DAY = 'S';
constexpr char EVENT_END_OF_DAY = 'E';

// Reject reason codes
constexpr char REJECT_TEST_MODE = 'T';
constexpr char REJECT_HALTED = 'H';
constexpr char REJECT_SHARES = 'Z';
constexpr char REJECT_PRICE = 'N';
constexpr char REJECT_FIRM_NOT_AUTHORIZED = 'F';
constexpr char REJECT_CLOSED = 'C';
constexpr char REJECT_REGULATORY = 'R';
constexpr char REJECT_DUPLICATE = 'D';
constexpr char REJECT_EXCEEDED_CANCEL = 'X';

// Cancel reason codes
constexpr char CANCEL_USER_REQUESTED = 'U';
constexpr char CANCEL_IOC = 'I';
constexpr char CANCEL_TIMEOUT = 'T';
constexpr char CANCEL_SUPERVISORY = 'S';
constexpr char CANCEL_HALTED = 'H';

// Liquidity indicators
constexpr char LIQUIDITY_ADDED = 'A';
constexpr char LIQUIDITY_REMOVED = 'R';
constexpr char LIQUIDITY_ROUTED = 'X';

}  // namespace ouch
}  // namespace hft
