#pragma once

#include "market_data.hpp"
#include <string>
#include <vector>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <curl/curl.h>

namespace hft {
namespace exchange {

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

        // Parse {"serverTime":1234567890123}
        size_t pos = response.find("serverTime");
        if (pos == std::string::npos) {
            throw std::runtime_error("Invalid server time response");
        }

        pos = response.find(':', pos);
        size_t end = response.find('}', pos);
        std::string time_str = response.substr(pos + 1, end - pos - 1);

        return std::stoull(time_str);
    }

    /**
     * Get exchange info (symbols, filters)
     */
    std::string get_exchange_info() {
        std::string url = std::string(base_url_) + "/api/v3/exchangeInfo";
        return http_get(url);
    }

    /**
     * Get ticker price
     */
    double get_price(const std::string& symbol) {
        std::string url = std::string(base_url_) + "/api/v3/ticker/price?symbol=" + symbol;
        std::string response = http_get(url);

        // Parse {"symbol":"BTCUSDT","price":"50000.00"}
        size_t pos = response.find("price");
        if (pos == std::string::npos) {
            throw std::runtime_error("Invalid price response");
        }

        pos = response.find('"', pos + 7);  // Skip "price":"
        size_t end = response.find('"', pos + 1);
        std::string price_str = response.substr(pos + 1, end - pos - 1);

        return std::stod(price_str);
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
    std::vector<Kline> parse_klines_json(const std::string& json) {
        std::vector<Kline> klines;

        // Simple JSON array parser
        // Find each inner array [...]
        size_t pos = 0;
        while ((pos = json.find('[', pos + 1)) != std::string::npos) {
            // Skip the outer array start
            if (pos == 0) continue;

            size_t end = json.find(']', pos);
            if (end == std::string::npos) break;

            std::string arr = json.substr(pos + 1, end - pos - 1);

            // Parse comma-separated values
            std::vector<std::string> values;
            std::stringstream ss(arr);
            std::string token;

            while (std::getline(ss, token, ',')) {
                // Remove quotes
                size_t start = 0, len = token.length();
                if (!token.empty() && token[0] == '"') {
                    start = 1;
                    len -= 2;
                }
                values.push_back(token.substr(start, len));
            }

            if (values.size() >= 11) {
                Kline k;
                k.open_time = std::stoull(values[0]);
                k.open = static_cast<Price>(std::stod(values[1]) * 10000);
                k.high = static_cast<Price>(std::stod(values[2]) * 10000);
                k.low = static_cast<Price>(std::stod(values[3]) * 10000);
                k.close = static_cast<Price>(std::stod(values[4]) * 10000);
                k.volume = std::stod(values[5]);
                k.close_time = std::stoull(values[6]);
                k.quote_volume = std::stod(values[7]);
                k.trades = static_cast<uint32_t>(std::stoul(values[8]));
                k.taker_buy_volume = std::stod(values[9]);

                klines.push_back(k);
            }

            pos = end;
        }

        return klines;
    }
};

}  // namespace exchange
}  // namespace hft
