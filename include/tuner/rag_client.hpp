#pragma once

/**
 * RAG Client for HFT Tuner
 *
 * Communicates with the RAG service to retrieve relevant knowledge
 * for parameter tuning decisions.
 *
 * The RAG service provides context from:
 * - Market regime documentation
 * - Parameter tuning guidelines
 * - Strategy overview
 * - C++ header documentation
 */

#include <string>
#include <vector>
#include <sstream>
#include <chrono>
#include <cstring>
#include <curl/curl.h>

namespace hft {
namespace tuner {

// Request structure for RAG queries
struct RagQueryRequest {
    std::string query;
    std::string regime;   // Optional: filter by regime
    std::string symbol;   // Optional: filter by symbol
    int n_results = 5;
};

// Response from RAG query
struct RagQueryResponse {
    bool success = false;
    std::string error;
    std::string context;
    std::vector<std::string> sources;
    int n_chunks = 0;
    uint32_t latency_ms = 0;
};

// Response from health check
struct RagHealthResponse {
    bool success = false;
    std::string error;
    bool is_healthy = false;
    int collection_size = 0;
    std::string model;
};

/**
 * RAG Service Client
 *
 * HTTP client for querying the RAG service.
 * Uses curl for connection pooling and reuse.
 */
class RagClient {
public:
    explicit RagClient(const std::string& base_url = "")
        : timeout_ms_(5000)
    {
        // Check environment variable first
        const char* env_url = std::getenv("RAG_SERVICE_URL");
        if (!base_url.empty()) {
            base_url_ = base_url;
        } else if (env_url) {
            base_url_ = env_url;
        } else {
            base_url_ = "http://localhost:9528";
        }

        // Initialize curl
        curl_ = curl_easy_init();
        if (curl_) {
            curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_callback);
            curl_easy_setopt(curl_, CURLOPT_TCP_KEEPALIVE, 1L);
            curl_easy_setopt(curl_, CURLOPT_TCP_KEEPIDLE, 120L);
        }
    }

    RagClient(const std::string& base_url, uint32_t timeout_ms)
        : timeout_ms_(timeout_ms)
    {
        base_url_ = base_url;
        curl_ = curl_easy_init();
        if (curl_) {
            curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_callback);
            curl_easy_setopt(curl_, CURLOPT_TCP_KEEPALIVE, 1L);
        }
    }

    ~RagClient() {
        if (curl_) {
            curl_easy_cleanup(curl_);
        }
    }

    // Non-copyable
    RagClient(const RagClient&) = delete;
    RagClient& operator=(const RagClient&) = delete;

    bool is_valid() const { return curl_ != nullptr; }
    const std::string& base_url() const { return base_url_; }

    /**
     * Health check - verify RAG service is available
     */
    RagHealthResponse health_check() {
        RagHealthResponse response;

        if (!curl_) {
            response.error = "CURL not initialized";
            return response;
        }

        std::string url = base_url_ + "/health";
        std::string body;

        auto start = std::chrono::steady_clock::now();

        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms_));
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_ms_));

        CURLcode res = curl_easy_perform(curl_);

        if (res != CURLE_OK) {
            response.error = curl_easy_strerror(res);
            return response;
        }

        long http_code = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);

        if (http_code != 200) {
            response.error = "HTTP " + std::to_string(http_code);
            return response;
        }

        response.success = parse_health_response(body, response);
        return response;
    }

    /**
     * Query the knowledge base
     */
    RagQueryResponse query(const RagQueryRequest& request) {
        RagQueryResponse response;

        if (!curl_) {
            response.error = "CURL not initialized";
            return response;
        }

        std::string url = base_url_ + "/query";
        std::string request_body = build_query_json(request);
        std::string response_body;

        auto start = std::chrono::steady_clock::now();

        // Set headers
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_POST, 1L);
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl_, CURLOPT_COPYPOSTFIELDS, request_body.c_str());  // Use COPY to avoid dangling pointer
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response_body);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms_));

        CURLcode res = curl_easy_perform(curl_);

        auto end = std::chrono::steady_clock::now();
        response.latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        curl_slist_free_all(headers);

        // Reset POST state for next request
        curl_easy_setopt(curl_, CURLOPT_POST, 0L);
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, nullptr);

        if (res != CURLE_OK) {
            response.error = curl_easy_strerror(res);
            return response;
        }

        long http_code = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);

        if (http_code != 200) {
            response.error = "HTTP " + std::to_string(http_code);
            return response;
        }

        response.success = parse_query_response(response_body, response);
        return response;
    }

    /**
     * Build tuner context from RAG queries
     *
     * Combines multiple RAG queries to build comprehensive context
     * for the AI tuner based on current trading situation.
     */
    std::string build_tuner_context(
        const std::string& symbol,
        const std::string& regime,
        int consecutive_losses,
        double win_rate)
    {
        std::ostringstream context;

        // Query 1: Regime-specific parameters
        RagQueryRequest regime_query;
        regime_query.query = "What parameters for " + regime + " regime?";
        regime_query.regime = regime;
        regime_query.n_results = 2;

        auto regime_result = query(regime_query);
        if (regime_result.success && !regime_result.context.empty()) {
            context << "## Regime Guidelines (" << regime << ")\n";
            context << regime_result.context << "\n\n";
        }

        // Query 2: Loss recovery if needed
        if (consecutive_losses >= 2) {
            RagQueryRequest loss_query;
            loss_query.query = "How to recover from consecutive losses?";
            loss_query.n_results = 2;

            auto loss_result = query(loss_query);
            if (loss_result.success && !loss_result.context.empty()) {
                context << "## Loss Recovery Guidelines\n";
                context << loss_result.context << "\n\n";
            }
        }

        // Query 3: Win rate optimization
        if (win_rate < 50.0) {
            RagQueryRequest winrate_query;
            winrate_query.query = "How to improve low win rate?";
            winrate_query.n_results = 2;

            auto winrate_result = query(winrate_query);
            if (winrate_result.success && !winrate_result.context.empty()) {
                context << "## Win Rate Optimization\n";
                context << winrate_result.context << "\n\n";
            }
        }

        return context.str();
    }

    /**
     * Parse health check response (static for unit testing)
     */
    static bool parse_health_response(const std::string& json, RagHealthResponse& response) {
        // Find "status" field
        std::string status = extract_string(json, "status");
        response.is_healthy = (status == "healthy");

        // Find "collection_size" field
        response.collection_size = static_cast<int>(extract_number(json, "collection_size"));

        // Find "model" field
        response.model = extract_string(json, "model");

        return true;
    }

    /**
     * Parse query response (static for unit testing)
     */
    static bool parse_query_response(const std::string& json, RagQueryResponse& response) {
        // Verify it looks like JSON
        if (json.find("{") == std::string::npos) {
            return false;
        }

        // Extract context
        response.context = extract_string(json, "context");

        // Extract n_chunks
        response.n_chunks = static_cast<int>(extract_number(json, "n_chunks"));

        // Extract sources array
        size_t sources_pos = json.find("\"sources\"");
        if (sources_pos != std::string::npos) {
            size_t arr_start = json.find("[", sources_pos);
            size_t arr_end = json.find("]", arr_start);

            if (arr_start != std::string::npos && arr_end != std::string::npos) {
                std::string arr = json.substr(arr_start + 1, arr_end - arr_start - 1);

                // Parse array elements
                size_t pos = 0;
                while (pos < arr.size()) {
                    size_t quote_start = arr.find("\"", pos);
                    if (quote_start == std::string::npos) break;

                    size_t quote_end = quote_start + 1;
                    while (quote_end < arr.size()) {
                        if (arr[quote_end] == '"' && arr[quote_end - 1] != '\\') break;
                        quote_end++;
                    }

                    if (quote_end > quote_start + 1) {
                        std::string source = arr.substr(quote_start + 1, quote_end - quote_start - 1);
                        response.sources.push_back(source);
                    }

                    pos = quote_end + 1;
                }
            }
        }

        return true;
    }

private:
    std::string base_url_;
    CURL* curl_ = nullptr;
    uint32_t timeout_ms_;

    static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
        auto* str = static_cast<std::string*>(userp);
        str->append(static_cast<char*>(contents), size * nmemb);
        return size * nmemb;
    }

    std::string build_query_json(const RagQueryRequest& request) {
        std::ostringstream ss;
        ss << "{";
        ss << "\"query\":\"" << escape_json(request.query) << "\"";

        if (!request.regime.empty()) {
            ss << ",\"regime\":\"" << escape_json(request.regime) << "\"";
        }
        if (!request.symbol.empty()) {
            ss << ",\"symbol\":\"" << escape_json(request.symbol) << "\"";
        }

        ss << ",\"n_results\":" << request.n_results;
        ss << "}";

        return ss.str();
    }

    static std::string escape_json(const std::string& str) {
        std::string escaped;
        for (char c : str) {
            switch (c) {
                case '"': escaped += "\\\""; break;
                case '\\': escaped += "\\\\"; break;
                case '\n': escaped += "\\n"; break;
                case '\r': escaped += "\\r"; break;
                case '\t': escaped += "\\t"; break;
                default: escaped += c;
            }
        }
        return escaped;
    }

    static std::string extract_string(const std::string& json, const char* key) {
        std::string search = "\"" + std::string(key) + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return "";

        size_t colon = json.find(":", pos);
        if (colon == std::string::npos) return "";

        // Skip whitespace
        size_t start = colon + 1;
        while (start < json.size() && (json[start] == ' ' || json[start] == '\t' || json[start] == '\n')) {
            start++;
        }

        if (start >= json.size() || json[start] != '"') return "";

        size_t quote_end = start + 1;
        while (quote_end < json.size()) {
            if (json[quote_end] == '"' && json[quote_end - 1] != '\\') break;
            quote_end++;
        }

        std::string value = json.substr(start + 1, quote_end - start - 1);

        // Unescape
        std::string unescaped;
        for (size_t i = 0; i < value.size(); i++) {
            if (value[i] == '\\' && i + 1 < value.size()) {
                switch (value[i + 1]) {
                    case 'n': unescaped += '\n'; i++; break;
                    case 'r': unescaped += '\r'; i++; break;
                    case 't': unescaped += '\t'; i++; break;
                    case '"': unescaped += '"'; i++; break;
                    case '\\': unescaped += '\\'; i++; break;
                    default: unescaped += value[i];
                }
            } else {
                unescaped += value[i];
            }
        }

        return unescaped;
    }

    static double extract_number(const std::string& json, const char* key) {
        std::string search = "\"" + std::string(key) + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return 0;

        size_t colon = json.find(":", pos);
        if (colon == std::string::npos) return 0;

        // Skip whitespace
        size_t num_start = colon + 1;
        while (num_start < json.size() && (json[num_start] == ' ' || json[num_start] == '\t')) {
            num_start++;
        }

        return std::atof(json.c_str() + num_start);
    }
};

}  // namespace tuner
}  // namespace hft
