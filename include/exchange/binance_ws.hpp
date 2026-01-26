#pragma once

#include "market_data.hpp"
#include "../types.hpp"
#include <libwebsockets.h>
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <queue>
#include <cstring>

namespace hft {
namespace exchange {

/**
 * Binance WebSocket stream types
 */
enum class StreamType {
    Trade,      // Individual trade streams
    BookTicker, // Best bid/ask updates
    Depth5,     // Top 5 levels
    Depth10,    // Top 10 levels
    Kline       // Candlestick updates
};

/**
 * Book ticker update (best bid/ask)
 */
struct BookTicker {
    std::string symbol;
    Price bid_price = 0;
    double bid_qty = 0;
    Price ask_price = 0;
    double ask_qty = 0;
    Timestamp update_time = 0;
};

/**
 * Trade update from WebSocket
 */
struct WsTrade {
    std::string symbol;
    uint64_t trade_id = 0;
    Price price = 0;
    double quantity = 0;
    Timestamp time = 0;
    bool is_buyer_maker = false;
};

/**
 * Kline update from WebSocket
 */
struct WsKline {
    std::string symbol;
    Timestamp open_time = 0;
    Timestamp close_time = 0;
    Price open = 0;
    Price high = 0;
    Price low = 0;
    Price close = 0;
    double volume = 0;
    uint32_t trades = 0;
    bool is_closed = false;  // True when candle is finalized
};

// Callbacks
using BookTickerCallback = std::function<void(const BookTicker&)>;
using WsTradeCallback = std::function<void(const WsTrade&)>;
using WsKlineCallback = std::function<void(const WsKline&)>;
using WsErrorCallback = std::function<void(const std::string&)>;
using WsConnectCallback = std::function<void(bool connected)>;
// Note: std::function has ~40 byte overhead but WsReconnectCallback is only called
// on reconnect events (rare), not on the hot data path. Acceptable trade-off for flexibility.
using WsReconnectCallback = std::function<void(uint32_t retry_count, bool success)>;

/**
 * Binance WebSocket Client
 *
 * Connects to Binance WebSocket streams for real-time market data.
 * Uses libwebsockets for WebSocket protocol handling.
 *
 * Usage:
 *   BinanceWs ws;
 *   ws.set_book_ticker_callback([](const BookTicker& bt) {
 *       std::cout << bt.symbol << " bid=" << bt.bid_price << "\n";
 *   });
 *   ws.subscribe_book_ticker("BTCUSDT");
 *   ws.connect();
 *   // ... ws runs in background thread
 *   ws.disconnect();
 */
class BinanceWs {
public:
    static constexpr const char* MAINNET_WS = "stream.binance.com";
    static constexpr const char* TESTNET_WS = "testnet.binance.vision";
    static constexpr int MAINNET_PORT = 9443;
    static constexpr int TESTNET_PORT = 443;

    explicit BinanceWs(bool use_testnet = false)
        : host_(use_testnet ? TESTNET_WS : MAINNET_WS)
        , port_(use_testnet ? TESTNET_PORT : MAINNET_PORT)
        , running_(false)
        , connected_(false)
        , wsi_(nullptr)
        , context_(nullptr) {
    }

    ~BinanceWs() {
        disconnect();
    }

    // Non-copyable
    BinanceWs(const BinanceWs&) = delete;
    BinanceWs& operator=(const BinanceWs&) = delete;

    // ========================================
    // Subscription Management
    // ========================================

    void subscribe_book_ticker(const std::string& symbol) {
        std::string lower_symbol = to_lower(symbol);
        streams_.push_back(lower_symbol + "@bookTicker");
    }

    void subscribe_trade(const std::string& symbol) {
        std::string lower_symbol = to_lower(symbol);
        streams_.push_back(lower_symbol + "@trade");
    }

    void subscribe_kline(const std::string& symbol, const std::string& interval = "1m") {
        std::string lower_symbol = to_lower(symbol);
        streams_.push_back(lower_symbol + "@kline_" + interval);
    }

    void subscribe_depth(const std::string& symbol, int levels = 5) {
        std::string lower_symbol = to_lower(symbol);
        streams_.push_back(lower_symbol + "@depth" + std::to_string(levels) + "@100ms");
    }

    // ========================================
    // Callbacks
    // ========================================

    void set_book_ticker_callback(BookTickerCallback cb) {
        book_ticker_callback_ = std::move(cb);
    }

    void set_trade_callback(WsTradeCallback cb) {
        trade_callback_ = std::move(cb);
    }

    void set_kline_callback(WsKlineCallback cb) {
        kline_callback_ = std::move(cb);
    }

    void set_error_callback(WsErrorCallback cb) {
        error_callback_ = std::move(cb);
    }

    void set_connect_callback(WsConnectCallback cb) {
        connect_callback_ = std::move(cb);
    }

    // ========================================
    // Connection Management
    // ========================================

    bool connect() {
        if (streams_.empty()) {
            if (error_callback_) {
                error_callback_("No streams subscribed");
            }
            return false;
        }

        running_ = true;
        ws_thread_ = std::thread(&BinanceWs::run_event_loop, this);
        return true;
    }

    void disconnect() {
        running_ = false;
        if (ws_thread_.joinable()) {
            ws_thread_.join();
        }
        if (context_) {
            lws_context_destroy(context_);
            context_ = nullptr;
        }
        connected_ = false;
    }

    bool is_connected() const {
        return connected_;
    }

    bool is_running() const {
        return running_;
    }

    // ========================================
    // Auto-Reconnect and Health Management
    // ========================================

    void enable_auto_reconnect(bool enable = true) {
        auto_reconnect_ = enable;
    }

    bool is_healthy() const {
        return is_healthy(30);  // Default to 30 seconds
    }

    bool is_healthy(int timeout_seconds) const {
        if (!connected_) return false;
        // Consider healthy if we received data within timeout
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_data_time_.load()).count();
        return elapsed < timeout_seconds;
    }

    void force_reconnect() {
        reconnect_requested_ = true;
    }

    void set_reconnect_callback(WsReconnectCallback cb) {
        reconnect_callback_ = std::move(cb);
    }

private:
    std::string host_;
    int port_;
    std::vector<std::string> streams_;

    std::atomic<bool> running_;
    std::atomic<bool> connected_;
    std::atomic<bool> auto_reconnect_{false};
    std::atomic<bool> reconnect_requested_{false};
    std::atomic<std::chrono::steady_clock::time_point> last_data_time_{std::chrono::steady_clock::now()};
    std::thread ws_thread_;

    struct lws* wsi_;
    struct lws_context* context_;

    // Callbacks
    BookTickerCallback book_ticker_callback_;
    WsTradeCallback trade_callback_;
    WsKlineCallback kline_callback_;
    WsErrorCallback error_callback_;
    WsConnectCallback connect_callback_;
    WsReconnectCallback reconnect_callback_;

    // Receive buffer
    std::string rx_buffer_;
    std::mutex rx_mutex_;

    static std::string to_lower(const std::string& s) {
        std::string result = s;
        for (char& c : result) {
            c = std::tolower(c);
        }
        return result;
    }

    // Build combined stream path
    std::string build_stream_path() {
        if (streams_.size() == 1) {
            return "/ws/" + streams_[0];
        }

        std::string path = "/stream?streams=";
        for (size_t i = 0; i < streams_.size(); ++i) {
            if (i > 0) path += "/";
            path += streams_[i];
        }
        return path;
    }

    // Parse incoming JSON message
    void parse_message(const std::string& json) {
        // Update last data time for health monitoring
        last_data_time_.store(std::chrono::steady_clock::now());

        // Check if it's a combined stream message
        bool is_combined = (json.find("\"stream\"") != std::string::npos);

        std::string data_json = json;
        std::string stream_name;

        if (is_combined) {
            // Extract stream name and data
            size_t stream_pos = json.find("\"stream\":\"");
            if (stream_pos != std::string::npos) {
                size_t start = stream_pos + 10;
                size_t end = json.find("\"", start);
                stream_name = json.substr(start, end - start);
            }

            size_t data_pos = json.find("\"data\":");
            if (data_pos != std::string::npos) {
                size_t start = json.find("{", data_pos);
                size_t end = json.rfind("}");
                if (start != std::string::npos && end > start) {
                    data_json = json.substr(start, end - start + 1);
                }
            }
        }

        // Determine message type and parse
        if (stream_name.find("@bookTicker") != std::string::npos ||
            data_json.find("\"b\":") != std::string::npos) {
            parse_book_ticker(data_json);
        } else if (stream_name.find("@trade") != std::string::npos ||
                   data_json.find("\"T\":") != std::string::npos) {
            parse_trade(data_json);
        } else if (stream_name.find("@kline") != std::string::npos ||
                   data_json.find("\"k\":") != std::string::npos) {
            parse_kline(data_json);
        }
    }

    // Parse book ticker: {"u":123,"s":"BTCUSDT","b":"50000.00","B":"1.5","a":"50001.00","A":"2.0"}
    void parse_book_ticker(const std::string& json) {
        if (!book_ticker_callback_) return;

        BookTicker bt;
        bt.symbol = extract_string(json, "s");
        bt.bid_price = static_cast<Price>(extract_double(json, "b") * 10000);
        bt.bid_qty = extract_double(json, "B");
        bt.ask_price = static_cast<Price>(extract_double(json, "a") * 10000);
        bt.ask_qty = extract_double(json, "A");
        bt.update_time = extract_uint64(json, "u");

        book_ticker_callback_(bt);
    }

    // Parse trade: {"e":"trade","E":123,"s":"BTCUSDT","t":123,"p":"50000.00","q":"1.5","T":123,"m":true}
    void parse_trade(const std::string& json) {
        if (!trade_callback_) return;

        WsTrade trade;
        trade.symbol = extract_string(json, "s");
        trade.trade_id = extract_uint64(json, "t");
        trade.price = static_cast<Price>(extract_double(json, "p") * 10000);
        trade.quantity = extract_double(json, "q");
        trade.time = extract_uint64(json, "T");
        trade.is_buyer_maker = extract_bool(json, "m");

        trade_callback_(trade);
    }

    // Parse kline: {"e":"kline","E":123,"s":"BTCUSDT","k":{...}}
    void parse_kline(const std::string& json) {
        if (!kline_callback_) return;

        // Find the "k" object
        size_t k_pos = json.find("\"k\":");
        if (k_pos == std::string::npos) return;

        size_t start = json.find("{", k_pos);
        size_t end = json.find("}", start);
        if (start == std::string::npos || end == std::string::npos) return;

        std::string k_json = json.substr(start, end - start + 1);

        WsKline kline;
        kline.symbol = extract_string(json, "s");
        kline.open_time = extract_uint64(k_json, "t");
        kline.close_time = extract_uint64(k_json, "T");
        kline.open = static_cast<Price>(extract_double(k_json, "o") * 10000);
        kline.high = static_cast<Price>(extract_double(k_json, "h") * 10000);
        kline.low = static_cast<Price>(extract_double(k_json, "l") * 10000);
        kline.close = static_cast<Price>(extract_double(k_json, "c") * 10000);
        kline.volume = extract_double(k_json, "v");
        kline.trades = static_cast<uint32_t>(extract_uint64(k_json, "n"));
        kline.is_closed = extract_bool(k_json, "x");

        kline_callback_(kline);
    }

    // Simple JSON value extractors
    std::string extract_string(const std::string& json, const std::string& key) {
        std::string search = "\"" + key + "\":\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return "";

        size_t start = pos + search.length();
        size_t end = json.find("\"", start);
        if (end == std::string::npos) return "";

        return json.substr(start, end - start);
    }

    double extract_double(const std::string& json, const std::string& key) {
        // Try quoted number first: "key":"123.45"
        std::string search = "\"" + key + "\":\"";
        size_t pos = json.find(search);

        if (pos != std::string::npos) {
            size_t start = pos + search.length();
            size_t end = json.find("\"", start);
            if (end != std::string::npos) {
                return std::stod(json.substr(start, end - start));
            }
        }

        // Try unquoted: "key":123.45
        search = "\"" + key + "\":";
        pos = json.find(search);
        if (pos == std::string::npos) return 0.0;

        size_t start = pos + search.length();
        size_t end = json.find_first_of(",}", start);
        if (end == std::string::npos) return 0.0;

        return std::stod(json.substr(start, end - start));
    }

    uint64_t extract_uint64(const std::string& json, const std::string& key) {
        std::string search = "\"" + key + "\":";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return 0;

        size_t start = pos + search.length();
        size_t end = json.find_first_of(",}", start);
        if (end == std::string::npos) return 0;

        return std::stoull(json.substr(start, end - start));
    }

    bool extract_bool(const std::string& json, const std::string& key) {
        std::string search = "\"" + key + "\":";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return false;

        size_t start = pos + search.length();
        return (json.compare(start, 4, "true") == 0);
    }

    // WebSocket callback (static, called by libwebsockets)
    // NOTE: lws 4.3+ remapped LWS_CALLBACK_CLIENT_RECEIVE from 8 to 71
#ifdef LWS_VERSION_4_3_PLUS
    static constexpr int LWS_CLIENT_RECEIVE_COMPAT = 71;
#else
    static constexpr int LWS_CLIENT_RECEIVE_COMPAT = LWS_CALLBACK_CLIENT_RECEIVE;
#endif

    static int ws_callback(struct lws* wsi, enum lws_callback_reasons reason,
                           void* user, void* in, size_t len) {
        BinanceWs* self = static_cast<BinanceWs*>(lws_context_user(lws_get_context(wsi)));

        if (!self) return 0;

        switch (reason) {
            case LWS_CALLBACK_CLIENT_ESTABLISHED:
                self->connected_ = true;
                if (self->connect_callback_) {
                    self->connect_callback_(true);
                }
                break;

            case LWS_CALLBACK_CLIENT_RECEIVE:
#ifdef LWS_VERSION_4_3_PLUS
            case 71:  // LWS_CALLBACK_CLIENT_RECEIVE in lws 4.3+
#endif
                if (in && len > 0) {
                    std::string msg(static_cast<char*>(in), len);
                    self->parse_message(msg);
                }
                break;

            case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
                self->connected_ = false;
                if (self->error_callback_) {
                    std::string error = in ? std::string(static_cast<char*>(in), len) : "Connection error";
                    self->error_callback_(error);
                }
                break;

            case LWS_CALLBACK_CLOSED:
                self->connected_ = false;
                if (self->connect_callback_) {
                    self->connect_callback_(false);
                }
                break;

            default:
                break;
        }

        return 0;
    }

    // Event loop thread
    void run_event_loop() {
        // Create context
        struct lws_context_creation_info info;
        memset(&info, 0, sizeof(info));

        static const struct lws_protocols protocols[] = {
            {
                "binance-ws",
                ws_callback,
                0,
                65536,  // rx buffer size
            },
            { nullptr, nullptr, 0, 0 }
        };

        info.port = CONTEXT_PORT_NO_LISTEN;
        info.protocols = protocols;
        info.gid = -1;
        info.uid = -1;
        info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        info.user = this;

        context_ = lws_create_context(&info);
        if (!context_) {
            if (error_callback_) {
                error_callback_("Failed to create WebSocket context");
            }
            return;
        }

        // Connect
        struct lws_client_connect_info ccinfo;
        memset(&ccinfo, 0, sizeof(ccinfo));

        std::string path = build_stream_path();

        ccinfo.context = context_;
        ccinfo.address = host_.c_str();
        ccinfo.port = port_;
        ccinfo.path = path.c_str();
        ccinfo.host = host_.c_str();
        ccinfo.origin = host_.c_str();
        ccinfo.protocol = "binance-ws";
        // WSL2'de CA bundle sorunu olabilir, sertifika doğrulamayı atla
        ccinfo.ssl_connection = LCCSCF_USE_SSL |
                                LCCSCF_ALLOW_SELFSIGNED |
                                LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;

        wsi_ = lws_client_connect_via_info(&ccinfo);
        if (!wsi_) {
            if (error_callback_) {
                error_callback_("Failed to connect to WebSocket");
            }
            return;
        }

        // Event loop
        while (running_) {
            lws_service(context_, 100);  // 100ms timeout
        }
    }
};

}  // namespace exchange
}  // namespace hft
