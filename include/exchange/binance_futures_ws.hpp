#pragma once

#include "../types.hpp"
#include "binance_ws.hpp" // For WsKline
#include "futures_market_data.hpp"
#include "market_data.hpp"

#include <atomic>
#include <cstring>
#include <functional>
#include <libwebsockets.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hft {
namespace exchange {

/**
 * Aggregate Trade from WebSocket (futures-specific)
 * Futures use aggregate trades with different fields than spot trades
 * Uses double for price to prevent overflow (BTC 430K+)
 */
struct WsAggTrade {
    std::string symbol;
    uint64_t agg_trade_id = 0;
    double price = 0.0;
    double quantity = 0.0;
    uint64_t first_trade_id = 0;
    uint64_t last_trade_id = 0;
    Timestamp time = 0;
    bool is_buyer_maker = false;
};

// Callbacks (reuse WsKlineCallback, WsErrorCallback, WsConnectCallback from binance_ws.hpp)
using MarkPriceCallback = std::function<void(const MarkPriceUpdate&)>;
using LiquidationCallback = std::function<void(const LiquidationOrder&)>;
using FuturesBookTickerCallback = std::function<void(const FuturesBookTicker&)>;
using WsAggTradeCallback = std::function<void(const WsAggTrade&)>;

/**
 * Binance Futures WebSocket Client
 *
 * Connects to Binance USD-M Futures WebSocket streams for real-time market data.
 * Uses libwebsockets for WebSocket protocol handling.
 *
 * Streams:
 * - @markPrice@1s - Mark price and funding rate updates
 * - @forceOrder - Liquidation orders
 * - @bookTicker - Best bid/ask updates
 * - @aggTrade - Aggregate trade stream
 * - @kline_<interval> - Candlestick updates
 *
 * Usage:
 *   BinanceFuturesWs ws;
 *   ws.set_mark_price_callback([](const MarkPriceUpdate& mp) {
 *       std::cout << mp.symbol << " mark=" << mp.mark_price << "\n";
 *   });
 *   ws.subscribe_mark_price("BTCUSDT");
 *   ws.connect();
 *   // ... ws runs in background thread
 *   ws.disconnect();
 */
class BinanceFuturesWs {
public:
    static constexpr const char* MAINNET_WS = "fstream.binance.com";
    static constexpr const char* TESTNET_WS = "testnet.binancefuture.com";
    static constexpr int PORT = 443;

    explicit BinanceFuturesWs(bool use_testnet = false)
        : host_(use_testnet ? TESTNET_WS : MAINNET_WS), port_(PORT), running_(false), connected_(false), wsi_(nullptr),
          context_(nullptr) {}

    ~BinanceFuturesWs() { disconnect(); }

    // Non-copyable
    BinanceFuturesWs(const BinanceFuturesWs&) = delete;
    BinanceFuturesWs& operator=(const BinanceFuturesWs&) = delete;

    // ========================================
    // Subscription Management
    // ========================================

    void subscribe_mark_price(const std::string& symbol, const std::string& update_speed = "1s") {
        std::string lower_symbol = to_lower(symbol);
        streams_.push_back(lower_symbol + "@markPrice@" + update_speed);
    }

    void subscribe_liquidation(const std::string& symbol) {
        std::string lower_symbol = to_lower(symbol);
        streams_.push_back(lower_symbol + "@forceOrder");
    }

    void subscribe_book_ticker(const std::string& symbol) {
        std::string lower_symbol = to_lower(symbol);
        streams_.push_back(lower_symbol + "@bookTicker");
    }

    void subscribe_agg_trade(const std::string& symbol) {
        std::string lower_symbol = to_lower(symbol);
        streams_.push_back(lower_symbol + "@aggTrade");
    }

    void subscribe_kline(const std::string& symbol, const std::string& interval = "1m") {
        std::string lower_symbol = to_lower(symbol);
        streams_.push_back(lower_symbol + "@kline_" + interval);
    }

    // ========================================
    // Callbacks
    // ========================================

    void set_mark_price_callback(MarkPriceCallback cb) { mark_price_callback_ = std::move(cb); }

    void set_liquidation_callback(LiquidationCallback cb) { liquidation_callback_ = std::move(cb); }

    void set_book_ticker_callback(FuturesBookTickerCallback cb) { book_ticker_callback_ = std::move(cb); }

    void set_agg_trade_callback(WsAggTradeCallback cb) { agg_trade_callback_ = std::move(cb); }

    void set_kline_callback(WsKlineCallback cb) { kline_callback_ = std::move(cb); }

    void set_error_callback(WsErrorCallback cb) { error_callback_ = std::move(cb); }

    void set_connect_callback(WsConnectCallback cb) { connect_callback_ = std::move(cb); }

    // ========================================
    // Accessors (for testing)
    // ========================================

    std::string get_host() const { return host_; }
    int get_port() const { return port_; }
    const std::vector<std::string>& get_streams() const { return streams_; }

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
        ws_thread_ = std::thread(&BinanceFuturesWs::run_event_loop, this);
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

    bool is_connected() const { return connected_; }

    bool is_running() const { return running_; }

    // ========================================
    // Static Parsing Methods (for testing)
    // ========================================

    static MarkPriceUpdate parse_mark_price_update(const std::string& json) {
        MarkPriceUpdate mp;
        mp.symbol = extract_string(json, "s");
        mp.mark_price = extract_double(json, "p");
        mp.index_price = extract_double(json, "P");
        mp.funding_rate = extract_double(json, "r");
        mp.next_funding_time = extract_uint64(json, "T");
        mp.event_time = extract_uint64(json, "E");
        return mp;
    }

    static LiquidationOrder parse_liquidation_order(const std::string& json) {
        LiquidationOrder lo;

        // Extract "o" object
        size_t o_pos = json.find("\"o\":");
        if (o_pos == std::string::npos)
            return lo;

        size_t start = json.find("{", o_pos);
        size_t end = json.find("}", start);
        if (start == std::string::npos || end == std::string::npos)
            return lo;

        std::string o_json = json.substr(start, end - start + 1);

        lo.symbol = extract_string(o_json, "s");
        std::string side_str = extract_string(o_json, "S");
        lo.side = (side_str == "SELL") ? Side::Sell : Side::Buy;
        lo.price = extract_double(o_json, "p");
        lo.quantity = extract_double(o_json, "q");
        lo.avg_price = extract_double(o_json, "ap");
        lo.order_status = extract_string(o_json, "X");
        lo.trade_time = extract_uint64(o_json, "T");
        lo.event_time = extract_uint64(json, "E");

        return lo;
    }

    static FuturesBookTicker parse_futures_book_ticker(const std::string& json) {
        FuturesBookTicker fbt;
        fbt.symbol = extract_string(json, "s");
        fbt.bid_price = extract_double(json, "b");
        fbt.bid_qty = extract_double(json, "B");
        fbt.ask_price = extract_double(json, "a");
        fbt.ask_qty = extract_double(json, "A");
        fbt.transaction_time = extract_uint64(json, "T");
        fbt.event_time = extract_uint64(json, "E");
        return fbt;
    }

    static WsAggTrade parse_agg_trade(const std::string& json) {
        WsAggTrade trade;
        trade.symbol = extract_string(json, "s");
        trade.agg_trade_id = extract_uint64(json, "a");
        trade.price = extract_double(json, "p");
        trade.quantity = extract_double(json, "q");
        trade.first_trade_id = extract_uint64(json, "f");
        trade.last_trade_id = extract_uint64(json, "l");
        trade.time = extract_uint64(json, "T");
        trade.is_buyer_maker = extract_bool(json, "m");
        return trade;
    }

    static WsKline parse_kline(const std::string& json) {
        WsKline kline;

        // Extract symbol from outer JSON
        kline.symbol = extract_string(json, "s");

        // Find the "k" object
        size_t k_pos = json.find("\"k\":");
        if (k_pos == std::string::npos)
            return kline;

        size_t start = json.find("{", k_pos);
        size_t end = json.find("}", start);
        if (start == std::string::npos || end == std::string::npos)
            return kline;

        std::string k_json = json.substr(start, end - start + 1);

        kline.open_time = extract_uint64(k_json, "t");
        kline.close_time = extract_uint64(k_json, "T");
        kline.open = static_cast<Price>(extract_double(k_json, "o") * 10000);
        kline.high = static_cast<Price>(extract_double(k_json, "h") * 10000);
        kline.low = static_cast<Price>(extract_double(k_json, "l") * 10000);
        kline.close = static_cast<Price>(extract_double(k_json, "c") * 10000);
        kline.volume = extract_double(k_json, "v");
        kline.trades = static_cast<uint32_t>(extract_uint64(k_json, "n"));
        kline.is_closed = extract_bool(k_json, "x");

        return kline;
    }

    // ========================================
    // Static Helpers (for testing stream path building)
    // ========================================

    static std::string build_stream_path(const std::vector<std::string>& streams) {
        if (streams.size() == 1) {
            return "/ws/" + streams[0];
        }

        std::string path = "/stream?streams=";
        for (size_t i = 0; i < streams.size(); ++i) {
            if (i > 0)
                path += "/";
            path += streams[i];
        }
        return path;
    }

private:
    std::string host_;
    int port_;
    std::vector<std::string> streams_;

    std::atomic<bool> running_;
    std::atomic<bool> connected_;
    std::thread ws_thread_;

    struct lws* wsi_;
    struct lws_context* context_;

    // Callbacks
    MarkPriceCallback mark_price_callback_;
    LiquidationCallback liquidation_callback_;
    FuturesBookTickerCallback book_ticker_callback_;
    WsAggTradeCallback agg_trade_callback_;
    WsKlineCallback kline_callback_;
    WsErrorCallback error_callback_;
    WsConnectCallback connect_callback_;

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

    // Build combined stream path (calls static version)
    std::string build_stream_path_internal() const { return build_stream_path(streams_); }

    // Parse incoming JSON message
    void parse_message(const std::string& json) {
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
        if (stream_name.find("@markPrice") != std::string::npos ||
            data_json.find("\"e\":\"markPriceUpdate\"") != std::string::npos) {
            if (mark_price_callback_) {
                mark_price_callback_(parse_mark_price_update(data_json));
            }
        } else if (stream_name.find("@forceOrder") != std::string::npos ||
                   data_json.find("\"e\":\"forceOrder\"") != std::string::npos) {
            if (liquidation_callback_) {
                liquidation_callback_(parse_liquidation_order(data_json));
            }
        } else if (stream_name.find("@bookTicker") != std::string::npos ||
                   data_json.find("\"e\":\"bookTicker\"") != std::string::npos) {
            if (book_ticker_callback_) {
                book_ticker_callback_(parse_futures_book_ticker(data_json));
            }
        } else if (stream_name.find("@aggTrade") != std::string::npos ||
                   data_json.find("\"e\":\"aggTrade\"") != std::string::npos) {
            if (agg_trade_callback_) {
                agg_trade_callback_(parse_agg_trade(data_json));
            }
        } else if (stream_name.find("@kline") != std::string::npos ||
                   data_json.find("\"e\":\"kline\"") != std::string::npos) {
            if (kline_callback_) {
                kline_callback_(parse_kline(data_json));
            }
        }
    }

    // Simple JSON value extractors (static for testing)
    static std::string extract_string(const std::string& json, const std::string& key) {
        std::string search = "\"" + key + "\":\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos)
            return "";

        size_t start = pos + search.length();
        size_t end = json.find("\"", start);
        if (end == std::string::npos)
            return "";

        return json.substr(start, end - start);
    }

    static double extract_double(const std::string& json, const std::string& key) {
        // Try quoted number first: "key":"123.45"
        std::string search = "\"" + key + "\":\"";
        size_t pos = json.find(search);

        if (pos != std::string::npos) {
            size_t start = pos + search.length();
            size_t end = json.find("\"", start);
            if (end != std::string::npos) {
                try {
                    return std::stod(json.substr(start, end - start));
                } catch (...) {
                    return 0.0;
                }
            }
        }

        // Try unquoted: "key":123.45
        search = "\"" + key + "\":";
        pos = json.find(search);
        if (pos == std::string::npos)
            return 0.0;

        size_t start = pos + search.length();
        size_t end = json.find_first_of(",}", start);
        if (end == std::string::npos)
            return 0.0;

        try {
            return std::stod(json.substr(start, end - start));
        } catch (...) {
            return 0.0;
        }
    }

    static uint64_t extract_uint64(const std::string& json, const std::string& key) {
        std::string search = "\"" + key + "\":";
        size_t pos = json.find(search);
        if (pos == std::string::npos)
            return 0;

        size_t start = pos + search.length();
        size_t end = json.find_first_of(",}", start);
        if (end == std::string::npos)
            return 0;

        try {
            return std::stoull(json.substr(start, end - start));
        } catch (...) {
            return 0;
        }
    }

    static bool extract_bool(const std::string& json, const std::string& key) {
        std::string search = "\"" + key + "\":";
        size_t pos = json.find(search);
        if (pos == std::string::npos)
            return false;

        size_t start = pos + search.length();
        return (json.compare(start, 4, "true") == 0);
    }

    // WebSocket callback (static, called by libwebsockets)
#ifdef LWS_VERSION_4_3_PLUS
    static constexpr int LWS_CLIENT_RECEIVE_COMPAT = 71;
#else
    static constexpr int LWS_CLIENT_RECEIVE_COMPAT = LWS_CALLBACK_CLIENT_RECEIVE;
#endif

    static int ws_callback(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in, size_t len) {
        BinanceFuturesWs* self = static_cast<BinanceFuturesWs*>(lws_context_user(lws_get_context(wsi)));

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
    }

    // Event loop thread
    void run_event_loop() {
        // Create context
        struct lws_context_creation_info info;
        memset(&info, 0, sizeof(info));

        static const struct lws_protocols protocols[] = {{
                                                             "binance-futures-ws", ws_callback, 0,
                                                             65536, // rx buffer size
                                                         },
                                                         {nullptr, nullptr, 0, 0}};

        info.port = CONTEXT_PORT_NO_LISTEN;
        info.protocols = protocols;
        info.gid = -1;
        info.uid = -1;
        info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        info.user = this;
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

        std::string path = build_stream_path_internal();

        ccinfo.context = context_;
        ccinfo.address = host_.c_str();
        ccinfo.port = port_;
        ccinfo.path = path.c_str();
        ccinfo.host = host_.c_str();
        ccinfo.origin = host_.c_str();
        ccinfo.protocol = protocols[0].name;
        ccinfo.ssl_connection = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK |
                                LCCSCF_ALLOW_EXPIRED | LCCSCF_ALLOW_INSECURE;

        wsi_ = lws_client_connect_via_info(&ccinfo);
        if (!wsi_) {
            if (error_callback_) {
                error_callback_("Failed to connect to WebSocket");
            }
            return;
        }

        // Event loop
        while (running_) {
            lws_service(context_, 100); // 100ms timeout
        }
    }
};

} // namespace exchange
} // namespace hft
