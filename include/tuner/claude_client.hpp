#pragma once

/**
 * Claude API Client for HFT Tuner
 *
 * Communicates with Anthropic's Claude API to get tuning recommendations.
 * Optimized for low latency using connection reuse and haiku model.
 *
 * Response format: JSON with structured tuning command
 */

#include "../ipc/symbol_config.hpp"
#include "../ipc/tuner_event.hpp"
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <cstring>
#include <curl/curl.h>
#include <unistd.h>  // for getcwd

namespace hft {
namespace tuner {

// Response from Claude API
struct ClaudeResponse {
    bool success = false;
    int http_code = 0;
    uint32_t latency_ms = 0;
    uint32_t input_tokens = 0;
    uint32_t output_tokens = 0;
    std::string error;
    std::string raw_response;
    ipc::TunerCommand command;
};

// Cost metrics for AI tuner - CRITICAL for profitability analysis
struct CostMetrics {
    double total_commissions;      // Total commission fees paid
    double total_slippage;         // Total slippage cost
    double total_costs;            // Commission + slippage
    double total_volume;           // Total traded volume
    uint32_t total_fills;          // Total number of fills
    uint32_t total_targets;        // Number of take-profit hits
    uint32_t total_stops;          // Number of stop-loss hits
    double cost_per_trade;         // Average cost per trade
    double avg_trade_value;        // Average trade size
    double cost_pct_per_trade;     // Cost as % of trade value
    double round_trip_cost_pct;    // Round-trip cost (buy+sell)

    // Calculated metrics
    double gross_pnl;              // P&L before costs
    double net_pnl;                // P&L after costs
    double win_rate;               // Win rate %
    double profit_factor;          // Gross profit / Gross loss

    // Trading frequency
    uint32_t session_duration_sec; // How long we've been trading
    double trades_per_hour;        // Fill rate

    // Expected vs Observed costs (CRITICAL for AI to understand)
    double configured_slippage_bps;    // Expected slippage (default: 5 bps)
    double configured_commission_bps;  // Expected commission (default: 10 bps)
    double observed_slippage_bps;      // Actual slippage per fill
    double observed_commission_bps;    // Actual commission per fill
    double expected_round_trip_pct;    // Expected round-trip (default: 0.3%)
    double actual_round_trip_pct;      // Actual observed round-trip

    // Cost efficiency - AI will interpret these
    bool costs_exceed_profits() const { return total_costs > (gross_pnl > 0 ? gross_pnl : 0); }
    bool slippage_exceeds_expected() const { return observed_slippage_bps > configured_slippage_bps * 1.5; }
    double slippage_ratio() const { return configured_slippage_bps > 0 ? observed_slippage_bps / configured_slippage_bps : 0; }
};

// Market snapshot data for AI tuner
struct MarketSnapshotData {
    double price_high;         // Highest price in last ~60s
    double price_low;          // Lowest price in last ~60s
    double price_open;         // Price at start of window
    double ema_20;             // EMA-20
    double atr_14;             // ATR-14
    double volume_sum;         // Total volume in window
    double volatility_pct;     // Volatility as %
    double price_range_pct;    // High-low range as %
    int32_t tick_count;        // Number of ticks in window
    int8_t trend_direction;    // -1=down, 0=flat, 1=up
};

// Symbol data for tuning request
struct SymbolTuningData {
    char symbol[16];
    double current_price;
    double ema_20;
    double price_change_1m;
    double price_change_5m;
    double atr_14;
    int32_t trades_session;
    int32_t wins_session;
    double pnl_session;
    int32_t consecutive_losses;
    int32_t consecutive_wins;
    uint8_t current_regime;
    ipc::SymbolTuningConfig current_config;

    // Market snapshot for rich context
    MarketSnapshotData snapshot;
    bool has_snapshot = false;
};

// Valid Claude model IDs - updated for 2025/2026
static constexpr const char* VALID_CLAUDE_MODELS[] = {
    // Current generation (2025+)
    "claude-opus-4-5-20251101",     // Most capable - complex reasoning (DEFAULT)
    "claude-sonnet-4-5-20241022",   // Balanced performance/cost
    "claude-sonnet-4-20250514",     // Sonnet 4
    "claude-haiku-3-5-20241022",    // Fast model
    // Legacy models (may still work)
    "claude-3-opus-20240229",
    "claude-3-5-sonnet-20241022",
    "claude-3-haiku-20240307",
};
static constexpr size_t VALID_MODEL_COUNT = sizeof(VALID_CLAUDE_MODELS) / sizeof(VALID_CLAUDE_MODELS[0]);

// Default to most capable current model
static constexpr const char* DEFAULT_MODEL = "claude-opus-4-5-20251101";

/**
 * Claude API Client
 *
 * Thread-safe, connection-pooling HTTP client for Anthropic API.
 */
class ClaudeClient {
public:
    ClaudeClient() {
        // Try environment variables first
        const char* env_key = std::getenv("ANTHROPIC_API_KEY");
        if (!env_key) {
            env_key = std::getenv("CLAUDE_API_KEY");
        }

        if (env_key) {
            api_key_ = env_key;
        } else {
            // Fallback: try to load from .env.local file
            api_key_ = load_api_key_from_env_file();
        }

        const char* model_env = std::getenv("HFT_TUNER_MODEL");
        if (model_env) {
            if (is_valid_model(model_env)) {
                model_ = model_env;
            } else {
                std::cerr << "[WARN] Invalid model ID '" << model_env
                          << "' - falling back to " << DEFAULT_MODEL << "\n";
                std::cerr << "[WARN] Valid models: ";
                for (size_t i = 0; i < VALID_MODEL_COUNT; ++i) {
                    std::cerr << VALID_CLAUDE_MODELS[i];
                    if (i < VALID_MODEL_COUNT - 1) std::cerr << ", ";
                }
                std::cerr << "\n";
                model_ = DEFAULT_MODEL;
            }
        } else {
            // Opus has superior math and reasoning for complex trading decisions
            model_ = DEFAULT_MODEL;
        }

        const char* url_env = std::getenv("CLAUDE_API_URL");
        api_url_ = url_env ? url_env : "https://api.anthropic.com/v1/messages";

        // Initialize curl globally (once)
        curl_global_init(CURL_GLOBAL_DEFAULT);

        // Create reusable curl handle
        curl_ = curl_easy_init();
        if (curl_) {
            // Set common options
            curl_easy_setopt(curl_, CURLOPT_URL, api_url_.c_str());
            curl_easy_setopt(curl_, CURLOPT_POST, 1L);
            curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_callback);
            curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 30L);  // 30 second timeout
            curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 10L);

            // Reuse connections
            curl_easy_setopt(curl_, CURLOPT_TCP_KEEPALIVE, 1L);
            curl_easy_setopt(curl_, CURLOPT_TCP_KEEPIDLE, 120L);
            curl_easy_setopt(curl_, CURLOPT_TCP_KEEPINTVL, 60L);
        }
    }

    ~ClaudeClient() {
        if (curl_) {
            curl_easy_cleanup(curl_);
        }
        curl_global_cleanup();
    }

    bool is_valid() const {
        return !api_key_.empty() && curl_ != nullptr;
    }

    const char* model() const { return model_.c_str(); }

    void set_model(const std::string& model) {
        if (model.empty()) return;

        if (is_valid_model(model.c_str())) {
            model_ = model;
        } else {
            std::cerr << "[WARN] set_model: Invalid model ID '" << model
                      << "' - keeping current model " << model_ << "\n";
        }
    }

    /**
     * Check if a model ID is valid
     */
    static bool is_valid_model(const char* model) {
        if (!model) return false;
        for (size_t i = 0; i < VALID_MODEL_COUNT; ++i) {
            if (std::strcmp(model, VALID_CLAUDE_MODELS[i]) == 0) {
                return true;
            }
        }
        return false;
    }

    /**
     * Get list of valid model IDs (for display/help)
     */
    static std::string get_valid_models_list() {
        std::string result;
        for (size_t i = 0; i < VALID_MODEL_COUNT; ++i) {
            if (i > 0) result += ", ";
            result += VALID_CLAUDE_MODELS[i];
        }
        return result;
    }

    /**
     * Request tuning recommendation from Claude
     * @param news_context Optional news summary to include in prompt
     */
    ClaudeResponse request_tuning(
        const std::vector<SymbolTuningData>& symbols,
        double portfolio_pnl,
        double portfolio_cash,
        ipc::TriggerReason trigger,
        const std::string& news_context = "",
        const CostMetrics* costs = nullptr)
    {
        ClaudeResponse response;

        if (!is_valid()) {
            response.error = "Client not initialized or API key missing";
            return response;
        }

        auto start = std::chrono::steady_clock::now();

        // Build the prompt
        std::string prompt = build_prompt(symbols, portfolio_pnl, portfolio_cash, trigger, news_context, costs);

        // Build request JSON
        std::string request_body = build_request_json(prompt);

        // Set headers
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");

        std::string auth_header = "x-api-key: " + api_key_;
        headers = curl_slist_append(headers, auth_header.c_str());

        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, request_body.c_str());

        // Response buffer
        std::string response_body;
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response_body);

        // Make request
        CURLcode res = curl_easy_perform(curl_);

        auto end = std::chrono::steady_clock::now();
        response.latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        curl_slist_free_all(headers);

        if (res != CURLE_OK) {
            response.error = curl_easy_strerror(res);
            return response;
        }

        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &response.http_code);
        response.raw_response = response_body;

        if (response.http_code != 200) {
            response.error = "HTTP " + std::to_string(response.http_code);
            return response;
        }

        // Parse response
        if (!parse_response(response_body, response)) {
            response.error = "Failed to parse response";
            return response;
        }

        response.success = true;
        return response;
    }

private:
    std::string api_key_;
    std::string model_;
    std::string api_url_;
    CURL* curl_ = nullptr;

    /**
     * Load API key from .env.local file
     * Searches in: current dir, parent dir (for build/), project root
     */
    static std::string load_api_key_from_env_file() {
        // Search paths for .env.local
        std::vector<std::string> paths = {
            ".env.local",
            "../.env.local",
            "../../.env.local",
            "/mnt/c/Users/orhan/projects/orhan/hft/.env.local"  // Absolute fallback
        };

        for (const auto& path : paths) {
            std::ifstream file(path);
            if (!file.is_open()) continue;

            std::string line;
            while (std::getline(file, line)) {
                // Skip comments and empty lines
                if (line.empty() || line[0] == '#') continue;

                // Look for CLAUDE_API_KEY or ANTHROPIC_API_KEY
                std::string key;
                if (line.find("CLAUDE_API_KEY=") == 0 || line.find("export CLAUDE_API_KEY=") == 0) {
                    size_t pos = line.find('=');
                    key = line.substr(pos + 1);
                } else if (line.find("ANTHROPIC_API_KEY=") == 0 || line.find("export ANTHROPIC_API_KEY=") == 0) {
                    size_t pos = line.find('=');
                    key = line.substr(pos + 1);
                }

                if (!key.empty()) {
                    // Remove surrounding quotes if present
                    if (key.front() == '"' && key.back() == '"') {
                        key = key.substr(1, key.size() - 2);
                    } else if (key.front() == '\'' && key.back() == '\'') {
                        key = key.substr(1, key.size() - 2);
                    }
                    return key;
                }
            }
        }
        return "";
    }

    static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
        auto* str = static_cast<std::string*>(userp);
        str->append(static_cast<char*>(contents), size * nmemb);
        return size * nmemb;
    }

    std::string build_prompt(
        const std::vector<SymbolTuningData>& symbols,
        double portfolio_pnl,
        double portfolio_cash,
        ipc::TriggerReason trigger,
        const std::string& news_context,
        const CostMetrics* costs = nullptr)
    {
        std::ostringstream ss;

        ss << "You are an HFT parameter tuner. Analyze the trading performance and recommend ONE action.\n\n";

        // CRITICAL: Cold-start handling - prevent hallucination
        bool has_trades = costs && costs->total_fills > 0;
        if (!has_trades) {
            ss << "## 🚨 COLD START - NO TRADING DATA\n";
            ss << "**CRITICAL: There are ZERO trades in this session.**\n";
            ss << "- You MUST return NO_CHANGE because there is no performance data to analyze.\n";
            ss << "- Do NOT make up trade statistics or win rates.\n";
            ss << "- Do NOT recommend parameter changes without real trading data.\n";
            ss << "- Wait for actual trades before making tuning decisions.\n\n";
            ss << "**REQUIRED RESPONSE:**\n";
            ss << "```json\n";
            ss << "{\"action\": \"NO_CHANGE\", \"confidence\": 100, \"reason\": \"No trades to analyze\"}\n";
            ss << "```\n\n";
        }

        ss << "## Current State\n";
        ss << "- Trigger: " << trigger_name(trigger) << "\n";
        ss << "- Portfolio Cash: $" << std::fixed << std::setprecision(2) << portfolio_cash << "\n";
        ss << "- Session P&L: $" << portfolio_pnl << "\n";
        ss << "- **Total Trades This Session: " << (has_trades ? std::to_string(costs->total_fills) : "0") << "**\n\n";

        // CRITICAL: Cost Analysis Section - AI MUST consider this
        if (has_trades) {
            ss << "## ⚠️ COST ANALYSIS (CRITICAL)\n";
            ss << "Trading costs are eating into profits. You MUST consider this.\n\n";
            ss << "**Cost Breakdown:**\n";
            ss << "- Total Commissions: $" << std::setprecision(2) << costs->total_commissions << "\n";
            ss << "- Total Slippage: $" << costs->total_slippage << "\n";
            ss << "- **TOTAL COSTS: $" << costs->total_costs << "**\n";
            ss << "- Cost per trade: $" << std::setprecision(4) << costs->cost_per_trade << "\n";
            ss << "- Avg trade value: $" << std::setprecision(2) << costs->avg_trade_value << "\n";
            ss << "- Cost % per trade: " << std::setprecision(3) << costs->cost_pct_per_trade << "%\n";
            ss << "- Round-trip cost: ~" << (costs->round_trip_cost_pct) << "%\n\n";

            // CRITICAL: Expected vs Observed cost comparison
            ss << "**🔍 EXPECTED vs OBSERVED COSTS (CRITICAL):**\n";
            ss << "| Metric | Expected | Observed | Status |\n";
            ss << "|--------|----------|----------|--------|\n";
            ss << "| Slippage/fill | " << std::setprecision(1) << costs->configured_slippage_bps << " bps | "
               << costs->observed_slippage_bps << " bps | "
               << (costs->slippage_exceeds_expected() ? "⚠️ HIGH" : "✅ OK") << " |\n";
            ss << "| Commission/fill | " << costs->configured_commission_bps << " bps | "
               << costs->observed_commission_bps << " bps | "
               << (costs->observed_commission_bps > costs->configured_commission_bps * 1.1 ? "⚠️" : "✅") << " |\n";
            ss << "| Round-trip | " << costs->expected_round_trip_pct << "% | "
               << std::setprecision(2) << costs->actual_round_trip_pct << "% | "
               << (costs->actual_round_trip_pct > costs->expected_round_trip_pct * 1.3 ? "🚨 TOO HIGH" : "✅") << " |\n\n";

            if (costs->slippage_exceeds_expected()) {
                ss << "**⚠️ SLIPPAGE WARNING:**\n";
                ss << "- Actual slippage is " << std::setprecision(1) << costs->slippage_ratio() << "x higher than expected!\n";
                ss << "- This means targets must be HIGHER to be profitable.\n";
                ss << "- Consider: target_pct should be > " << std::setprecision(2)
                   << (costs->actual_round_trip_pct * 2) << "% to cover costs.\n";
                ss << "- Or: reduce trade frequency to only take high-confidence signals.\n\n";
            }

            if (costs->actual_round_trip_pct > costs->expected_round_trip_pct * 1.5) {
                ss << "**🚨 ROUND-TRIP COST ALERT:**\n";
                ss << "- System expects " << costs->expected_round_trip_pct << "% round-trip cost.\n";
                ss << "- Actual cost is " << costs->actual_round_trip_pct << "% ("
                   << std::setprecision(1) << (costs->actual_round_trip_pct / costs->expected_round_trip_pct)
                   << "x higher).\n";
                ss << "- Current target/stop may be miscalibrated for actual costs.\n";
                ss << "- **Required minimum target: " << std::setprecision(2)
                   << (costs->actual_round_trip_pct * 1.5) << "%** to have positive expectancy.\n\n";
            }

            ss << "**Trading Statistics:**\n";
            ss << "- Total fills: " << costs->total_fills << "\n";
            ss << "- Targets hit: " << costs->total_targets << " (take-profit)\n";
            ss << "- Stops hit: " << costs->total_stops << " (stop-loss)\n";
            ss << "- Win rate: " << std::setprecision(1) << costs->win_rate << "%\n";
            ss << "- Trades per hour: " << std::setprecision(1) << costs->trades_per_hour << "\n";
            ss << "- Session duration: " << (costs->session_duration_sec / 60) << " minutes\n\n";

            ss << "**P&L Impact:**\n";
            ss << "- Gross P&L (before costs): $" << std::setprecision(2) << costs->gross_pnl << "\n";
            ss << "- Net P&L (after costs): $" << costs->net_pnl << "\n\n";

            // Key relationships for AI to analyze
            ss << "**Key Metrics to Analyze:**\n";
            ss << "- Is cost/profit ratio sustainable? (costs should be small relative to gross profits)\n";
            ss << "- Is trade frequency appropriate? (more trades = more costs)\n";
            ss << "- Is win rate adequate for the risk/reward ratio?\n";
            ss << "- Are stops being hit more than targets? (signals may be poor)\n";
            ss << "\nYou must determine appropriate actions based on these metrics.\n\n";

            // CRITICAL: Trade Frequency Warning
            if (costs->trades_per_hour < 5 && costs->session_duration_sec > 1800) {
                ss << "## ⚠️ LOW TRADE FREQUENCY WARNING\n";
                ss << "- Trades per hour: " << std::setprecision(1) << costs->trades_per_hour << "\n";
                ss << "- This is TOO LOW! The EMA thresholds might be too tight.\n";
                ss << "- **ema_dev_trending_pct should be 0.5-2.0%, NOT 3-4%**\n";
                ss << "- **ema_dev_ranging_pct should be 0.3-1.0%**\n";
                ss << "- If no trades happen, we cannot make profit!\n";
                ss << "- Balance: reduce costs BUT keep trading activity\n\n";
            }

            // CRITICAL: Stop/Target Ratio Analysis with Math
            if (costs->total_stops > 0 || costs->total_targets > 0) {
                ss << "## 🚨 STOP/TARGET RATIO ANALYSIS (CRITICAL - READ THIS FIRST)\n";
                double stop_target_ratio = costs->total_targets > 0 ?
                    (double)costs->total_stops / costs->total_targets : 999.0;
                ss << "- Stops hit: " << costs->total_stops << "\n";
                ss << "- Targets hit: " << costs->total_targets << "\n";
                ss << "- **Stop/Target Ratio: " << std::setprecision(1) << stop_target_ratio << ":1**\n\n";

                ss << "**THE MATH (MANDATORY - you MUST apply this):**\n";
                ss << "Break-even formula: Required Win Rate = stop / (stop + target)\n\n";
                ss << "| Stop | Target | Required Win Rate |\n";
                ss << "|------|--------|------------------|\n";
                ss << "| 1%   | 3%     | 75% ← TOO HARD   |\n";
                ss << "| 1%   | 4%     | 80% ← TOO HARD   |\n";
                ss << "| 2%   | 3%     | 60% ← STILL HARD |\n";
                ss << "| 3%   | 3%     | 50% ← ACHIEVABLE |\n";
                ss << "| 4%   | 3%     | 43% ← REALISTIC  |\n";
                ss << "| 5%   | 3%     | 37% ← EASIER     |\n\n";

                ss << "**Current State:**\n";
                ss << "- Win rate: " << std::setprecision(1) << costs->win_rate << "%\n";

                if (stop_target_ratio > 3) {
                    ss << "\n⚠️ **ACTION REQUIRED: Stop is TOO TIGHT!**\n";
                    ss << "With " << std::setprecision(1) << costs->win_rate << "% win rate, you MUST:\n";
                    if (costs->win_rate < 30) {
                        ss << "1. Set stop_pct >= 4% (recommended: 5%)\n";
                        ss << "2. Set target_pct around 2-3%\n";
                        ss << "3. This gives ~60% required win rate which is achievable\n";
                    } else if (costs->win_rate < 50) {
                        ss << "1. Set stop_pct >= 3%\n";
                        ss << "2. Set target_pct around 2-3%\n";
                    } else {
                        ss << "1. Set stop_pct >= 2%\n";
                    }
                    ss << "\n**DO NOT keep stop_pct at 1% - IT DOES NOT WORK!**\n\n";
                }
            }
        }

        // Include news context if provided
        if (!news_context.empty()) {
            ss << "## Recent News\n";
            ss << news_context << "\n";
        }

        ss << "## Symbol Performance\n";
        for (const auto& s : symbols) {
            if (s.symbol[0] == '\0') continue;

            double win_rate = s.trades_session > 0 ?
                (100.0 * s.wins_session / s.trades_session) : 0.0;

            ss << "### " << s.symbol << "\n";
            ss << "- Price: $" << std::setprecision(4) << s.current_price;
            if (s.ema_20 > 0) {
                double ema_dev = (s.current_price - s.ema_20) / s.ema_20 * 100;
                ss << " (EMA20: $" << s.ema_20 << ", dev: " << std::setprecision(2) << ema_dev << "%)";
            }
            ss << "\n";
            ss << "- Trades: " << s.trades_session << ", Win rate: " << std::setprecision(1) << win_rate << "%\n";
            ss << "- P&L: $" << std::setprecision(2) << s.pnl_session << "\n";
            ss << "- Consecutive losses: " << s.consecutive_losses << ", wins: " << s.consecutive_wins << "\n";
            ss << "- Regime: " << regime_name(s.current_regime) << "\n";
            // Full config display for AI tuning
            const auto& cfg = s.current_config;
            const char* order_type_names[] = {"Auto", "MarketOnly", "LimitOnly", "Adaptive"};
            const char* order_type_name = (cfg.order_type_preference < 4) ? order_type_names[cfg.order_type_preference] : "Auto";

            ss << "- Current config (FULL):\n";
            ss << "  - EMA deviation: trending=" << (cfg.ema_dev_trending_x100 / 100.0)
               << "%, ranging=" << (cfg.ema_dev_ranging_x100 / 100.0)
               << "%, highvol=" << (cfg.ema_dev_highvol_x100 / 100.0) << "%\n";
            ss << "  - Position: base=" << (cfg.base_position_x100 / 100.0)
               << "%, max=" << (cfg.max_position_x100 / 100.0)
               << "%, min=" << (cfg.min_position_x100 / 100.0) << "%\n";
            ss << "  - Trade filtering: cooldown=" << cfg.cooldown_ms << "ms, signal_strength=" << (int)cfg.signal_strength << "\n";
            ss << "  - Target/Stop: target=" << (cfg.target_pct_x100 / 100.0)
               << "%, stop=" << (cfg.stop_pct_x100 / 100.0)
               << "%, pullback=" << (cfg.pullback_pct_x100 / 100.0) << "%\n";
            ss << "  - Order type: " << order_type_name
               << ", limit_offset=" << (cfg.limit_offset_bps_x100 / 100.0) << "bps"
               << ", limit_timeout=" << cfg.limit_timeout_ms << "ms\n";
            ss << "  - Mode transitions: losses_to_cautious=" << (int)cfg.losses_to_cautious
               << ", losses_to_defensive=" << (int)cfg.losses_to_defensive
               << ", losses_to_exit_only=" << (int)cfg.losses_to_exit_only
               << ", wins_to_aggressive=" << (int)cfg.wins_to_aggressive << "\n";
            ss << "  - Signal thresholds: aggressive=" << (cfg.signal_aggressive_x100 / 100.0)
               << ", normal=" << (cfg.signal_normal_x100 / 100.0)
               << ", cautious=" << (cfg.signal_cautious_x100 / 100.0)
               << ", min_confidence=" << (cfg.min_confidence_x100 / 100.0) << "\n";
            ss << "  - Accumulation: floor_trend=" << (cfg.accum_floor_trending_x100 / 100.0)
               << ", floor_range=" << (cfg.accum_floor_ranging_x100 / 100.0)
               << ", floor_hvol=" << (cfg.accum_floor_highvol_x100 / 100.0)
               << ", boost_win=" << (cfg.accum_boost_per_win_x100 / 100.0)
               << ", penalty_loss=" << (cfg.accum_penalty_per_loss_x100 / 100.0)
               << ", max=" << (cfg.accum_max_x100 / 100.0) << "\n";

            // Include market snapshot if available
            if (s.has_snapshot) {
                ss << "- Last 60s market data:\n";
                ss << "  - Price range: $" << std::setprecision(2) << s.snapshot.price_low
                   << " - $" << s.snapshot.price_high
                   << " (range: " << std::setprecision(2) << s.snapshot.price_range_pct << "%)\n";
                ss << "  - Volatility: " << std::setprecision(2) << s.snapshot.volatility_pct << "%\n";
                ss << "  - Trend: " << (s.snapshot.trend_direction > 0 ? "UP" :
                                        s.snapshot.trend_direction < 0 ? "DOWN" : "FLAT") << "\n";
                if (s.snapshot.ema_20 > 0) {
                    double ema_dev = (s.current_price - s.snapshot.ema_20) / s.snapshot.ema_20 * 100;
                    ss << "  - EMA-20: $" << std::setprecision(2) << s.snapshot.ema_20
                       << " (current dev: " << std::setprecision(2) << ema_dev << "%)\n";
                }
                if (s.snapshot.atr_14 > 0) {
                    ss << "  - ATR-14: $" << std::setprecision(2) << s.snapshot.atr_14 << "\n";
                }
                ss << "  - Ticks: " << s.snapshot.tick_count << "\n";
            }
            ss << "\n";
        }

        ss << "## Available Actions\n";
        ss << "1. NO_CHANGE - Keep current parameters\n";
        ss << "2. UPDATE_CONFIG <symbol> - Adjust parameters for symbol\n";
        ss << "3. PAUSE <symbol> - Stop trading this symbol\n";
        ss << "4. RESUME <symbol> - Resume trading this symbol\n";
        ss << "5. EMERGENCY_EXIT <symbol> - Close all positions for symbol\n\n";

        ss << "## Response Format\n";
        ss << "Respond with EXACTLY this JSON format (include ONLY fields you want to change):\n";
        ss << "```json\n";
        ss << "{\n";
        ss << "  \"action\": \"NO_CHANGE|UPDATE_CONFIG|PAUSE|RESUME|EMERGENCY_EXIT\",\n";
        ss << "  \"symbol\": \"BTCUSDT\",\n";
        ss << "  \"confidence\": 0-100,\n";
        ss << "  \"urgency\": 0-2,\n";
        ss << "  \"reason\": \"Brief explanation\",\n";
        ss << "  \"config\": {\n";
        ss << "    // EMA deviation thresholds\n";
        ss << "    \"ema_dev_trending_pct\": 1.0,\n";
        ss << "    \"ema_dev_ranging_pct\": 0.5,\n";
        ss << "    \"ema_dev_highvol_pct\": 0.2,\n";
        ss << "    // Position sizing\n";
        ss << "    \"base_position_pct\": 2.0,\n";
        ss << "    \"max_position_pct\": 5.0,\n";
        ss << "    \"min_position_pct\": 0.5,\n";
        ss << "    // Trade filtering\n";
        ss << "    \"cooldown_ms\": 2000,\n";
        ss << "    \"signal_strength\": 2,\n";
        ss << "    // Target/stop\n";
        ss << "    \"target_pct\": 3.0,\n";
        ss << "    \"stop_pct\": 4.0,\n";
        ss << "    \"pullback_pct\": 0.5,\n";
        ss << "    // Order execution\n";
        ss << "    \"order_type\": \"Auto|MarketOnly|LimitOnly|Adaptive\",\n";
        ss << "    \"limit_offset_bps\": 2.0,\n";
        ss << "    \"limit_timeout_ms\": 500,\n";
        ss << "    // Mode transitions (streak-based)\n";
        ss << "    \"losses_to_cautious\": 2,\n";
        ss << "    \"losses_to_defensive\": 4,\n";
        ss << "    \"losses_to_exit_only\": 6,\n";
        ss << "    \"wins_to_aggressive\": 3,\n";
        ss << "    // Signal thresholds by mode (0-1 scale)\n";
        ss << "    \"signal_aggressive\": 0.30,\n";
        ss << "    \"signal_normal\": 0.50,\n";
        ss << "    \"signal_cautious\": 0.70,\n";
        ss << "    \"min_confidence\": 0.30,\n";
        ss << "    // Accumulation control (0-1 scale)\n";
        ss << "    \"accum_floor_trending\": 0.50,\n";
        ss << "    \"accum_floor_ranging\": 0.30,\n";
        ss << "    \"accum_floor_highvol\": 0.20,\n";
        ss << "    \"accum_boost_win\": 0.10,\n";
        ss << "    \"accum_penalty_loss\": 0.10,\n";
        ss << "    \"accum_max\": 0.80\n";
        ss << "  }\n";
        ss << "}\n";
        ss << "```\n\n";

        ss << "## ⚠️ MANDATORY RULES (NEVER VIOLATE)\n";
        ss << "1. **stop_pct MUST be >= 3%** - Tight stops cause excessive losses\n";
        ss << "2. **stop_pct SHOULD be >= target_pct** - With low win rate, stop must be wider than target\n";
        ss << "3. **signal_strength = 1** is fine - We need more trades to capture opportunities\n";
        ss << "4. If win_rate < 30%, set stop_pct = 5% minimum\n\n";

        ss << "## Parameter Meanings (with REALISTIC ranges)\n";
        ss << "**EMA Deviation:**\n";
        ss << "- ema_dev_trending_pct: Max % price can deviate from EMA in uptrend. **REALISTIC: 0.5-2.0%** (NOT 3-4%!)\n";
        ss << "- ema_dev_ranging_pct: Max % price can deviate from EMA in ranging markets. **REALISTIC: 0.3-1.0%**\n";
        ss << "- ema_dev_highvol_pct: Max % deviation in high volatility. **REALISTIC: 0.2-0.5%**\n";
        ss << "\n**Position Sizing:**\n";
        ss << "- base_position_pct: Position size as % of portfolio. **ADJUST BASED ON PERFORMANCE** (see Position Sizing Guidance)\n";
        ss << "- max_position_pct: Max position size. **SCALE UP when profitable** (see Position Sizing Guidance)\n";
        ss << "- min_position_pct: Minimum position size (0.5-2%)\n";
        ss << "\n**Trade Filtering:**\n";
        ss << "- cooldown_ms: Minimum time between trades in milliseconds\n";
        ss << "- signal_strength: Required signal strength (1=Medium, 2=Strong, 3=VeryStrong) - **USE 1 for more trades**\n";
        ss << "\n**Target/Stop:**\n";
        ss << "- target_pct: Take profit threshold as % of entry price (REALISTIC: 2-4%)\n";
        ss << "- stop_pct: Stop loss threshold as % of entry price (REALISTIC: 3-5%, NEVER below 3%)\n";
        ss << "- pullback_pct: Exit when price drops this % from peak unrealized profit\n";
        ss << "\n**Order Execution:**\n";
        ss << "- order_type: Order execution preference for THIS symbol:\n";
        ss << "  - \"Auto\": ExecutionEngine decides based on signal strength, regime, spread\n";
        ss << "  - \"MarketOnly\": Always use market orders (faster execution, accept slippage)\n";
        ss << "  - \"LimitOnly\": Always use limit orders (no slippage, maker rebate, may miss fills)\n";
        ss << "  - \"Adaptive\": Start with limit, convert to market after timeout\n";
        ss << "- limit_offset_bps: For limit orders, how many basis points inside the spread (1-10)\n";
        ss << "- limit_timeout_ms: For Adaptive mode, milliseconds before limit→market (100-5000)\n";
        ss << "\n**Mode Transitions (streak-based):**\n";
        ss << "- losses_to_cautious: Consecutive losses to enter CAUTIOUS mode (default: 2)\n";
        ss << "- losses_to_defensive: Consecutive losses to enter DEFENSIVE mode (default: 4)\n";
        ss << "- losses_to_exit_only: Consecutive losses to enter EXIT_ONLY mode (default: 6)\n";
        ss << "- wins_to_aggressive: Consecutive wins to unlock AGGRESSIVE mode (default: 3)\n";
        ss << "\n**Signal Thresholds by Mode (0-1 scale):**\n";
        ss << "- signal_aggressive: Signal threshold when AGGRESSIVE (default: 0.30 - take more trades)\n";
        ss << "- signal_normal: Signal threshold when NORMAL (default: 0.50)\n";
        ss << "- signal_cautious: Signal threshold when CAUTIOUS (default: 0.70 - more selective)\n";
        ss << "- min_confidence: Minimum signal confidence to trade (default: 0.30)\n";
        ss << "\n**Accumulation Control (0-1 scale) - How to add to existing positions:**\n";
        ss << "- accum_floor_trending: Base accumulation when trending (default: 0.50)\n";
        ss << "- accum_floor_ranging: Base accumulation when ranging (default: 0.30)\n";
        ss << "- accum_floor_highvol: Base accumulation when high volatility (default: 0.20)\n";
        ss << "- accum_boost_win: Bonus per consecutive win (default: 0.10)\n";
        ss << "- accum_penalty_loss: Penalty per consecutive loss (default: 0.10)\n";
        ss << "- accum_max: Maximum accumulation factor (default: 0.80)\n\n";

        ss << "## Decision Guidelines\n";
        ss << "Consider the following relationships when making decisions:\n";
        ss << "- Consecutive losses: May indicate strategy mismatch with current regime\n";
        ss << "- Win rate: Affects required risk/reward ratio to be profitable\n";
        ss << "- Trade frequency vs costs: More trades = more costs, may need longer cooldown\n";
        ss << "- Costs vs profits: If costs eat profits, reduce frequency or pause\n";
        ss << "- Market regime: Different regimes require different parameter settings\n";
        ss << "- Use the data provided to make your own judgment about optimal parameters.\n\n";

        // Position sizing guidance - CRITICAL for capital efficiency
        ss << "## 💰 POSITION SIZING GUIDANCE (CAPITAL EFFICIENCY)\n";
        ss << "**Goal: Maximize capital deployment when strategy is profitable.**\n\n";

        double cash_pct = portfolio_cash / (portfolio_cash + std::abs(portfolio_pnl) + 0.01) * 100;
        ss << "**Current State:**\n";
        ss << "- Portfolio Cash: $" << std::setprecision(2) << portfolio_cash << "\n";
        ss << "- Cash % of portfolio: ~" << std::setprecision(0) << cash_pct << "%\n";
        ss << "- Session P&L: $" << std::setprecision(2) << portfolio_pnl << "\n\n";

        ss << "**When to INCREASE position sizing (base_position_pct, max_position_pct):**\n";
        ss << "- ✅ Win rate > 45% AND session is profitable → increase base_position_pct to 5-10%\n";
        ss << "- ✅ Win rate > 55% AND consistent profits → increase to 10-15%\n";
        ss << "- ✅ Cash sitting idle > 70% of portfolio → MUST deploy more capital\n";
        ss << "- ✅ Profit factor > 1.5 → strategy is working, scale up\n";
        ss << "- ✅ Low drawdown + positive P&L → safe to increase exposure\n\n";

        ss << "**When to DECREASE position sizing:**\n";
        ss << "- ❌ Win rate < 40% → reduce to 1-2% until strategy improves\n";
        ss << "- ❌ Consecutive losses > 3 → halve position size\n";
        ss << "- ❌ Session P&L deeply negative → reduce exposure\n\n";

        ss << "**Position Sizing Ranges:**\n";
        ss << "| Strategy Performance | base_position_pct | max_position_pct |\n";
        ss << "|---------------------|-------------------|------------------|\n";
        ss << "| Poor (WR < 40%)     | 1-2%              | 3-5%             |\n";
        ss << "| Average (WR 40-50%) | 3-5%              | 8-10%            |\n";
        ss << "| Good (WR 50-60%)    | 5-10%             | 15-20%           |\n";
        ss << "| Excellent (WR > 60%)| 10-15%            | 20-25%           |\n\n";

        ss << "**IMPORTANT:** Conservative 2% position sizing wastes capital!\n";
        ss << "If you see 90%+ cash sitting idle and strategy is profitable, INCREASE position sizing.\n\n";

        ss << "## Order Type Selection Guidelines\n";
        ss << "Choose order_type based on symbol characteristics and trading goals:\n";
        ss << "- MarketOnly: High volatility symbols, urgent entries/exits, wide spreads that change fast\n";
        ss << "- LimitOnly: Low volatility symbols, tight spreads, when you want maker rebates\n";
        ss << "- Adaptive: Best of both - try limit first, fall back to market if not filled\n";
        ss << "- Auto: When unsure, let ExecutionEngine decide based on real-time conditions\n";
        ss << "- If slippage costs are high → consider LimitOnly or Adaptive\n";
        ss << "- If missing trades due to unfilled limits → consider MarketOnly or Adaptive\n\n";

        ss << "## ⚠️ CRITICAL: OUTPUT FORMAT\n";
        ss << "**Output ONLY the JSON response. NO analysis, NO explanation, NO markdown text.**\n";
        ss << "Start your response DIRECTLY with the opening brace `{`.\n";
        ss << "Do NOT include any text before or after the JSON.\n";

        return ss.str();
    }

    std::string build_request_json(const std::string& prompt) {
        // Simple JSON escaping
        std::string escaped_prompt;
        for (char c : prompt) {
            switch (c) {
                case '"': escaped_prompt += "\\\""; break;
                case '\\': escaped_prompt += "\\\\"; break;
                case '\n': escaped_prompt += "\\n"; break;
                case '\r': escaped_prompt += "\\r"; break;
                case '\t': escaped_prompt += "\\t"; break;
                default: escaped_prompt += c;
            }
        }

        std::ostringstream ss;
        ss << "{"
           << "\"model\":\"" << model_ << "\","
           << "\"max_tokens\":2048,"
           << "\"messages\":[{\"role\":\"user\",\"content\":\"" << escaped_prompt << "\"}]"
           << "}";

        return ss.str();
    }

    bool parse_response(const std::string& body, ClaudeResponse& response) {
        // Simple JSON parsing (production would use a proper JSON library)
        // Extract content from Claude's response

        // Find "content" array
        size_t content_pos = body.find("\"content\"");
        if (content_pos == std::string::npos) return false;

        // Find "text": field (with colon to avoid matching "type":"text")
        size_t text_pos = body.find("\"text\":", content_pos);
        if (text_pos == std::string::npos) return false;

        // Extract text value (skip "text": which is 7 chars)
        size_t text_start = body.find("\"", text_pos + 7) + 1;
        size_t text_end = text_start;
        while (text_end < body.size()) {
            if (body[text_end] == '"' && body[text_end - 1] != '\\') break;
            text_end++;
        }

        std::string text = body.substr(text_start, text_end - text_start);

        // Unescape the text
        std::string unescaped;
        for (size_t i = 0; i < text.size(); i++) {
            if (text[i] == '\\' && i + 1 < text.size()) {
                switch (text[i + 1]) {
                    case 'n': unescaped += '\n'; i++; break;
                    case 'r': unescaped += '\r'; i++; break;
                    case 't': unescaped += '\t'; i++; break;
                    case '"': unescaped += '"'; i++; break;
                    case '\\': unescaped += '\\'; i++; break;
                    default: unescaped += text[i];
                }
            } else {
                unescaped += text[i];
            }
        }

        // Parse usage tokens
        size_t input_pos = body.find("\"input_tokens\"");
        if (input_pos != std::string::npos) {
            size_t num_start = body.find(":", input_pos) + 1;
            response.input_tokens = std::atoi(body.c_str() + num_start);
        }

        size_t output_pos = body.find("\"output_tokens\"");
        if (output_pos != std::string::npos) {
            size_t num_start = body.find(":", output_pos) + 1;
            response.output_tokens = std::atoi(body.c_str() + num_start);
        }

        // Parse the tuning command from the text
        return parse_tuner_command(unescaped, response.command);
    }

protected:
    // Protected for testing - parsing methods can be tested via inheritance
    bool parse_tuner_command(const std::string& text, ipc::TunerCommand& cmd) {
        std::memset(&cmd, 0, sizeof(cmd));
        cmd.magic = ipc::TunerCommand::MAGIC;
        cmd.version = ipc::TunerCommand::VERSION;
        cmd.action = ipc::TunerCommand::Action::NoChange;

        // Remove markdown code blocks if present
        std::string clean_text = text;
        size_t md_start = clean_text.find("```json");
        if (md_start != std::string::npos) {
            clean_text = clean_text.substr(md_start + 7);  // Skip "```json"
        } else {
            md_start = clean_text.find("```");
            if (md_start != std::string::npos) {
                clean_text = clean_text.substr(md_start + 3);  // Skip "```"
            }
        }
        size_t md_end = clean_text.find("```");
        if (md_end != std::string::npos) {
            clean_text = clean_text.substr(0, md_end);
        }

        // Find JSON block in response
        size_t json_start = clean_text.find("{");
        size_t json_end = clean_text.rfind("}");

        if (json_start == std::string::npos || json_end == std::string::npos) {
            // No JSON found, default to no change
            std::strncpy(cmd.reason, "No valid JSON in response", 63);
            cmd.finalize();
            return true;
        }

        std::string json = clean_text.substr(json_start, json_end - json_start + 1);

        // Parse action
        std::string action = extract_string(json, "action");
        if (action == "UPDATE_CONFIG") {
            cmd.action = ipc::TunerCommand::Action::UpdateSymbolConfig;
        } else if (action == "PAUSE") {
            cmd.action = ipc::TunerCommand::Action::PauseSymbol;
        } else if (action == "RESUME") {
            cmd.action = ipc::TunerCommand::Action::ResumeSymbol;
        } else if (action == "EMERGENCY_EXIT") {
            cmd.action = ipc::TunerCommand::Action::EmergencyExitSymbol;
        }
        // else NO_CHANGE (default)

        // Parse symbol
        std::string symbol = extract_string(json, "symbol");
        std::strncpy(cmd.symbol, symbol.c_str(), 15);

        // Parse confidence and urgency
        cmd.confidence = static_cast<uint8_t>(extract_number(json, "confidence"));
        cmd.urgency = static_cast<uint8_t>(extract_number(json, "urgency"));

        // Parse reason
        std::string reason = extract_string(json, "reason");
        std::strncpy(cmd.reason, reason.c_str(), 63);

        // Parse config if action is UPDATE_CONFIG
        if (cmd.action == ipc::TunerCommand::Action::UpdateSymbolConfig) {
            // Find config object
            size_t config_pos = json.find("\"config\"");
            if (config_pos != std::string::npos) {
                size_t config_start = json.find("{", config_pos);
                size_t config_end = json.find("}", config_start);
                if (config_start != std::string::npos && config_end != std::string::npos) {
                    std::string config_json = json.substr(config_start, config_end - config_start + 1);

                    cmd.config.init(cmd.symbol);

                    double val;
                    // EMA deviation thresholds
                    bool ema_changed = false;
                    if ((val = extract_number(config_json, "ema_dev_trending_pct")) > 0) {
                        cmd.config.ema_dev_trending_x100 = static_cast<int16_t>(val * 100);
                        ema_changed = true;
                    }
                    if ((val = extract_number(config_json, "ema_dev_ranging_pct")) > 0) {
                        cmd.config.ema_dev_ranging_x100 = static_cast<int16_t>(val * 100);
                        ema_changed = true;
                    }
                    if ((val = extract_number(config_json, "ema_dev_highvol_pct")) > 0) {
                        cmd.config.ema_dev_highvol_x100 = static_cast<int16_t>(val * 100);
                        ema_changed = true;
                    }
                    if (ema_changed) {
                        cmd.config.set_use_global_ema(false);
                    }

                    // Position sizing
                    bool position_changed = false;
                    if ((val = extract_number(config_json, "base_position_pct")) > 0) {
                        cmd.config.base_position_x100 = static_cast<int16_t>(val * 100);
                        position_changed = true;
                    }
                    if ((val = extract_number(config_json, "max_position_pct")) > 0) {
                        cmd.config.max_position_x100 = static_cast<int16_t>(val * 100);
                        position_changed = true;
                    }
                    if (position_changed) {
                        cmd.config.set_use_global_position(false);
                    }

                    // Trade filtering
                    bool filtering_changed = false;
                    if ((val = extract_number(config_json, "cooldown_ms")) > 0) {
                        cmd.config.set_cooldown_ms(val);  // Use setter with bounds checking
                        filtering_changed = true;
                    }
                    if ((val = extract_number(config_json, "signal_strength")) > 0) {
                        cmd.config.signal_strength = static_cast<int8_t>(val);
                        filtering_changed = true;
                    }
                    if (filtering_changed) {
                        cmd.config.set_use_global_filtering(false);
                    }

                    // Profit targets
                    bool target_changed = false;
                    if ((val = extract_number(config_json, "target_pct")) > 0) {
                        cmd.config.target_pct_x100 = static_cast<int16_t>(val * 100);
                        target_changed = true;
                    }
                    if ((val = extract_number(config_json, "stop_pct")) > 0) {
                        cmd.config.stop_pct_x100 = static_cast<int16_t>(val * 100);
                        target_changed = true;
                    }
                    if ((val = extract_number(config_json, "pullback_pct")) > 0) {
                        cmd.config.pullback_pct_x100 = static_cast<int16_t>(val * 100);
                        target_changed = true;
                    }
                    if (target_changed) {
                        cmd.config.set_use_global_target(false);
                    }

                    // Order execution preferences (per-symbol)
                    std::string order_type = extract_string(config_json, "order_type");
                    if (!order_type.empty()) {
                        if (order_type == "MarketOnly" || order_type == "Market") {
                            cmd.config.order_type_preference = 1;
                        } else if (order_type == "LimitOnly" || order_type == "Limit") {
                            cmd.config.order_type_preference = 2;
                        } else if (order_type == "Adaptive") {
                            cmd.config.order_type_preference = 3;
                        } else {
                            cmd.config.order_type_preference = 0;  // Auto
                        }
                    }
                    if ((val = extract_number(config_json, "limit_offset_bps")) > 0) {
                        cmd.config.limit_offset_bps_x100 = static_cast<int16_t>(val * 100);
                    }
                    if ((val = extract_number(config_json, "limit_timeout_ms")) > 0) {
                        cmd.config.limit_timeout_ms = static_cast<int16_t>(val);
                    }

                    // Minimum position
                    if ((val = extract_number(config_json, "min_position_pct")) > 0) {
                        cmd.config.min_position_x100 = static_cast<int16_t>(val * 100);
                    }

                    // Mode thresholds (streak-based)
                    if ((val = extract_number(config_json, "losses_to_cautious")) > 0) {
                        cmd.config.losses_to_cautious = static_cast<int8_t>(val);
                    }
                    if ((val = extract_number(config_json, "losses_to_defensive")) > 0) {
                        cmd.config.losses_to_defensive = static_cast<int8_t>(val);
                    }
                    if ((val = extract_number(config_json, "losses_to_exit_only")) > 0) {
                        cmd.config.losses_to_exit_only = static_cast<int8_t>(val);
                    }
                    if ((val = extract_number(config_json, "wins_to_aggressive")) > 0) {
                        cmd.config.wins_to_aggressive = static_cast<int8_t>(val);
                    }

                    // Signal thresholds by mode
                    if ((val = extract_number(config_json, "signal_aggressive")) > 0) {
                        cmd.config.signal_aggressive_x100 = static_cast<int8_t>(val * 100);
                    }
                    if ((val = extract_number(config_json, "signal_normal")) > 0) {
                        cmd.config.signal_normal_x100 = static_cast<int8_t>(val * 100);
                    }
                    if ((val = extract_number(config_json, "signal_cautious")) > 0) {
                        cmd.config.signal_cautious_x100 = static_cast<int8_t>(val * 100);
                    }
                    if ((val = extract_number(config_json, "min_confidence")) > 0) {
                        cmd.config.min_confidence_x100 = static_cast<int8_t>(val * 100);
                    }

                    // Accumulation control
                    if ((val = extract_number(config_json, "accum_floor_trending")) > 0) {
                        cmd.config.accum_floor_trending_x100 = static_cast<int8_t>(val * 100);
                    }
                    if ((val = extract_number(config_json, "accum_floor_ranging")) > 0) {
                        cmd.config.accum_floor_ranging_x100 = static_cast<int8_t>(val * 100);
                    }
                    if ((val = extract_number(config_json, "accum_floor_highvol")) > 0) {
                        cmd.config.accum_floor_highvol_x100 = static_cast<int8_t>(val * 100);
                    }
                    if ((val = extract_number(config_json, "accum_boost_win")) > 0) {
                        cmd.config.accum_boost_per_win_x100 = static_cast<int8_t>(val * 100);
                    }
                    if ((val = extract_number(config_json, "accum_penalty_loss")) > 0) {
                        cmd.config.accum_penalty_per_loss_x100 = static_cast<int8_t>(val * 100);
                    }
                    if ((val = extract_number(config_json, "accum_max")) > 0) {
                        cmd.config.accum_max_x100 = static_cast<int8_t>(val * 100);
                    }
                }
            }
        }

        cmd.finalize();
        return true;
    }

    std::string extract_string(const std::string& json, const char* key) {
        std::string search = "\"" + std::string(key) + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return "";

        size_t colon = json.find(":", pos);
        if (colon == std::string::npos) return "";

        size_t quote_start = json.find("\"", colon);
        if (quote_start == std::string::npos) return "";

        size_t quote_end = quote_start + 1;
        while (quote_end < json.size()) {
            if (json[quote_end] == '"' && json[quote_end - 1] != '\\') break;
            quote_end++;
        }

        return json.substr(quote_start + 1, quote_end - quote_start - 1);
    }

    double extract_number(const std::string& json, const char* key) {
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

private:
    // Internal helper methods
    const char* trigger_name(ipc::TriggerReason trigger) {
        switch (trigger) {
            case ipc::TriggerReason::Scheduled: return "Scheduled (periodic)";
            case ipc::TriggerReason::LossThreshold: return "Loss threshold exceeded";
            case ipc::TriggerReason::ConsecutiveLosses: return "Consecutive losses";
            case ipc::TriggerReason::WinStreak: return "Win streak";
            case ipc::TriggerReason::VolatilitySpike: return "Volatility spike";
            case ipc::TriggerReason::NewsTriggered: return "News event";
            case ipc::TriggerReason::ManualRequest: return "Manual request";
            case ipc::TriggerReason::StartupInit: return "Startup initialization";
            case ipc::TriggerReason::RegimeChange: return "Regime change";
            case ipc::TriggerReason::DrawdownAlert: return "Drawdown alert";
            default: return "Unknown";
        }
    }

    const char* regime_name(uint8_t regime) {
        switch (regime) {
            case 0: return "Unknown";
            case 1: return "TrendingUp";
            case 2: return "TrendingDown";
            case 3: return "Ranging";
            case 4: return "HighVolatility";
            case 5: return "LowVolatility";
            default: return "Unknown";
        }
    }
};

}  // namespace tuner
}  // namespace hft
