/**
 * RAG Integration Tests
 *
 * TDD-driven tests for RAG service integration.
 * Tests cover:
 * 1. RAG client connectivity
 * 2. Query functionality
 * 3. Response parsing
 * 4. Tuner-RAG integration
 */

#include "../include/tuner/rag_client.hpp"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using namespace hft::tuner;

#define TEST(name) void name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "  " << #name << "... ";                                                                          \
        name();                                                                                                        \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

#define SKIP_TEST(name, reason)                                                                                        \
    do {                                                                                                               \
        std::cout << "  " << #name << "... SKIPPED (" << reason << ")\n";                                              \
    } while (0)

#define ASSERT_EQ(a, b) assert((a) == (b))
#define ASSERT_NE(a, b) assert((a) != (b))
#define ASSERT_TRUE(x) assert(x)
#define ASSERT_FALSE(x) assert(!(x))
#define ASSERT_GT(a, b) assert((a) > (b))
#define ASSERT_LT(a, b) assert((a) < (b))
#define ASSERT_GE(a, b) assert((a) >= (b))

// =============================================================================
// Unit Tests - RAG Client Construction
// =============================================================================

TEST(client_constructs_with_default_url) {
    // Clear env variable first
    unsetenv("RAG_SERVICE_URL");

    RagClient client;
    ASSERT_TRUE(client.is_valid());
    ASSERT_EQ(client.base_url(), "http://localhost:9528");
}

TEST(client_constructs_with_custom_url) {
    RagClient client("http://custom-host:8080");
    ASSERT_TRUE(client.is_valid());
    ASSERT_EQ(client.base_url(), "http://custom-host:8080");
}

TEST(client_constructs_from_environment_variable) {
    // Save current env
    const char* old_val = std::getenv("RAG_SERVICE_URL");

    // Set test env
    setenv("RAG_SERVICE_URL", "http://env-host:9999", 1);

    RagClient client;
    ASSERT_EQ(client.base_url(), "http://env-host:9999");

    // Restore env
    if (old_val) {
        setenv("RAG_SERVICE_URL", old_val, 1);
    } else {
        unsetenv("RAG_SERVICE_URL");
    }
}

// =============================================================================
// Unit Tests - Response Parsing
// =============================================================================

TEST(parses_valid_query_response) {
    const std::string json_response = R"({
        "context": "## Market Regimes\n\nTrending markets show...",
        "sources": ["knowledge/market_regimes.md", "include/strategy/regime_detector.hpp"],
        "n_chunks": 3
    })";

    RagQueryResponse response;
    bool success = RagClient::parse_query_response(json_response, response);

    ASSERT_TRUE(success);
    ASSERT_FALSE(response.context.empty());
    ASSERT_TRUE(response.context.find("Market Regimes") != std::string::npos);
    ASSERT_EQ(response.sources.size(), 2u);
    ASSERT_EQ(response.n_chunks, 3);
}

TEST(parses_empty_sources_array) {
    const std::string json_response = R"({
        "context": "No relevant information found.",
        "sources": [],
        "n_chunks": 0
    })";

    RagQueryResponse response;
    bool success = RagClient::parse_query_response(json_response, response);

    ASSERT_TRUE(success);
    ASSERT_EQ(response.sources.size(), 0u);
    ASSERT_EQ(response.n_chunks, 0);
}

TEST(handles_invalid_json) {
    const std::string invalid_json = "not a json";

    RagQueryResponse response;
    bool success = RagClient::parse_query_response(invalid_json, response);

    ASSERT_FALSE(success);
}

TEST(handles_missing_fields) {
    const std::string partial_json = R"({"context": "some text"})";

    RagQueryResponse response;
    bool success = RagClient::parse_query_response(partial_json, response);

    // Should handle gracefully - context is present
    ASSERT_TRUE(success);
    ASSERT_EQ(response.context, "some text");
    ASSERT_EQ(response.sources.size(), 0u); // Default empty
    ASSERT_EQ(response.n_chunks, 0);        // Default 0
}

// =============================================================================
// Unit Tests - Health Response Parsing
// =============================================================================

TEST(parses_health_response) {
    const std::string json = R"({
        "status": "healthy",
        "collection_size": 42,
        "model": "all-MiniLM-L6-v2"
    })";

    RagHealthResponse response;
    bool success = RagClient::parse_health_response(json, response);

    ASSERT_TRUE(success);
    ASSERT_TRUE(response.is_healthy);
    ASSERT_EQ(response.collection_size, 42);
    ASSERT_EQ(response.model, "all-MiniLM-L6-v2");
}

TEST(detects_unhealthy_status) {
    const std::string json = R"({
        "status": "unhealthy",
        "collection_size": 0,
        "model": ""
    })";

    RagHealthResponse response;
    bool success = RagClient::parse_health_response(json, response);

    ASSERT_TRUE(success);
    ASSERT_FALSE(response.is_healthy);
}

// =============================================================================
// Integration Tests - Require Running RAG Server
// =============================================================================

// Global flag for server availability
static bool g_rag_server_available = false;

void check_rag_server() {
    RagClient client("http://localhost:9528");
    auto result = client.health_check();
    g_rag_server_available = result.success;
    if (!g_rag_server_available) {
        std::cout << "\n  [INFO] RAG server not available at localhost:9528\n";
        std::cout << "  [INFO] Integration tests will be skipped\n";
        std::cout << "  [INFO] Start server with: cd rag_service && python rag_server.py\n\n";
    }
}

TEST(health_check_returns_status) {
    if (!g_rag_server_available) {
        std::cout << "SKIPPED (server not available)\n";
        return;
    }

    RagClient client("http://localhost:9528");
    auto result = client.health_check();

    ASSERT_TRUE(result.success);
    ASSERT_TRUE(result.is_healthy);
    ASSERT_GT(result.collection_size, 0); // Knowledge base should have documents
    ASSERT_FALSE(result.model.empty());
}

TEST(query_returns_relevant_context) {
    if (!g_rag_server_available) {
        std::cout << "SKIPPED (server not available)\n";
        return;
    }

    RagClient client("http://localhost:9528");

    RagQueryRequest request;
    request.query = "What parameters should I tune in a trending market?";
    request.regime = "TRENDING_UP";
    request.n_results = 3;

    auto result = client.query(request);

    ASSERT_TRUE(result.success);
    ASSERT_FALSE(result.context.empty());
    ASSERT_GT(result.n_chunks, 0);

    // Verify context contains relevant information
    bool has_trending_info = result.context.find("trending") != std::string::npos ||
                             result.context.find("TRENDING") != std::string::npos ||
                             result.context.find("Trending") != std::string::npos;
    ASSERT_TRUE(has_trending_info);
}

TEST(query_with_symbol_filter) {
    if (!g_rag_server_available) {
        std::cout << "SKIPPED (server not available)\n";
        return;
    }

    RagClient client("http://localhost:9528");

    RagQueryRequest request;
    request.query = "What are the risk parameters?";
    request.symbol = "BTCUSDT";
    request.n_results = 2;

    auto result = client.query(request);

    ASSERT_TRUE(result.success);
    ASSERT_FALSE(result.context.empty());
}

TEST(query_returns_source_references) {
    if (!g_rag_server_available) {
        std::cout << "SKIPPED (server not available)\n";
        return;
    }

    RagClient client("http://localhost:9528");

    RagQueryRequest request;
    request.query = "market regime detection";
    request.n_results = 5;

    auto result = client.query(request);

    ASSERT_TRUE(result.success);
    ASSERT_GT(result.sources.size(), 0u); // Should return source references

    // Verify sources are valid paths
    for (const auto& source : result.sources) {
        ASSERT_FALSE(source.empty());
        bool valid_path = source.find("knowledge/") == 0 || source.find("include/") == 0;
        ASSERT_TRUE(valid_path);
    }
}

TEST(measures_query_latency) {
    if (!g_rag_server_available) {
        std::cout << "SKIPPED (server not available)\n";
        return;
    }

    RagClient client("http://localhost:9528");

    RagQueryRequest request;
    request.query = "parameter tuning guidelines";
    request.n_results = 3;

    auto start = std::chrono::steady_clock::now();
    auto result = client.query(request);
    auto end = std::chrono::steady_clock::now();

    auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    ASSERT_TRUE(result.success);
    ASSERT_GT(result.latency_ms, 0u);
    ASSERT_LT(result.latency_ms, 5000u); // Query should complete within 5 seconds
}

TEST(builds_tuner_context_from_rag) {
    if (!g_rag_server_available) {
        std::cout << "SKIPPED (server not available)\n";
        return;
    }

    RagClient client("http://localhost:9528");

    // Simulate tuner requesting context for a specific scenario
    std::string tuner_context = client.build_tuner_context("BTCUSDT", "TRENDING_UP",
                                                           3,   // consecutive losses
                                                           45.0 // win rate
    );

    ASSERT_FALSE(tuner_context.empty());

    // Context should include relevant tuning advice
    bool has_relevant_content =
        tuner_context.find("position") != std::string::npos || tuner_context.find("EMA") != std::string::npos ||
        tuner_context.find("loss") != std::string::npos || tuner_context.find("Regime") != std::string::npos;

    ASSERT_TRUE(has_relevant_content);
}

TEST(handles_high_volatility_regime_query) {
    if (!g_rag_server_available) {
        std::cout << "SKIPPED (server not available)\n";
        return;
    }

    RagClient client("http://localhost:9528");

    RagQueryRequest request;
    request.query = "How to adjust parameters in high volatility?";
    request.regime = "HIGH_VOLATILITY";

    auto result = client.query(request);

    ASSERT_TRUE(result.success);

    // In high volatility, knowledge base should recommend defensive settings
    bool has_volatility_advice = result.context.find("volatility") != std::string::npos ||
                                 result.context.find("Volatility") != std::string::npos ||
                                 result.context.find("position") != std::string::npos ||
                                 result.context.find("reduce") != std::string::npos ||
                                 result.context.find("defensive") != std::string::npos;

    ASSERT_TRUE(has_volatility_advice);
}

// =============================================================================
// Error Handling Tests
// =============================================================================

TEST(handles_connection_refused) {
    RagClient client("http://localhost:9999"); // Non-existent server

    auto result = client.health_check();

    ASSERT_FALSE(result.success);
    ASSERT_FALSE(result.error.empty());
}

TEST(handles_invalid_host) {
    RagClient client("http://invalid-host-that-does-not-exist:9528");

    auto result = client.health_check();

    ASSERT_FALSE(result.success);
    ASSERT_FALSE(result.error.empty());
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "\n=== RAG Integration Tests ===\n\n";

    std::cout << "Unit Tests - Client Construction:\n";
    RUN_TEST(client_constructs_with_default_url);
    RUN_TEST(client_constructs_with_custom_url);
    RUN_TEST(client_constructs_from_environment_variable);

    std::cout << "\nUnit Tests - Response Parsing:\n";
    RUN_TEST(parses_valid_query_response);
    RUN_TEST(parses_empty_sources_array);
    RUN_TEST(handles_invalid_json);
    RUN_TEST(handles_missing_fields);

    std::cout << "\nUnit Tests - Health Response Parsing:\n";
    RUN_TEST(parses_health_response);
    RUN_TEST(detects_unhealthy_status);

    std::cout << "\nChecking RAG server availability...\n";
    check_rag_server();

    std::cout << "\nIntegration Tests (requires RAG server):\n";
    RUN_TEST(health_check_returns_status);
    RUN_TEST(query_returns_relevant_context);
    RUN_TEST(query_with_symbol_filter);
    RUN_TEST(query_returns_source_references);
    RUN_TEST(measures_query_latency);
    RUN_TEST(builds_tuner_context_from_rag);
    RUN_TEST(handles_high_volatility_regime_query);

    std::cout << "\nError Handling Tests:\n";
    RUN_TEST(handles_connection_refused);
    RUN_TEST(handles_invalid_host);

    std::cout << "\n=== All tests completed ===\n\n";
    return 0;
}
