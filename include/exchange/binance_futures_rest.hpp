#pragma once

#include "futures_market_data.hpp"
#include "market_data.hpp" // For Kline

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace hft {
namespace exchange {

using json = nlohmann::json;

/**
 * Binance Futures REST API client
 *
 * Uses libcurl for HTTP requests to Binance USD-M Futures API.
 * This is NOT for hot path - only for data download/backfill.
 *
 * Endpoints:
 * - /fapi/v1/premiumIndex - Mark price + funding rate
 * - /fapi/v1/fundingRate - Funding rate history
 * - /fapi/v1/openInterest - Current open interest
 * - /futures/data/openInterestHist - OI history
 * - /fapi/v1/klines - Klines (OHLCV)
 */
class BinanceFuturesRest {
public:
    // API base URLs
    static constexpr const char* MAINNET = "https://fapi.binance.com";
    static constexpr const char* TESTNET = "https://testnet.binancefuture.com";

    // Kline intervals
    static constexpr const char* INTERVAL_1m = "1m";
    static constexpr const char* INTERVAL_5m = "5m";
    static constexpr const char* INTERVAL_15m = "15m";
    static constexpr const char* INTERVAL_1h = "1h";
    static constexpr const char* INTERVAL_4h = "4h";
    static constexpr const char* INTERVAL_1d = "1d";

    // Open interest history periods
    static constexpr const char* PERIOD_5m = "5m";
    static constexpr const char* PERIOD_15m = "15m";
    static constexpr const char* PERIOD_30m = "30m";
    static constexpr const char* PERIOD_1h = "1h";
    static constexpr const char* PERIOD_2h = "2h";
    static constexpr const char* PERIOD_4h = "4h";
    static constexpr const char* PERIOD_6h = "6h";
    static constexpr const char* PERIOD_12h = "12h";
    static constexpr const char* PERIOD_1d = "1d";

    explicit BinanceFuturesRest(bool use_testnet = false) : base_url_(use_testnet ? TESTNET : MAINNET), curl_(nullptr) { // LCOV_EXCL_START - Network I/O: libcurl initialization
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_ = curl_easy_init();
        if (!curl_) {
            throw std::runtime_error("Failed to initialize CURL");
        }
    } // LCOV_EXCL_STOP

    ~BinanceFuturesRest() { // LCOV_EXCL_START - Network I/O: libcurl cleanup
        if (curl_) {
            curl_easy_cleanup(curl_);
        }
        curl_global_cleanup();
    } // LCOV_EXCL_STOP

    // Non-copyable
    BinanceFuturesRest(const BinanceFuturesRest&) = delete;
    BinanceFuturesRest& operator=(const BinanceFuturesRest&) = delete;

    /**
     * Fetch current funding rate
     */
    FundingRate fetch_funding_rate(const std::string& symbol) { // LCOV_EXCL_START - Network I/O: HTTP request
        std::string url = base_url_ + build_funding_rate_url(symbol);
        std::string response = http_get(url);
        return parse_funding_rate_json(response);
    } // LCOV_EXCL_STOP

    /**
     * Fetch funding rate history
     */
    std::vector<FundingRate> fetch_funding_rate_history(const std::string& symbol, Timestamp start_time = 0,
                                                        Timestamp end_time = 0, int limit = 100) { // LCOV_EXCL_START - Network I/O: HTTP request
        std::string url = base_url_ + build_funding_rate_history_url(symbol, start_time, end_time, limit);
        std::string response = http_get(url);
        return parse_funding_rate_history_json(response);
    } // LCOV_EXCL_STOP

    /**
     * Fetch current open interest
     */
    OpenInterest fetch_open_interest(const std::string& symbol) { // LCOV_EXCL_START - Network I/O: HTTP request
        std::string url = base_url_ + build_open_interest_url(symbol);
        std::string response = http_get(url);
        return parse_open_interest_json(response);
    } // LCOV_EXCL_STOP

    /**
     * Fetch open interest history
     */
    std::vector<OpenInterest> fetch_open_interest_history(const std::string& symbol, const std::string& period = "5m",
                                                          Timestamp start_time = 0, Timestamp end_time = 0,
                                                          int limit = 30) { // LCOV_EXCL_START - Network I/O: HTTP request
        std::string url = base_url_ + build_open_interest_history_url(symbol, period, start_time, end_time, limit);
        std::string response = http_get(url);
        return parse_open_interest_history_json(response);
    } // LCOV_EXCL_STOP

    /**
     * Fetch mark price
     */
    MarkPriceUpdate fetch_mark_price(const std::string& symbol) { // LCOV_EXCL_START - Network I/O: HTTP request
        std::string url = base_url_ + build_mark_price_url(symbol);
        std::string response = http_get(url);
        return parse_mark_price_json(response);
    } // LCOV_EXCL_STOP

    /**
     * Fetch klines (candlestick data)
     */
    std::vector<Kline> fetch_klines(const std::string& symbol, const std::string& interval, Timestamp start_time = 0,
                                    Timestamp end_time = 0, int limit = 500) { // LCOV_EXCL_START - Network I/O: HTTP request
        std::string url = base_url_ + build_klines_url(symbol, interval, start_time, end_time, limit);
        std::string response = http_get(url);
        return parse_klines_json(response);
    } // LCOV_EXCL_STOP

    /**
     * Fetch all klines in a time range (handles pagination)
     */
    std::vector<Kline> fetch_klines_range(const std::string& symbol, const std::string& interval, Timestamp start_time,
                                          Timestamp end_time) { // LCOV_EXCL_START - Network I/O: HTTP request with pagination
        std::vector<Kline> all_klines;
        Timestamp current_start = start_time;
        const int limit = 1000; // Max per request

        while (current_start < end_time) {
            auto batch = fetch_klines(symbol, interval, current_start, end_time, limit);

            if (batch.empty()) {
                break; // No more data
            }

            all_klines.insert(all_klines.end(), batch.begin(), batch.end());

            // Move start to after last kline
            current_start = batch.back().close_time + 1;

            // Rate limiting - Binance allows 2400 requests/min for futures
            // Sleep 100ms between requests to be safe
            usleep(100000);
        }

        return all_klines;
    } // LCOV_EXCL_STOP

    // ========================================
    // Static URL Builders (for testing)
    // ========================================

    static std::string build_funding_rate_url(const std::string& symbol) {
        return "/fapi/v1/premiumIndex?symbol=" + symbol;
    }

    static std::string build_funding_rate_history_url(const std::string& symbol, Timestamp start_time,
                                                      Timestamp end_time, int limit) {
        std::stringstream url;
        url << "/fapi/v1/fundingRate?symbol=" << symbol;
        if (start_time > 0) {
            url << "&startTime=" << start_time;
        }
        if (end_time > 0) {
            url << "&endTime=" << end_time;
        }
        url << "&limit=" << limit;
        return url.str();
    }

    static std::string build_open_interest_url(const std::string& symbol) {
        return "/fapi/v1/openInterest?symbol=" + symbol;
    }

    static std::string build_open_interest_history_url(const std::string& symbol, const std::string& period,
                                                       Timestamp start_time, Timestamp end_time, int limit) {
        std::stringstream url;
        url << "/futures/data/openInterestHist?symbol=" << symbol << "&period=" << period;
        if (start_time > 0) {
            url << "&startTime=" << start_time;
        }
        if (end_time > 0) {
            url << "&endTime=" << end_time;
        }
        url << "&limit=" << limit;
        return url.str();
    }

    static std::string build_mark_price_url(const std::string& symbol) {
        return "/fapi/v1/premiumIndex?symbol=" + symbol;
    }

    static std::string build_klines_url(const std::string& symbol, const std::string& interval, Timestamp start_time,
                                        Timestamp end_time, int limit) {
        std::stringstream url;
        url << "/fapi/v1/klines?symbol=" << symbol << "&interval=" << interval;
        if (start_time > 0) {
            url << "&startTime=" << start_time;
        }
        if (end_time > 0) {
            url << "&endTime=" << end_time;
        }
        url << "&limit=" << limit;
        return url.str();
    }

    // ========================================
    // Static JSON Parsers (for testing)
    // ========================================

    static FundingRate parse_funding_rate_json(const std::string& response) {
        FundingRate fr;
        try {
            json data = json::parse(response);
            fr.symbol = data.value("symbol", "");
            fr.mark_price = parse_double_field(data, "markPrice");
            fr.funding_rate = parse_double_field(data, "lastFundingRate");
            fr.funding_time = data.value("nextFundingTime", 0ULL);
            fr.event_time = data.value("time", 0ULL);
        } catch (const json::exception&) {
            // Return empty on parse error
        }
        return fr;
    }

    static std::vector<FundingRate> parse_funding_rate_history_json(const std::string& response) {
        std::vector<FundingRate> history;
        try {
            json data = json::parse(response);
            for (const auto& item : data) {
                FundingRate fr;
                fr.symbol = item.value("symbol", "");
                fr.funding_rate = parse_double_field(item, "fundingRate");
                fr.funding_time = item.value("fundingTime", 0ULL);
                history.push_back(fr);
            }
        } catch (const json::exception&) {
            // Return empty on parse error
        }
        return history;
    }

    static OpenInterest parse_open_interest_json(const std::string& response) {
        OpenInterest oi;
        try {
            json data = json::parse(response);
            oi.symbol = data.value("symbol", "");
            oi.open_interest = parse_double_field(data, "openInterest");
            oi.time = data.value("time", 0ULL);
        } catch (const json::exception&) {
            // Return empty on parse error
        }
        return oi;
    }

    static std::vector<OpenInterest> parse_open_interest_history_json(const std::string& response) {
        std::vector<OpenInterest> history;
        try {
            json data = json::parse(response);
            for (const auto& item : data) {
                OpenInterest oi;
                oi.symbol = item.value("symbol", "");
                oi.open_interest = parse_double_field(item, "sumOpenInterest");
                oi.open_interest_value = parse_double_field(item, "sumOpenInterestValue");
                oi.time = item.value("timestamp", 0ULL);
                history.push_back(oi);
            }
        } catch (const json::exception&) {
            // Return empty on parse error
        }
        return history;
    }

    static MarkPriceUpdate parse_mark_price_json(const std::string& response) {
        MarkPriceUpdate mp;
        try {
            json data = json::parse(response);
            mp.symbol = data.value("symbol", "");
            mp.mark_price = parse_double_field(data, "markPrice");
            mp.index_price = parse_double_field(data, "indexPrice");
            mp.funding_rate = parse_double_field(data, "lastFundingRate");
            mp.next_funding_time = data.value("nextFundingTime", 0ULL);
            mp.event_time = data.value("time", 0ULL);
        } catch (const json::exception&) {
            // Return empty on parse error
        }
        return mp;
    }

    static std::vector<Kline> parse_klines_json(const std::string& response) {
        std::vector<Kline> klines;
        try {
            json data = json::parse(response);
            for (const auto& arr : data) {
                if (!arr.is_array() || arr.size() < 11) {
                    continue;
                }

                Kline k;
                k.open_time = arr[0].get<Timestamp>();
                k.open = static_cast<Price>(parse_double_from_json(arr[1]) * 10000);
                k.high = static_cast<Price>(parse_double_from_json(arr[2]) * 10000);
                k.low = static_cast<Price>(parse_double_from_json(arr[3]) * 10000);
                k.close = static_cast<Price>(parse_double_from_json(arr[4]) * 10000);
                k.volume = parse_double_from_json(arr[5]);
                k.close_time = arr[6].get<Timestamp>();
                k.quote_volume = parse_double_from_json(arr[7]);
                k.trades = arr[8].get<uint32_t>();
                k.taker_buy_volume = parse_double_from_json(arr[9]);

                klines.push_back(k);
            }
        } catch (const json::exception&) {
            // Return empty on parse error
        }
        return klines;
    }

private:
    std::string base_url_;
    CURL* curl_;

    // CURL write callback
    static size_t write_callback(void* contents, size_t size, size_t nmemb, std::string* output) { // LCOV_EXCL_START - Network I/O: libcurl callback
        size_t total_size = size * nmemb;
        output->append(static_cast<char*>(contents), total_size);
        return total_size;
    } // LCOV_EXCL_STOP

    // HTTP GET request
    std::string http_get(const std::string& url) { // LCOV_EXCL_START - Network I/O: libcurl HTTP request
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
            throw std::runtime_error(std::string("CURL error: ") + curl_easy_strerror(res));
        }

        // Check HTTP status
        long http_code = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);

        if (http_code != 200) {
            throw std::runtime_error("HTTP error " + std::to_string(http_code) + ": " + response);
        }

        return response;
    } // LCOV_EXCL_STOP

    // Parse double field (handles both string and number types)
    static double parse_double_field(const json& obj, const std::string& key) {
        if (!obj.contains(key)) {
            return 0.0;
        }

        const auto& val = obj[key];
        if (val.is_string()) {
            try {
                return std::stod(val.get<std::string>());
            } catch (...) {
                return 0.0;
            }
        } else if (val.is_number()) {
            return val.get<double>();
        }
        return 0.0;
    }

    // Parse double from JSON value (array element)
    static double parse_double_from_json(const json& val) {
        if (val.is_string()) {
            try {
                return std::stod(val.get<std::string>());
            } catch (...) {
                return 0.0;
            }
        } else if (val.is_number()) {
            return val.get<double>();
        }
        return 0.0;
    }
};

} // namespace exchange
} // namespace hft
