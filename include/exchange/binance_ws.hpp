#pragma once

#include "../orderbook.hpp"
#include "../types.hpp"
#include "market_data.hpp"

#include <atomic>
#include <concepts>
#include <cstring>
#include <functional>
#include <libwebsockets.h>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

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
    bool is_closed = false; // True when candle is finalized
};

/**
 * Depth update (partial book snapshot)
 */
struct WsDepthUpdate {
    std::string symbol;
    uint64_t last_update_id = 0;

    struct Level {
        Price price = 0;       // Actual price * 10000
        Quantity quantity = 0; // Actual qty * QUANTITY_SCALE
    };

    // Fixed-size arrays matching BookSnapshot::MAX_LEVELS (20)
    std::array<Level, 20> bids{};
    std::array<Level, 20> asks{};
    int bid_count = 0;
    int ask_count = 0;
};

// Callbacks
using BookTickerCallback = std::function<void(const BookTicker&)>;
using WsTradeCallback = std::function<void(const WsTrade&)>;
using WsKlineCallback = std::function<void(const WsKline&)>;
using WsDepthCallback = std::function<void(const WsDepthUpdate&)>;
using WsErrorCallback = std::function<void(const std::string&)>;
using WsConnectCallback = std::function<void(bool connected)>;
// Callback concepts for compile-time type checking
template <typename T, typename... Args>
concept Callable = requires(T t, Args... args) {
    { t(args...) } -> std::same_as<void>;
};

template <typename T>
concept ReconnectCallable = Callable<T, uint32_t, bool>;

// Type-erased storage (required due to libwebsockets C-style callback architecture)
// Template setters below provide compile-time type checking
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
        : host_(use_testnet ? TESTNET_WS : MAINNET_WS), port_(use_testnet ? TESTNET_PORT : MAINNET_PORT),
          running_(false), connected_(false), wsi_(nullptr), context_(nullptr) {}

    ~BinanceWs() { disconnect(); }

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

    void set_book_ticker_callback(BookTickerCallback cb) { book_ticker_callback_ = std::move(cb); }

    void set_trade_callback(WsTradeCallback cb) { trade_callback_ = std::move(cb); }

    void set_kline_callback(WsKlineCallback cb) { kline_callback_ = std::move(cb); }

    void set_depth_callback(WsDepthCallback cb) { depth_callback_ = std::move(cb); }

    void set_error_callback(WsErrorCallback cb) { error_callback_ = std::move(cb); }

    void set_connect_callback(WsConnectCallback cb) { connect_callback_ = std::move(cb); }

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

        // Test mode: simulate connection without thread
        if (test_mode_) {
            running_ = true;
            connected_ = true;
            if (connect_callback_) {
                connect_callback_(true);
            }
            return true;
        }

        // Real mode: spawn event loop thread
        running_ = true; // LCOV_EXCL_LINE - Network I/O: real mode thread spawn
        ws_thread_ = std::thread(&BinanceWs::run_event_loop, this); // LCOV_EXCL_LINE
        return true;                                                // LCOV_EXCL_LINE
    }

    void disconnect() {
        running_ = false;

        // Test mode: immediate cleanup
        if (test_mode_) {
            connected_ = false;
            if (connect_callback_) {
                connect_callback_(false);
            }
            return;
        }

        // Real mode: join thread and cleanup libwebsockets
        if (ws_thread_.joinable()) {       // LCOV_EXCL_LINE - Network I/O: real mode cleanup
            ws_thread_.join();             // LCOV_EXCL_LINE
        }                                  // LCOV_EXCL_LINE
        if (context_) {                    // LCOV_EXCL_LINE
            lws_context_destroy(context_); // LCOV_EXCL_LINE
            context_ = nullptr;            // LCOV_EXCL_LINE
        }                                  // LCOV_EXCL_LINE
        connected_ = false;
    }

    bool is_connected() const { return connected_; }

    bool is_running() const { return running_; }

    // ========================================
    // Test Mode Helpers
    // ========================================

    void set_test_mode(bool enable) { test_mode_ = enable; }

    void simulate_error(const std::string& error) {
        if (error_callback_) {
            error_callback_(error);
        }
    }

    // For testing: expose parse_message
    void parse_message_for_test(const std::string& json) { parse_message(json); }

    // ========================================
    // Auto-Reconnect and Health Management
    // ========================================

    void enable_auto_reconnect(bool enable = true) { auto_reconnect_ = enable; }

    bool is_healthy() const {
        return is_healthy(30); // Default to 30 seconds
    }

    bool is_healthy(int timeout_seconds) const {
        if (!connected_)
            return false;
        // Consider healthy if we received data within timeout
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_data_time_.load()).count();
        return elapsed < timeout_seconds;
    }

    void force_reconnect() { reconnect_requested_ = true; }

    // Template setter with concept constraint for compile-time type checking
    template <ReconnectCallable Callback>
    void set_reconnect_callback(Callback&& cb) {
        reconnect_callback_ = std::forward<Callback>(cb);
    }

    /**
     * Convert WsDepthUpdate to hft::BookSnapshot for metrics consumption.
     *
     * Binance sends aggregated levels, OrderBook expects order-by-order.
     * Metrics only use hft::BookSnapshot, so we convert directly.
     */
    static hft::BookSnapshot depth_to_snapshot(const WsDepthUpdate& depth) {
        hft::BookSnapshot snapshot;

        // Best bid/ask
        if (depth.bid_count > 0) {
            snapshot.best_bid = depth.bids[0].price;
            snapshot.best_bid_qty = depth.bids[0].quantity;
        }
        if (depth.ask_count > 0) {
            snapshot.best_ask = depth.asks[0].price;
            snapshot.best_ask_qty = depth.asks[0].quantity;
        }

        // Copy levels
        snapshot.bid_level_count = std::min(depth.bid_count, static_cast<int>(hft::BookSnapshot::MAX_LEVELS));
        snapshot.ask_level_count = std::min(depth.ask_count, static_cast<int>(hft::BookSnapshot::MAX_LEVELS));

        for (int i = 0; i < snapshot.bid_level_count; ++i) {
            snapshot.bid_levels[i].price = depth.bids[i].price;
            snapshot.bid_levels[i].quantity = depth.bids[i].quantity;
        }

        for (int i = 0; i < snapshot.ask_level_count; ++i) {
            snapshot.ask_levels[i].price = depth.asks[i].price;
            snapshot.ask_levels[i].quantity = depth.asks[i].quantity;
        }

        return snapshot;
    }

    /**
     * Parse price/qty pairs from JSON array string.
     * Format: ["42000.50","1.5"],["41999.00","3.2"]
     *
     * Public for testing. Returns number of levels parsed.
     */
    static int parse_levels(const std::string& json, std::array<WsDepthUpdate::Level, 20>& levels) {
        int count = 0;
        size_t pos = 0;

        while (count < 20 && pos < json.length()) {
            // Find next [ bracket
            size_t bracket_start = json.find('[', pos);
            if (bracket_start == std::string::npos)
                break;

            // Find closing ] bracket
            size_t bracket_end = json.find(']', bracket_start);
            if (bracket_end == std::string::npos)
                break;

            // Extract price and quantity strings
            size_t price_start = json.find('"', bracket_start) + 1;
            size_t price_end = json.find('"', price_start);
            size_t qty_start = json.find('"', price_end + 1) + 1;
            size_t qty_end = json.find('"', qty_start);

            if (price_end != std::string::npos && qty_end != std::string::npos) {
                double price = std::stod(json.substr(price_start, price_end - price_start));
                double qty = std::stod(json.substr(qty_start, qty_end - qty_start));

                levels[count].price = static_cast<Price>(price * 10000.0);
                levels[count].quantity = static_cast<Quantity>(qty * 10000.0);
                count++;
            }

            pos = bracket_end + 1;
        }

        return count;
    }

private:
    std::string host_;
    int port_;
    std::vector<std::string> streams_;

    std::atomic<bool> running_;
    std::atomic<bool> connected_;
    std::atomic<bool> test_mode_{false};
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
    WsDepthCallback depth_callback_;
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
    std::string build_stream_path() { // LCOV_EXCL_START - Network I/O: only called from real mode run_event_loop
        if (streams_.size() == 1) {
            return "/ws/" + streams_[0];
        }

        std::string path = "/stream?streams=";
        for (size_t i = 0; i < streams_.size(); ++i) {
            if (i > 0)
                path += "/";
            path += streams_[i];
        }
        return path;
    } // LCOV_EXCL_STOP

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

        // Determine message type and parse (order matters: check most specific first)
        if (stream_name.find("@kline") != std::string::npos || data_json.find("\"k\":") != std::string::npos) {
            parse_kline(data_json);
        } else if (stream_name.find("@bookTicker") != std::string::npos ||
                   (data_json.find("\"b\":") != std::string::npos && data_json.find("\"a\":") != std::string::npos)) {
            parse_book_ticker(data_json);
        } else if (stream_name.find("@trade") != std::string::npos ||
                   (data_json.find("\"t\":") != std::string::npos && data_json.find("\"p\":") != std::string::npos)) {
            parse_trade(data_json);
        } else if (stream_name.find("@depth") != std::string::npos || data_json.find("\"bids\"") != std::string::npos) {
            // Extract symbol from stream name (e.g., "btcusdt@depth20@100ms")
            std::string symbol = stream_name.substr(0, stream_name.find('@'));
            // Convert to uppercase for consistency
            std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::toupper);
            parse_depth(data_json, symbol);
        }
    }

    // Parse book ticker: {"u":123,"s":"BTCUSDT","b":"50000.00","B":"1.5","a":"50001.00","A":"2.0"}
    void parse_book_ticker(const std::string& json) {
        if (!book_ticker_callback_)
            return;

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
        if (!trade_callback_)
            return;

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
        if (!kline_callback_)
            return;

        // Find the "k" object
        size_t k_pos = json.find("\"k\":");
        if (k_pos == std::string::npos) // LCOV_EXCL_LINE - Defensive: routing already verified "k": exists
            return;                     // LCOV_EXCL_LINE

        size_t start = json.find("{", k_pos);
        size_t end = json.find("}", start);
        if (start == std::string::npos ||
            end == std::string::npos) // LCOV_EXCL_LINE - Defensive: valid JSON from routing
            return;                   // LCOV_EXCL_LINE

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

    // Parse depth: {"lastUpdateId":160,"bids":[["42000.50","1.5"]],"asks":[["42001.00","2.0"]]}
    // Symbol comes from stream name (e.g., btcusdt@depth20@100ms), passed as parameter
    void parse_depth(const std::string& json, const std::string& symbol) {
        if (!depth_callback_)
            return;

        WsDepthUpdate depth;
        depth.symbol = symbol;
        depth.last_update_id = extract_uint64(json, "lastUpdateId");

        // Parse bids array
        size_t bids_pos = json.find("\"bids\":[");
        if (bids_pos != std::string::npos) {
            size_t start = bids_pos + 8; // After "bids":[
            // Find outer closing ]] - nested array end
            size_t end = json.find("]]", start);
            if (end != std::string::npos) {
                // Extract array contents: [["level1"],["level2"]]
                std::string bids_json = json.substr(start, end - start + 1);
                depth.bid_count = parse_levels(bids_json, depth.bids);
            }
        }

        // Parse asks array
        size_t asks_pos = json.find("\"asks\":[");
        if (asks_pos != std::string::npos) {
            size_t start = asks_pos + 8; // After "asks":[
            // Find outer closing ]] - nested array end
            size_t end = json.find("]]", start);
            if (end != std::string::npos) {
                // Extract array contents: [["level1"],["level2"]]
                std::string asks_json = json.substr(start, end - start + 1);
                depth.ask_count = parse_levels(asks_json, depth.asks);
            }
        }

        depth_callback_(depth);
    }

    // Simple JSON value extractors
    std::string extract_string(const std::string& json, const std::string& key) {
        std::string search = "\"" + key + "\":\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos)
            return "";

        size_t start = pos + search.length();
        size_t end = json.find("\"", start);
        if (end == std::string::npos) // LCOV_EXCL_LINE - Defensive: valid JSON from routing
            return "";                // LCOV_EXCL_LINE

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

        // Try unquoted: "key":123.45                                                 // LCOV_EXCL_START
        search = "\"" + key + "\":";  // Defensive: Binance always uses
        pos = json.find(search);      // quoted numbers, this fallback
        if (pos == std::string::npos) // is for spec compliance only
            return 0.0;

        size_t start = pos + search.length();
        size_t end = json.find_first_of(",}", start);
        if (end == std::string::npos)
            return 0.0;

        return std::stod(json.substr(start, end - start)); // LCOV_EXCL_STOP
    }

    uint64_t extract_uint64(const std::string& json, const std::string& key) {
        std::string search = "\"" + key + "\":";
        size_t pos = json.find(search);
        if (pos == std::string::npos)
            return 0;

        size_t start = pos + search.length();
        size_t end = json.find_first_of(",}", start);
        if (end == std::string::npos) // LCOV_EXCL_LINE - Defensive: valid JSON from routing
            return 0;                 // LCOV_EXCL_LINE

        return std::stoull(json.substr(start, end - start));
    }

    bool extract_bool(const std::string& json, const std::string& key) {
        std::string search = "\"" + key + "\":";
        size_t pos = json.find(search);
        if (pos == std::string::npos)
            return false;

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

    static int ws_callback(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in,
                           size_t len) { // LCOV_EXCL_START - Network I/O: libwebsockets callback
        BinanceWs* self = static_cast<BinanceWs*>(lws_context_user(lws_get_context(wsi)));

        if (!self)
            return 0;

        switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            self->connected_ = true;
            if (self->connect_callback_) {
                self->connect_callback_(true);
            }
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE:
#ifdef LWS_VERSION_4_3_PLUS
        case 71: // LWS_CALLBACK_CLIENT_RECEIVE in lws 4.3+
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
    } // LCOV_EXCL_STOP

    // Event loop thread
    void run_event_loop() { // LCOV_EXCL_START - Network I/O: libwebsockets event loop
        // Create context
        struct lws_context_creation_info info;
        memset(&info, 0, sizeof(info));

        static const struct lws_protocols protocols[] = {{
                                                             "binance-ws", ws_callback, 0,
                                                             65536, // rx buffer size
                                                         },
                                                         {nullptr, nullptr, 0, 0}};

        info.port = CONTEXT_PORT_NO_LISTEN;
        info.protocols = protocols;
        info.gid = -1;
        info.uid = -1;
        info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        info.user = this;
        // CA certificate bundle for SSL verification (WSL2 compatibility)
        info.client_ssl_ca_filepath = "/etc/ssl/certs/ca-certificates.crt";

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
        ccinfo.protocol = protocols[0].name; // Use same protocol name as defined
        // WSL2'de CA bundle sorunu olabilir, sertifika doğrulamayı atla
        ccinfo.ssl_connection = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK |
                                LCCSCF_ALLOW_EXPIRED | LCCSCF_ALLOW_INSECURE;

        wsi_ = lws_client_connect_via_info(&ccinfo);
        if (!wsi_) {
            if (error_callback_) {
                error_callback_("Failed to connect to WebSocket");
            }
            return;
        }

        // Event loop with auto-reconnect (unlimited retries with exponential backoff)
        uint32_t retry_count = 0;
        static constexpr int BASE_RETRY_DELAY_SEC = 5;  // Start with 5 seconds
        static constexpr int MAX_RETRY_DELAY_SEC = 300; // Max 5 minutes between retries
        auto last_connect_attempt = std::chrono::steady_clock::now();

        while (running_) {
            lws_service(context_, 100); // 100ms timeout

            // Handle reconnection if needed
            auto now = std::chrono::steady_clock::now();

            // Calculate backoff delay: 5s, 10s, 20s, 40s, 80s, ... up to 300s
            int backoff_delay = std::min(BASE_RETRY_DELAY_SEC * (1 << std::min(retry_count, 6u)), MAX_RETRY_DELAY_SEC);

            auto since_last_attempt =
                std::chrono::duration_cast<std::chrono::seconds>(now - last_connect_attempt).count();

            if (reconnect_requested_.exchange(false) || (!connected_ && since_last_attempt > backoff_delay)) {
                // UNLIMITED RETRIES - connection is essential for trading
                ++retry_count;
                if (error_callback_) {
                    error_callback_("Reconnection attempt " + std::to_string(retry_count) + " (next retry in " +
                                    std::to_string(backoff_delay) + "s)");
                }
                if (reconnect_callback_) {
                    reconnect_callback_(retry_count, false);
                }

                // Try to reconnect
                wsi_ = lws_client_connect_via_info(&ccinfo);
                last_connect_attempt = now;
            }

            // Reset retry count on successful connection
            if (connected_) {
                if (retry_count > 0 && reconnect_callback_) {
                    reconnect_callback_(retry_count, true); // Notify success
                }
                retry_count = 0;
            }
        }
    } // LCOV_EXCL_STOP
};

} // namespace exchange
} // namespace hft
