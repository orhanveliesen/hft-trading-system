#pragma once

/**
 * News Client for HFT Tuner
 *
 * Fetches crypto news from various sources to provide context
 * for AI-driven parameter tuning decisions.
 *
 * Sources:
 *   - CryptoPanic API (free tier available)
 *   - Binance announcements
 *
 * Environment:
 *   CRYPTOPANIC_API_KEY - Optional API key for CryptoPanic
 */

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <curl/curl.h>

namespace hft {
namespace tuner {

// News item structure
struct NewsItem {
    char title[256];
    char source[32];
    char url[256];
    char currencies[64];      // Comma-separated: "BTC,ETH,SOL"
    int64_t published_at;     // Unix timestamp
    int8_t sentiment;         // -1=bearish, 0=neutral, 1=bullish
    uint8_t importance;       // 0-100 score

    bool affects_symbol(const char* symbol) const {
        // Check if symbol matches any currency in the news
        // Symbol format: "BTCUSDT" -> extract "BTC"
        char base[8] = {0};
        size_t len = std::strlen(symbol);
        if (len >= 6) {
            // Remove "USDT" suffix if present
            size_t base_len = len;
            if (len > 4 && std::strcmp(symbol + len - 4, "USDT") == 0) {
                base_len = len - 4;
            }
            std::strncpy(base, symbol, std::min(base_len, size_t(7)));
        }

        return std::strstr(currencies, base) != nullptr;
    }

    // Age in seconds
    int64_t age_seconds() const {
        return std::time(nullptr) - published_at;
    }

    bool is_recent(int max_age_seconds = 3600) const {
        return age_seconds() <= max_age_seconds;
    }
};

// News feed result
struct NewsFeed {
    std::vector<NewsItem> items;
    int64_t fetched_at;
    bool success;
    char error[128];

    NewsFeed() : fetched_at(0), success(false) {
        error[0] = '\0';
    }

    // Filter news for a specific symbol
    std::vector<const NewsItem*> for_symbol(const char* symbol) const {
        std::vector<const NewsItem*> result;
        for (const auto& item : items) {
            if (item.affects_symbol(symbol)) {
                result.push_back(&item);
            }
        }
        return result;
    }

    // Get recent news (last N minutes)
    std::vector<const NewsItem*> recent(int minutes = 60) const {
        std::vector<const NewsItem*> result;
        int64_t max_age = minutes * 60;
        for (const auto& item : items) {
            if (item.age_seconds() <= max_age) {
                result.push_back(&item);
            }
        }
        return result;
    }

    // Summary for AI prompt
    std::string summary_for_prompt(int max_items = 5) const {
        std::string result;
        int count = 0;
        for (const auto& item : items) {
            if (count >= max_items) break;
            if (!item.is_recent(7200)) continue;  // Last 2 hours only

            result += "- [";
            result += item.sentiment < 0 ? "BEARISH" :
                      item.sentiment > 0 ? "BULLISH" : "NEUTRAL";
            result += "] ";
            result += item.title;
            result += " (";
            result += item.currencies;
            result += ", ";
            result += std::to_string(item.age_seconds() / 60);
            result += "m ago)\n";
            count++;
        }
        return result.empty() ? "No recent news.\n" : result;
    }
};

// Curl write callback
static size_t news_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    std::string* response = static_cast<std::string*>(userdata);
    size_t total = size * nmemb;
    response->append(ptr, total);
    return total;
}

/**
 * News Client
 * Fetches and parses crypto news from multiple sources
 */
class NewsClient {
public:
    NewsClient() {
        // Get API key from environment
        const char* key = std::getenv("CRYPTOPANIC_API_KEY");
        if (key) {
            api_key_ = key;
        }

        curl_ = curl_easy_init();
        if (curl_) {
            curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, news_write_callback);
            curl_easy_setopt(curl_, CURLOPT_USERAGENT, "HFT-Tuner/1.0");
            curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 1L);
        }
    }

    ~NewsClient() {
        if (curl_) {
            curl_easy_cleanup(curl_);
        }
    }

    bool is_valid() const { return curl_ != nullptr; }

    /**
     * Fetch news from CryptoPanic
     * Returns recent crypto news with sentiment
     */
    NewsFeed fetch_cryptopanic(const char* filter = "hot") {
        NewsFeed feed;

        if (!curl_) {
            std::strcpy(feed.error, "CURL not initialized");
            return feed;
        }

        // Build URL
        std::string url = "https://cryptopanic.com/api/v1/posts/";
        url += "?auth_token=";
        url += api_key_.empty() ? "free" : api_key_;
        url += "&filter=";
        url += filter;
        url += "&currencies=BTC,ETH,SOL,XRP,BNB,DOGE";

        std::string response;
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);

        CURLcode res = curl_easy_perform(curl_);

        if (res != CURLE_OK) {
            std::snprintf(feed.error, sizeof(feed.error),
                         "CURL error: %s", curl_easy_strerror(res));
            return feed;
        }

        long http_code = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);

        if (http_code != 200) {
            std::snprintf(feed.error, sizeof(feed.error),
                         "HTTP %ld", http_code);
            return feed;
        }

        // Parse response
        parse_cryptopanic_response(response, feed);
        feed.fetched_at = std::time(nullptr);
        feed.success = true;

        return feed;
    }

    /**
     * Fetch Binance announcements
     * Returns listing/delisting announcements
     */
    NewsFeed fetch_binance_announcements() {
        NewsFeed feed;

        if (!curl_) {
            std::strcpy(feed.error, "CURL not initialized");
            return feed;
        }

        std::string url = "https://www.binance.com/bapi/composite/v1/public/cms/article/list/query"
                          "?type=1&pageNo=1&pageSize=10";

        std::string response;
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);

        CURLcode res = curl_easy_perform(curl_);

        if (res != CURLE_OK) {
            std::snprintf(feed.error, sizeof(feed.error),
                         "CURL error: %s", curl_easy_strerror(res));
            return feed;
        }

        long http_code = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);

        if (http_code != 200) {
            std::snprintf(feed.error, sizeof(feed.error),
                         "HTTP %ld", http_code);
            return feed;
        }

        parse_binance_response(response, feed);
        feed.fetched_at = std::time(nullptr);
        feed.success = true;

        return feed;
    }

    /**
     * Fetch all news sources and merge
     */
    NewsFeed fetch_all() {
        NewsFeed combined;
        combined.success = true;

        // CryptoPanic
        auto cp = fetch_cryptopanic("hot");
        if (cp.success) {
            for (const auto& item : cp.items) {
                combined.items.push_back(item);
            }
        }

        // Binance announcements
        auto ba = fetch_binance_announcements();
        if (ba.success) {
            for (const auto& item : ba.items) {
                combined.items.push_back(item);
            }
        }

        // Sort by published_at descending
        std::sort(combined.items.begin(), combined.items.end(),
                  [](const NewsItem& a, const NewsItem& b) {
                      return a.published_at > b.published_at;
                  });

        combined.fetched_at = std::time(nullptr);
        return combined;
    }

private:
    CURL* curl_ = nullptr;
    std::string api_key_;

    // Parse CryptoPanic JSON response
    void parse_cryptopanic_response(const std::string& json, NewsFeed& feed) {
        // Simple JSON parsing for "results" array
        size_t results_pos = json.find("\"results\"");
        if (results_pos == std::string::npos) return;

        size_t arr_start = json.find('[', results_pos);
        if (arr_start == std::string::npos) return;

        // Parse each item
        size_t pos = arr_start;
        while (pos < json.size() && feed.items.size() < 20) {
            size_t item_start = json.find('{', pos);
            if (item_start == std::string::npos) break;

            // Find matching close brace (simple approach)
            int depth = 1;
            size_t item_end = item_start + 1;
            while (item_end < json.size() && depth > 0) {
                if (json[item_end] == '{') depth++;
                else if (json[item_end] == '}') depth--;
                item_end++;
            }

            if (depth == 0) {
                std::string item_json = json.substr(item_start, item_end - item_start);
                NewsItem item;
                if (parse_cryptopanic_item(item_json, item)) {
                    feed.items.push_back(item);
                }
            }

            pos = item_end;
        }
    }

    bool parse_cryptopanic_item(const std::string& json, NewsItem& item) {
        std::memset(&item, 0, sizeof(item));

        // Extract title
        if (!extract_string(json, "\"title\"", item.title, sizeof(item.title))) {
            return false;
        }

        // Extract source
        size_t source_pos = json.find("\"source\"");
        if (source_pos != std::string::npos) {
            size_t title_pos = json.find("\"title\"", source_pos);
            if (title_pos != std::string::npos) {
                extract_string(json.substr(source_pos, title_pos - source_pos + 100),
                              "\"title\"", item.source, sizeof(item.source));
            }
        }

        // Extract currencies
        std::string currencies;
        size_t curr_pos = json.find("\"currencies\"");
        if (curr_pos != std::string::npos) {
            size_t arr_start = json.find('[', curr_pos);
            size_t arr_end = json.find(']', arr_start);
            if (arr_start != std::string::npos && arr_end != std::string::npos) {
                std::string arr = json.substr(arr_start, arr_end - arr_start);
                // Extract currency codes
                size_t code_pos = 0;
                while ((code_pos = arr.find("\"code\"", code_pos)) != std::string::npos) {
                    char code[16];
                    if (extract_string(arr.substr(code_pos), "\"code\"", code, sizeof(code))) {
                        if (!currencies.empty()) currencies += ",";
                        currencies += code;
                    }
                    code_pos++;
                }
            }
        }
        std::strncpy(item.currencies, currencies.c_str(), sizeof(item.currencies) - 1);

        // Extract published_at
        char published[64];
        if (extract_string(json, "\"published_at\"", published, sizeof(published))) {
            item.published_at = parse_iso_time(published);
        }

        // Extract votes for sentiment
        int positive = 0, negative = 0;
        size_t votes_pos = json.find("\"votes\"");
        if (votes_pos != std::string::npos) {
            char val[16];
            if (extract_string(json.substr(votes_pos), "\"positive\"", val, sizeof(val))) {
                positive = std::atoi(val);
            }
            if (extract_string(json.substr(votes_pos), "\"negative\"", val, sizeof(val))) {
                negative = std::atoi(val);
            }
        }

        if (positive > negative + 2) item.sentiment = 1;
        else if (negative > positive + 2) item.sentiment = -1;
        else item.sentiment = 0;

        // Importance based on kind
        char kind[32];
        if (extract_string(json, "\"kind\"", kind, sizeof(kind))) {
            if (std::strcmp(kind, "news") == 0) item.importance = 70;
            else if (std::strcmp(kind, "media") == 0) item.importance = 50;
            else item.importance = 30;
        }

        std::strcpy(item.source, "CryptoPanic");
        return true;
    }

    // Parse Binance announcements
    void parse_binance_response(const std::string& json, NewsFeed& feed) {
        size_t data_pos = json.find("\"data\"");
        if (data_pos == std::string::npos) return;

        size_t catalogs_pos = json.find("\"catalogs\"", data_pos);
        if (catalogs_pos == std::string::npos) return;

        size_t articles_pos = json.find("\"articles\"", catalogs_pos);
        if (articles_pos == std::string::npos) return;

        size_t arr_start = json.find('[', articles_pos);
        if (arr_start == std::string::npos) return;

        // Parse each article
        size_t pos = arr_start;
        while (pos < json.size() && feed.items.size() < 10) {
            size_t item_start = json.find('{', pos);
            if (item_start == std::string::npos) break;

            int depth = 1;
            size_t item_end = item_start + 1;
            while (item_end < json.size() && depth > 0) {
                if (json[item_end] == '{') depth++;
                else if (json[item_end] == '}') depth--;
                item_end++;
            }

            if (depth == 0) {
                std::string item_json = json.substr(item_start, item_end - item_start);
                NewsItem item;
                if (parse_binance_item(item_json, item)) {
                    feed.items.push_back(item);
                }
            }

            pos = item_end;
        }
    }

    bool parse_binance_item(const std::string& json, NewsItem& item) {
        std::memset(&item, 0, sizeof(item));

        if (!extract_string(json, "\"title\"", item.title, sizeof(item.title))) {
            return false;
        }

        // Extract releaseDate (milliseconds)
        char date_str[32];
        if (extract_string(json, "\"releaseDate\"", date_str, sizeof(date_str))) {
            item.published_at = std::atoll(date_str) / 1000;  // ms to seconds
        }

        // Detect sentiment from title
        const char* title_lower = item.title;
        if (std::strstr(title_lower, "list") || std::strstr(title_lower, "List")) {
            item.sentiment = 1;  // Listing is bullish
            item.importance = 90;
        } else if (std::strstr(title_lower, "delist") || std::strstr(title_lower, "Delist")) {
            item.sentiment = -1;  // Delisting is bearish
            item.importance = 90;
        } else {
            item.sentiment = 0;
            item.importance = 50;
        }

        // Extract currency from title (simple heuristic)
        // Look for patterns like "BTC", "ETH", etc.
        const char* coins[] = {"BTC", "ETH", "SOL", "XRP", "BNB", "DOGE", "ADA", "DOT"};
        std::string currencies;
        for (const char* coin : coins) {
            if (std::strstr(item.title, coin)) {
                if (!currencies.empty()) currencies += ",";
                currencies += coin;
            }
        }
        std::strncpy(item.currencies, currencies.c_str(), sizeof(item.currencies) - 1);

        std::strcpy(item.source, "Binance");
        return true;
    }

    // Helper: extract string value from JSON
    bool extract_string(const std::string& json, const char* key, char* out, size_t out_size) {
        size_t key_pos = json.find(key);
        if (key_pos == std::string::npos) return false;

        size_t colon = json.find(':', key_pos);
        if (colon == std::string::npos) return false;

        // Find opening quote
        size_t start = json.find('"', colon + 1);
        if (start == std::string::npos) return false;
        start++;

        // Find closing quote
        size_t end = start;
        while (end < json.size()) {
            if (json[end] == '"' && json[end-1] != '\\') break;
            end++;
        }

        size_t len = std::min(end - start, out_size - 1);
        std::strncpy(out, json.c_str() + start, len);
        out[len] = '\0';
        return true;
    }

    // Parse ISO 8601 timestamp
    int64_t parse_iso_time(const char* str) {
        struct tm tm = {};
        // Format: 2024-01-15T10:30:00Z
        if (sscanf(str, "%d-%d-%dT%d:%d:%d",
                   &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                   &tm.tm_hour, &tm.tm_min, &tm.tm_sec) >= 3) {
            tm.tm_year -= 1900;
            tm.tm_mon -= 1;
            return timegm(&tm);
        }
        return std::time(nullptr);
    }
};

}  // namespace tuner
}  // namespace hft
