#pragma once

#include "market_data.hpp"
#include <string>
#include <vector>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <iostream>

namespace hft {
namespace exchange {

using json = nlohmann::json;

/**
 * Binance REST API client for historical data
 *
 * Uses libcurl for HTTP requests.
 * This is NOT for hot path - only for data download/backfill.
 */
class BinanceRest {
public:
    // API base URLs
    static constexpr const char* MAINNET = "https://api.binance.com";
    static constexpr const char* TESTNET = "https://testnet.binance.vision";

    // Kline intervals
    static constexpr const char* INTERVAL_1m = "1m";
    static constexpr const char* INTERVAL_5m = "5m";
    static constexpr const char* INTERVAL_15m = "15m";
    static constexpr const char* INTERVAL_1h = "1h";
    static constexpr const char* INTERVAL_4h = "4h";
    static constexpr const char* INTERVAL_1d = "1d";

    explicit BinanceRest(bool use_testnet = false)
        : base_url_(use_testnet ? TESTNET : MAINNET)
        , curl_(nullptr) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_ = curl_easy_init();
        if (!curl_) {
            throw std::runtime_error("Failed to initialize CURL");
        }
    }

    ~BinanceRest() {
        if (curl_) {
            curl_easy_cleanup(curl_);
        }
        curl_global_cleanup();
    }

    // Non-copyable
    BinanceRest(const BinanceRest&) = delete;
    BinanceRest& operator=(const BinanceRest&) = delete;

    /**
     * Fetch klines (candlestick data)
     *
     * @param symbol Trading pair (e.g., "BTCUSDT")
     * @param interval Kline interval (e.g., "1m", "5m", "1h")
     * @param start_time Start time in milliseconds (optional, 0 = from beginning)
     * @param end_time End time in milliseconds (optional, 0 = until now)
     * @param limit Max klines to fetch (default 500, max 1000)
     * @return Vector of Kline structs
     */
    std::vector<Kline> fetch_klines(
        const std::string& symbol,
        const std::string& interval,
        Timestamp start_time = 0,
        Timestamp end_time = 0,
        int limit = 500
    ) {
        // Build URL
        std::stringstream url;
        url << base_url_ << "/api/v3/klines?symbol=" << symbol
            << "&interval=" << interval
            << "&limit=" << limit;

        if (start_time > 0) {
            url << "&startTime=" << start_time;
        }
        if (end_time > 0) {
            url << "&endTime=" << end_time;
        }

        // Fetch data
        std::string response = http_get(url.str());

        // Parse JSON array of arrays
        return parse_klines_json(response);
    }

    /**
     * Fetch all klines in a time range (handles pagination)
     *
     * @param symbol Trading pair
     * @param interval Kline interval
     * @param start_time Start time in ms
     * @param end_time End time in ms
     * @return Vector of all Kline structs in range
     */
    std::vector<Kline> fetch_klines_range(
        const std::string& symbol,
        const std::string& interval,
        Timestamp start_time,
        Timestamp end_time
    ) {
        std::vector<Kline> all_klines;
        Timestamp current_start = start_time;
        const int limit = 1000;  // Max per request

        while (current_start < end_time) {
            auto batch = fetch_klines(symbol, interval, current_start, end_time, limit);

            if (batch.empty()) {
                break;  // No more data
            }

            all_klines.insert(all_klines.end(), batch.begin(), batch.end());

            // Move start to after last kline
            current_start = batch.back().close_time + 1;

            // Rate limiting - Binance allows 1200 requests/min
            // Sleep 100ms between requests to be safe
            usleep(100000);
        }

        return all_klines;
    }

    /**
     * Get server time (for sync check)
     */
    Timestamp get_server_time() {
        std::string url = std::string(base_url_) + "/api/v3/time";
        std::string response = http_get(url);

        json data = json::parse(response);
        if (!data.contains("serverTime")) {
            throw std::runtime_error("Invalid server time response");
        }

        return data["serverTime"].get<Timestamp>();
    }

    /**
     * Get exchange info (symbols, filters)
     */
    std::string get_exchange_info() {
        std::string url = std::string(base_url_) + "/api/v3/exchangeInfo";
        return http_get(url);
    }

    /**
     * Fetch trading symbols from Binance
     *
     * Fetches exchange info and extracts spot trading pairs that are
     * currently trading, filtered by quote asset.
     *
     * @param quote_asset Filter by quote asset (default "USDT")
     * @return Vector of symbol strings (e.g., "BTCUSDT", "ETHUSDT")
     */
    std::vector<std::string> fetch_trading_symbols(
        const std::string& quote_asset = "USDT"
    ) {
        std::vector<std::string> symbols;

        try {
            std::string response = get_exchange_info();
            json exchange_info = json::parse(response);

            if (!exchange_info.contains("symbols")) {
                return symbols;
            }

            for (const auto& sym : exchange_info["symbols"]) {
                // Check status
                if (!sym.contains("status") || sym["status"] != "TRADING") {
                    continue;
                }

                // Check quote asset
                if (!sym.contains("quoteAsset") || sym["quoteAsset"] != quote_asset) {
                    continue;
                }

                // Check if SPOT trading is permitted
                // Note: Binance API changed - "permissions" array is now empty,
                // use "isSpotTradingAllowed" boolean instead
                bool is_spot = false;
                if (sym.contains("isSpotTradingAllowed")) {
                    is_spot = sym["isSpotTradingAllowed"].get<bool>();
                } else if (sym.contains("permissionSets")) {
                    // Fallback: check permissionSets nested array
                    for (const auto& perm_set : sym["permissionSets"]) {
                        for (const auto& perm : perm_set) {
                            if (perm == "SPOT") {
                                is_spot = true;
                                break;
                            }
                        }
                        if (is_spot) break;
                    }
                }

                if (!is_spot) {
                    continue;
                }

                // Extract symbol name
                if (sym.contains("symbol")) {
                    symbols.push_back(sym["symbol"].get<std::string>());
                }
            }

        } catch (const json::exception& e) {
            // JSON parsing error - return empty vector
            return symbols;
        } catch (const std::exception& e) {
            // Network or other error - return empty vector
            return symbols;
        }

        return symbols;
    }

    /**
     * Get ticker price
     */
    double get_price(const std::string& symbol) {
        std::string url = std::string(base_url_) + "/api/v3/ticker/price?symbol=" + symbol;
        std::string response = http_get(url);

        json data = json::parse(response);
        if (!data.contains("price")) {
            throw std::runtime_error("Invalid price response");
        }

        return std::stod(data["price"].get<std::string>());
    }

private:
    std::string base_url_;
    CURL* curl_;

    // CURL write callback
    static size_t write_callback(void* contents, size_t size, size_t nmemb, std::string* output) {
        size_t total_size = size * nmemb;
        output->append(static_cast<char*>(contents), total_size);
        return total_size;
    }

    // HTTP GET request
    std::string http_get(const std::string& url) {
        std::string response;

        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L);

        // SSL options
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 2L);

        CURLcode res = curl_easy_perform(curl_);

        if (res != CURLE_OK) {
            throw std::runtime_error(
                std::string("CURL error: ") + curl_easy_strerror(res));
        }

        // Check HTTP status
        long http_code = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);

        if (http_code != 200) {
            throw std::runtime_error(
                "HTTP error " + std::to_string(http_code) + ": " + response);
        }

        return response;
    }

    /**
     * Parse Binance klines JSON response
     *
     * Format: [[open_time, "open", "high", "low", "close", "volume",
     *           close_time, "quote_volume", trades, "taker_buy_base",
     *           "taker_buy_quote", "ignore"], ...]
     */
    std::vector<Kline> parse_klines_json(const std::string& response) {
        std::vector<Kline> klines;

        json data = json::parse(response);

        for (const auto& arr : data) {
            if (!arr.is_array() || arr.size() < 11) {
                continue;
            }

            Kline k;
            k.open_time = arr[0].get<Timestamp>();
            k.open = static_cast<Price>(std::stod(arr[1].get<std::string>()) * 10000);
            k.high = static_cast<Price>(std::stod(arr[2].get<std::string>()) * 10000);
            k.low = static_cast<Price>(std::stod(arr[3].get<std::string>()) * 10000);
            k.close = static_cast<Price>(std::stod(arr[4].get<std::string>()) * 10000);
            k.volume = std::stod(arr[5].get<std::string>());
            k.close_time = arr[6].get<Timestamp>();
            k.quote_volume = std::stod(arr[7].get<std::string>());
            k.trades = arr[8].get<uint32_t>();
            k.taker_buy_volume = std::stod(arr[9].get<std::string>());

            klines.push_back(k);
        }

        return klines;
    }
};

/**
 * Default symbol limit for paper trading.
 * MAX_SYMBOLS in portfolio.hpp is 64, but we default to 8 major pairs
 * to avoid memory/performance issues. Use -s flag for specific symbols.
 */
constexpr size_t DEFAULT_SYMBOL_LIMIT = 8;

/**
 * Priority symbols - major trading pairs with high liquidity.
 * These are checked first when selecting symbols from Binance.
 */
inline const std::vector<std::string>& get_priority_symbols() {
    static const std::vector<std::string> priority = {
        "BTCUSDT", "ETHUSDT", "BNBUSDT", "SOLUSDT",
        "XRPUSDT", "ADAUSDT", "DOGEUSDT", "MATICUSDT"
    };
    return priority;
}

/**
 * Fetch default trading symbols from Binance.
 *
 * Returns a limited set of symbols to avoid memory issues.
 * Priority is given to major trading pairs (BTC, ETH, etc.)
 * Limited to DEFAULT_SYMBOL_LIMIT (8) symbols by default.
 *
 * @param limit Maximum number of symbols to return (default: 8)
 * @return Vector of symbol strings (e.g., "BTCUSDT", "ETHUSDT")
 * @throws std::runtime_error if API returns empty response
 */
inline std::vector<std::string> fetch_default_symbols(size_t limit = DEFAULT_SYMBOL_LIMIT) {
    BinanceRest rest(false);  // Use mainnet
    auto all_symbols = rest.fetch_trading_symbols("USDT");

    if (all_symbols.empty()) {
        throw std::runtime_error("Failed to fetch symbols from Binance API: empty response");
    }

    // Build result with priority symbols first
    std::vector<std::string> result;
    result.reserve(limit);

    // Add priority symbols that are available
    const auto& priority = get_priority_symbols();
    for (const auto& sym : priority) {
        if (result.size() >= limit) break;
        auto it = std::find(all_symbols.begin(), all_symbols.end(), sym);
        if (it != all_symbols.end()) {
            result.push_back(sym);
        }
    }

    // Fill remaining slots with other symbols
    for (const auto& sym : all_symbols) {
        if (result.size() >= limit) break;
        if (std::find(result.begin(), result.end(), sym) == result.end()) {
            result.push_back(sym);
        }
    }

    std::cout << "[SYMBOLS] Selected " << result.size()
              << " symbols from " << all_symbols.size()
              << " available USDT trading pairs\n";
    return result;
}

}  // namespace exchange
}  // namespace hft
