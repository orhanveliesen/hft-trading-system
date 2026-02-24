#pragma once

#include "../types.hpp"

#include <functional>
#include <map>
#include <string>

namespace hft {
namespace exchange {

// Binance API endpoints
struct BinanceEndpoints {
    static constexpr const char* MAINNET_REST = "https://api.binance.com";
    static constexpr const char* MAINNET_WS = "wss://stream.binance.com:9443";
    static constexpr const char* TESTNET_REST = "https://testnet.binance.vision";
    static constexpr const char* TESTNET_WS = "wss://testnet.binance.vision";
};

// Order types
enum class OrderType {
    Limit,
    Market,
    LimitMaker // Post-only
};

enum class TimeInForce {
    GTC, // Good Till Cancel
    IOC, // Immediate or Cancel
    FOK  // Fill or Kill
};

// Order request
struct OrderRequest {
    std::string symbol;
    Side side;
    OrderType type;
    TimeInForce tif;
    Price price;
    Quantity quantity;
    std::string client_order_id;
};

// Order response
struct OrderResponse {
    bool success;
    OrderId order_id;
    std::string client_order_id;
    std::string status;
    Price filled_price;
    Quantity filled_quantity;
    std::string error_msg;
};

// Market data update
struct BookUpdate {
    std::string symbol;
    Price best_bid;
    Price best_ask;
    Quantity bid_size;
    Quantity ask_size;
    Timestamp timestamp;
};

// Trade update (execution report)
struct TradeUpdate {
    OrderId order_id;
    std::string client_order_id;
    Side side;
    Price price;
    Quantity quantity;
    bool is_maker;
    Timestamp timestamp;
};

// Callbacks
using BookUpdateCallback = std::function<void(const BookUpdate&)>;
using TradeUpdateCallback = std::function<void(const TradeUpdate&)>;
using ErrorCallback = std::function<void(const std::string&)>;

// Binance client configuration
struct BinanceConfig {
    std::string api_key;
    std::string api_secret;
    bool use_testnet = true;
    std::string symbol = "BTCUSDT";
    int recv_window = 5000;
};

// Abstract Binance client interface
// Implementation requires libcurl and libwebsockets (separate .cpp file)
class BinanceClient {
public:
    virtual ~BinanceClient() = default;

    // Connection management
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;

    // Market data
    virtual void subscribe_book(const std::string& symbol) = 0;
    virtual void set_book_callback(BookUpdateCallback callback) = 0;

    // Trading
    virtual OrderResponse place_order(const OrderRequest& order) = 0;
    virtual bool cancel_order(const std::string& symbol, OrderId order_id) = 0;
    virtual bool cancel_all_orders(const std::string& symbol) = 0;

    // User data stream
    virtual void subscribe_user_data() = 0;
    virtual void set_trade_callback(TradeUpdateCallback callback) = 0;
    virtual void set_error_callback(ErrorCallback callback) = 0;

    // Account info
    virtual double get_balance(const std::string& asset) = 0;
    virtual std::map<std::string, double> get_all_balances() = 0;
};

// Factory function (implemented in binance_client.cpp)
// Returns nullptr if dependencies (curl, websockets) not available
std::unique_ptr<BinanceClient> create_binance_client(const BinanceConfig& config);

// Helper to convert our types to Binance format
inline std::string side_to_string(Side side) {
    return side == Side::Buy ? "BUY" : "SELL";
}

inline std::string order_type_to_string(OrderType type) {
    switch (type) {
    case OrderType::Limit:
        return "LIMIT";
    case OrderType::Market:
        return "MARKET";
    case OrderType::LimitMaker:
        return "LIMIT_MAKER";
    }
    return "LIMIT";
}

inline std::string tif_to_string(TimeInForce tif) {
    switch (tif) {
    case TimeInForce::GTC:
        return "GTC";
    case TimeInForce::IOC:
        return "IOC";
    case TimeInForce::FOK:
        return "FOK";
    }
    return "GTC";
}

// Convert price from our fixed-point format to Binance decimal string
inline std::string price_to_string(Price price, int decimals = 2) {
    double d = price / 10000.0;
    char buf[32];
    snprintf(buf, sizeof(buf), "%.*f", decimals, d);
    return buf;
}

// Convert quantity to string
inline std::string quantity_to_string(Quantity qty, int decimals = 5) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.*f", decimals, static_cast<double>(qty));
    return buf;
}

} // namespace exchange
} // namespace hft
