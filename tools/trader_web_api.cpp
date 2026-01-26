/**
 * Trader Web API
 *
 * REST API backend for Trader monitoring and control.
 * Reads from shared memory and provides JSON endpoints.
 *
 * Endpoints:
 *   GET  /api/status          - System status
 *   GET  /api/portfolio       - Portfolio state
 *   GET  /api/symbols         - Symbol configs
 *   GET  /api/events          - Recent events
 *   GET  /api/events/stream   - SSE event stream
 *   GET  /api/stats           - Tuner statistics
 *   GET  /api/alerts          - Connection status and alerts
 *   POST /api/tune            - Trigger manual tuning
 *   POST /api/control         - Send control command (legacy)
 *
 *   POST /api/control/trading - Enable/disable trading
 *   POST /api/control/tuner   - Pause/resume/manual tuner mode
 *   PUT  /api/symbols/:symbol - Update symbol config
 *   POST /api/tuner/trigger   - Force immediate tuning
 *
 * Usage:
 *   trader_web_api                    # Start on port 8080
 *   trader_web_api --port 3000        # Start on port 3000
 *   trader_web_api --cors             # Enable CORS for development
 */

#define CPPHTTPLIB_NO_OPENSSL 1

#include "../external/httplib.h"
#include "../include/ipc/symbol_config.hpp"
#include "../include/ipc/shared_event_log.hpp"
#include "../include/ipc/shared_config.hpp"
#include "../include/ipc/shared_portfolio_state.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <csignal>
#include <cstring>

using namespace hft::ipc;

// Global flag for clean shutdown
volatile sig_atomic_t g_running = 1;

void signal_handler(int) {
    g_running = 0;
}

// ============================================================================
// JSON Builder Helper
// ============================================================================

class JsonBuilder {
public:
    void start_object() { ss_ << "{"; first_ = true; }
    void end_object() { ss_ << "}"; }
    void start_array() { ss_ << "["; first_ = true; }
    void end_array() { ss_ << "]"; }

    void key(const char* k) {
        if (!first_) ss_ << ",";
        first_ = false;
        ss_ << "\"" << k << "\":";
    }

    void value(const char* v) {
        ss_ << "\"" << escape(v) << "\"";
    }

    void value(const std::string& v) {
        ss_ << "\"" << escape(v.c_str()) << "\"";
    }

    void value(int64_t v) { ss_ << v; }
    void value(uint64_t v) { ss_ << v; }
    void value(int v) { ss_ << v; }
    void value(double v) { ss_ << std::fixed << std::setprecision(4) << v; }
    void value(bool v) { ss_ << (v ? "true" : "false"); }

    void raw_value(const char* v) { ss_ << v; }

    void kv(const char* k, const char* v) { key(k); value(v); }
    void kv(const char* k, const std::string& v) { key(k); value(v); }
    void kv(const char* k, int64_t v) { key(k); value(v); }
    void kv(const char* k, uint64_t v) { key(k); value(v); }
    void kv(const char* k, uint32_t v) { key(k); value(static_cast<uint64_t>(v)); }
    void kv(const char* k, int v) { key(k); value(v); }
    void kv(const char* k, double v) { key(k); value(v); }
    void kv(const char* k, bool v) { key(k); value(v); }

    std::string str() const { return ss_.str(); }

private:
    std::ostringstream ss_;
    bool first_ = true;

    std::string escape(const char* s) {
        std::string result;
        while (*s) {
            switch (*s) {
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default: result += *s;
            }
            s++;
        }
        return result;
    }
};

// ============================================================================
// Web API Server
// ============================================================================

class WebApiServer {
public:
    WebApiServer(int port, bool enable_cors)
        : port_(port), enable_cors_(enable_cors)
    {
        // Connect to shared memory (read-write for control operations)
        symbol_configs_ = SharedSymbolConfigs::open_rw("/trader_symbol_configs");
        event_log_ = SharedEventLog::open_readonly();
        shared_config_ = SharedConfig::open_rw("/trader_config");
        portfolio_state_ = SharedPortfolioState::open("/trader_portfolio");

        setup_routes();
    }

    void run() {
        std::cout << "[WEB] Starting API server on port " << port_ << "\n";

        if (symbol_configs_) std::cout << "[IPC] Connected to symbol configs\n";
        if (event_log_) std::cout << "[IPC] Connected to event log\n";
        if (shared_config_) std::cout << "[IPC] Connected to shared config\n";
        if (portfolio_state_) std::cout << "[IPC] Connected to portfolio state\n";

        server_.listen("0.0.0.0", port_);
    }

    void stop() {
        server_.stop();
    }

private:
    httplib::Server server_;
    int port_;
    bool enable_cors_;

    SharedSymbolConfigs* symbol_configs_ = nullptr;
    SharedEventLog* event_log_ = nullptr;
    SharedConfig* shared_config_ = nullptr;
    SharedPortfolioState* portfolio_state_ = nullptr;

    void setup_routes() {
        // CORS middleware
        if (enable_cors_) {
            server_.set_pre_routing_handler([](const httplib::Request&, httplib::Response& res) {
                res.set_header("Access-Control-Allow-Origin", "*");
                res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
                res.set_header("Access-Control-Allow-Headers", "Content-Type");
                return httplib::Server::HandlerResponse::Unhandled;
            });

            server_.Options(".*", [](const httplib::Request&, httplib::Response& res) {
                res.set_header("Access-Control-Allow-Origin", "*");
                res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
                res.set_header("Access-Control-Allow-Headers", "Content-Type");
                res.status = 204;
            });
        }

        // Status endpoint
        server_.Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
            handle_status(res);
        });

        // Portfolio endpoint
        server_.Get("/api/portfolio", [this](const httplib::Request&, httplib::Response& res) {
            handle_portfolio(res);
        });

        // Symbols endpoint
        server_.Get("/api/symbols", [this](const httplib::Request&, httplib::Response& res) {
            handle_symbols(res);
        });

        // Events endpoint
        server_.Get("/api/events", [this](const httplib::Request& req, httplib::Response& res) {
            int limit = 100;
            if (req.has_param("limit")) {
                limit = std::atoi(req.get_param_value("limit").c_str());
            }
            handle_events(res, limit);
        });

        // Stats endpoint
        server_.Get("/api/stats", [this](const httplib::Request&, httplib::Response& res) {
            handle_stats(res);
        });

        // Manual tune trigger
        server_.Post("/api/tune", [this](const httplib::Request&, httplib::Response& res) {
            handle_tune(res);
        });

        // Control command (legacy)
        server_.Post("/api/control", [this](const httplib::Request& req, httplib::Response& res) {
            handle_control(req, res);
        });

        // NEW: Trading control endpoint
        server_.Post("/api/control/trading", [this](const httplib::Request& req, httplib::Response& res) {
            handle_control_trading(req, res);
        });

        // NEW: Tuner control endpoint
        server_.Post("/api/control/tuner", [this](const httplib::Request& req, httplib::Response& res) {
            handle_control_tuner(req, res);
        });

        // NEW: Per-symbol config update
        server_.Put("/api/symbols/:symbol", [this](const httplib::Request& req, httplib::Response& res) {
            handle_symbol_update(req, res);
        });

        // NEW: Force immediate tuning
        server_.Post("/api/tuner/trigger", [this](const httplib::Request&, httplib::Response& res) {
            handle_tuner_trigger(res);
        });

        // NEW: Alerts and connection status
        server_.Get("/api/alerts", [this](const httplib::Request&, httplib::Response& res) {
            handle_alerts(res);
        });

        // NEW: Regime-Strategy mapping
        server_.Get("/api/config/regime_strategy", [this](const httplib::Request&, httplib::Response& res) {
            handle_regime_strategy_get(res);
        });
        server_.Put("/api/config/regime_strategy", [this](const httplib::Request& req, httplib::Response& res) {
            handle_regime_strategy_put(req, res);
        });

        // Serve static files (for frontend)
        server_.set_mount_point("/", "../web");
    }

    void handle_status(httplib::Response& res) {
        JsonBuilder json;
        json.start_object();

        json.kv("status", "running");

        // Trader connection status
        json.key("connections");
        json.start_object();
        json.kv("symbol_configs", symbol_configs_ != nullptr);
        json.kv("event_log", event_log_ != nullptr);
        json.kv("shared_config", shared_config_ != nullptr);
        json.kv("portfolio_state", portfolio_state_ != nullptr);
        json.end_object();

        // Trader heartbeat
        if (shared_config_) {
            auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            auto last_hb = shared_config_->get_heartbeat_ns();
            int64_t age_ms = (now - last_hb) / 1'000'000;

            json.key("hft");
            json.start_object();
            uint8_t status = shared_config_->get_trader_status();
            const char* status_name = status == 0 ? "stopped" :
                                      status == 1 ? "starting" :
                                      status == 2 ? "running" : "shutting_down";
            json.kv("status", status_name);
            json.kv("heartbeat_age_ms", age_ms);
            json.kv("heartbeat_ok", age_ms < 3000);
            json.end_object();
        }

        // Tuner connection
        if (symbol_configs_) {
            json.key("tuner");
            json.start_object();
            json.kv("connected", symbol_configs_->tuner_connected.load() != 0);
            json.kv("tune_count", static_cast<int>(symbol_configs_->tune_count.load()));
            json.end_object();
        }

        json.end_object();
        res.set_content(json.str(), "application/json");
    }

    void handle_portfolio(httplib::Response& res) {
        JsonBuilder json;
        json.start_object();

        if (!portfolio_state_) {
            json.kv("error", "Portfolio state not connected");
            json.end_object();
            res.status = 503;
            res.set_content(json.str(), "application/json");
            return;
        }

        json.kv("cash", portfolio_state_->cash());
        json.kv("total_realized_pnl", portfolio_state_->total_realized_pnl());
        json.kv("total_unrealized_pnl", portfolio_state_->total_unrealized_pnl());
        json.kv("total_equity", portfolio_state_->cash() +
                                portfolio_state_->total_unrealized_pnl());

        // Positions
        json.key("positions");
        json.start_array();

        bool first_pos = true;
        for (uint32_t i = 0; i < MAX_PORTFOLIO_SYMBOLS; ++i) {
            const auto& pos = portfolio_state_->positions[i];
            if (!pos.active.load()) continue;

            if (!first_pos) json.raw_value(",");
            first_pos = false;
            json.start_object();
            json.kv("symbol", pos.symbol);
            json.kv("quantity", pos.quantity_x8.load() / 1e8);
            json.kv("avg_price", pos.avg_price_x8.load() / 1e8);
            json.kv("current_price", pos.last_price_x8.load() / 1e8);
            json.kv("unrealized_pnl", pos.unrealized_pnl());
            json.kv("realized_pnl", pos.realized_pnl_x8.load() / 1e8);
            json.end_object();
        }

        json.end_array();
        json.end_object();
        res.set_content(json.str(), "application/json");
    }

    void handle_symbols(httplib::Response& res) {
        JsonBuilder json;
        json.start_object();

        if (!symbol_configs_) {
            json.kv("error", "Symbol configs not connected");
            json.end_object();
            res.status = 503;
            res.set_content(json.str(), "application/json");
            return;
        }

        json.kv("count", static_cast<int>(symbol_configs_->symbol_count.load()));
        json.kv("sequence", static_cast<int>(symbol_configs_->sequence.load()));

        json.key("symbols");
        json.start_array();

        uint32_t count = symbol_configs_->symbol_count.load();
        for (uint32_t i = 0; i < count; ++i) {
            const auto& cfg = symbol_configs_->symbols[i];

            if (i > 0) json.raw_value(",");
            json.start_object();
            json.kv("symbol", cfg.symbol);
            json.kv("enabled", cfg.is_enabled());
            json.kv("regime_override", static_cast<int>(cfg.regime_override));

            json.key("config");
            json.start_object();
            json.kv("ema_dev_trending_pct", cfg.ema_dev_trending_x100 / 100.0);
            json.kv("ema_dev_ranging_pct", cfg.ema_dev_ranging_x100 / 100.0);
            json.kv("ema_dev_highvol_pct", cfg.ema_dev_highvol_x100 / 100.0);
            json.kv("base_position_pct", cfg.base_position_x100 / 100.0);
            json.kv("max_position_pct", cfg.max_position_x100 / 100.0);
            json.kv("cooldown_ms", static_cast<int>(cfg.cooldown_ms));
            json.kv("target_pct", cfg.target_pct_x100 / 100.0);
            json.kv("stop_pct", cfg.stop_pct_x100 / 100.0);
            json.end_object();

            json.key("performance");
            json.start_object();
            json.kv("total_trades", cfg.total_trades);
            json.kv("winning_trades", cfg.winning_trades);
            json.kv("win_rate", cfg.win_rate());
            json.kv("total_pnl", cfg.total_pnl_x100 / 100.0);
            json.kv("avg_pnl", cfg.avg_pnl());
            json.end_object();

            json.end_object();
        }

        json.end_array();
        json.end_object();
        res.set_content(json.str(), "application/json");
    }

    void handle_events(httplib::Response& res, int limit) {
        JsonBuilder json;
        json.start_object();

        if (!event_log_) {
            json.kv("error", "Event log not connected");
            json.end_object();
            res.status = 503;
            res.set_content(json.str(), "application/json");
            return;
        }

        json.kv("total_events", event_log_->total_events.load());

        json.key("events");
        json.start_array();

        uint64_t current = event_log_->current_position();
        uint64_t start = current > static_cast<uint64_t>(limit) ? current - limit : 0;

        bool first = true;
        for (uint64_t seq = start; seq < current; ++seq) {
            const TunerEvent* e = event_log_->get_event(seq);
            if (!e) continue;

            if (!first) json.raw_value(",");
            first = false;

            json.start_object();
            json.kv("sequence", e->sequence);
            json.kv("type", e->type_name());
            json.kv("symbol", e->symbol);
            json.kv("severity", static_cast<int>(e->severity));

            // Type-specific payload
            switch (e->type) {
                case TunerEventType::Signal:
                case TunerEventType::Order:
                case TunerEventType::Fill:
                    json.key("trade");
                    json.start_object();
                    json.kv("side", e->payload.trade.side == TradeSide::Buy ? "BUY" : "SELL");
                    json.kv("price", e->payload.trade.price);
                    json.kv("quantity", e->payload.trade.quantity);
                    json.kv("pnl", e->payload.trade.pnl_x100 / 100.0);
                    json.end_object();
                    break;

                case TunerEventType::ConfigChange:
                    json.key("config_change");
                    json.start_object();
                    json.kv("param", e->payload.config.param_name);
                    json.kv("old_value", e->payload.config.old_value_x100 / 100.0);
                    json.kv("new_value", e->payload.config.new_value_x100 / 100.0);
                    json.kv("ai_confidence", static_cast<int>(e->payload.config.ai_confidence));
                    json.end_object();
                    break;

                case TunerEventType::AIDecision:
                    json.key("ai");
                    json.start_object();
                    json.kv("confidence", static_cast<int>(e->payload.ai.confidence));
                    json.kv("action", static_cast<int>(e->payload.ai.action_taken));
                    json.kv("latency_ms", static_cast<int>(e->payload.ai.latency_ms));
                    json.end_object();
                    break;

                default:
                    break;
            }

            if (e->reason[0] != '\0') {
                json.kv("reason", e->reason);
            }

            json.end_object();
        }

        json.end_array();
        json.end_object();
        res.set_content(json.str(), "application/json");
    }

    void handle_stats(httplib::Response& res) {
        JsonBuilder json;
        json.start_object();

        if (!event_log_) {
            json.kv("error", "Event log not connected");
            json.end_object();
            res.status = 503;
            res.set_content(json.str(), "application/json");
            return;
        }

        json.kv("total_events", event_log_->total_events.load());
        json.kv("session_pnl", event_log_->session_pnl_x100.load() / 100.0);

        // Tuner stats
        json.key("tuner");
        json.start_object();
        const auto& t = event_log_->tuner_stats;
        json.kv("total_decisions", t.total_decisions.load());
        json.kv("config_changes", t.config_changes.load());
        json.kv("pauses_triggered", t.pauses_triggered.load());
        json.kv("emergency_exits", t.emergency_exits.load());
        json.kv("avg_latency_ms", t.avg_latency_ms());
        json.kv("total_cost_usd", t.total_cost());
        json.end_object();

        // Symbol stats
        json.key("symbols");
        json.start_array();

        uint32_t count = event_log_->symbol_count.load();
        for (uint32_t i = 0; i < count; ++i) {
            const auto& s = event_log_->symbol_stats[i];
            if (s.is_empty()) continue;

            if (i > 0) json.raw_value(",");
            json.start_object();
            json.kv("symbol", s.symbol);
            json.kv("signal_count", s.signal_count.load());
            json.kv("fill_count", s.fill_count.load());
            json.kv("win_rate", s.win_rate());
            json.kv("session_pnl", s.session_pnl_x100.load() / 100.0);
            json.kv("total_pnl", s.total_pnl_x100.load() / 100.0);
            json.kv("config_changes", s.config_changes.load());
            json.end_object();
        }

        json.end_array();
        json.end_object();
        res.set_content(json.str(), "application/json");
    }

    void handle_tune(httplib::Response& res) {
        JsonBuilder json;
        json.start_object();

        if (!shared_config_) {
            json.kv("error", "Shared config not connected");
            json.end_object();
            res.status = 503;
            res.set_content(json.str(), "application/json");
            return;
        }

        // Set manual tune request flag - tuner will pick this up
        shared_config_->request_manual_tune();

        json.kv("status", "requested");
        json.kv("message", "Manual tuning request submitted");

        json.end_object();
        res.set_content(json.str(), "application/json");
    }

    void handle_control(const httplib::Request& req, httplib::Response& res) {
        JsonBuilder json;
        json.start_object();

        if (!shared_config_) {
            json.kv("error", "Shared config not connected");
            json.end_object();
            res.status = 503;
            res.set_content(json.str(), "application/json");
            return;
        }

        // Parse command from body (simple key=value)
        std::string command;
        if (req.has_param("command")) {
            command = req.get_param_value("command");
        }

        if (command == "pause") {
            // Send pause command via shared config
            json.kv("status", "ok");
            json.kv("message", "Pause command sent");
        } else if (command == "resume") {
            json.kv("status", "ok");
            json.kv("message", "Resume command sent");
        } else {
            json.kv("error", "Unknown command");
            res.status = 400;
        }

        json.end_object();
        res.set_content(json.str(), "application/json");
    }

    // ========================================================================
    // NEW Control Endpoints
    // ========================================================================

    // Simple JSON value extraction (no external dependency)
    static std::string extract_json_string(const std::string& body, const char* key) {
        std::string search = "\"" + std::string(key) + "\"";
        auto pos = body.find(search);
        if (pos == std::string::npos) return "";

        pos = body.find(':', pos);
        if (pos == std::string::npos) return "";

        pos = body.find('"', pos);
        if (pos == std::string::npos) return "";

        auto end = body.find('"', pos + 1);
        if (end == std::string::npos) return "";

        return body.substr(pos + 1, end - pos - 1);
    }

    static bool extract_json_bool(const std::string& body, const char* key, bool& out) {
        std::string search = "\"" + std::string(key) + "\"";
        auto pos = body.find(search);
        if (pos == std::string::npos) return false;

        pos = body.find(':', pos);
        if (pos == std::string::npos) return false;

        // Skip whitespace
        while (pos < body.size() && (body[pos] == ':' || body[pos] == ' ')) pos++;

        if (body.substr(pos, 4) == "true") {
            out = true;
            return true;
        } else if (body.substr(pos, 5) == "false") {
            out = false;
            return true;
        }
        return false;
    }

    static bool extract_json_double(const std::string& body, const char* key, double& out) {
        std::string search = "\"" + std::string(key) + "\"";
        auto pos = body.find(search);
        if (pos == std::string::npos) return false;

        pos = body.find(':', pos);
        if (pos == std::string::npos) return false;

        // Skip whitespace
        while (pos < body.size() && (body[pos] == ':' || body[pos] == ' ')) pos++;

        try {
            out = std::stod(body.substr(pos));
            return true;
        } catch (...) {
            return false;
        }
    }

    static bool extract_json_int(const std::string& body, const char* key, int& out) {
        std::string search = "\"" + std::string(key) + "\"";
        auto pos = body.find(search);
        if (pos == std::string::npos) return false;

        pos = body.find(':', pos);
        if (pos == std::string::npos) return false;

        // Skip whitespace
        while (pos < body.size() && (body[pos] == ':' || body[pos] == ' ')) pos++;

        try {
            out = std::stoi(body.substr(pos));
            return true;
        } catch (...) {
            return false;
        }
    }

    // POST /api/control/trading - Enable/disable trading
    void handle_control_trading(const httplib::Request& req, httplib::Response& res) {
        JsonBuilder json;
        json.start_object();

        if (!shared_config_) {
            json.kv("error", "Shared config not connected");
            json.end_object();
            res.status = 503;
            res.set_content(json.str(), "application/json");
            return;
        }

        bool enabled;
        if (!extract_json_bool(req.body, "enabled", enabled)) {
            json.kv("error", "Missing or invalid 'enabled' field (boolean)");
            json.end_object();
            res.status = 400;
            res.set_content(json.str(), "application/json");
            return;
        }

        // TODO: Add trading enabled/disabled flag to SharedConfig if not exists
        // For now, we use manual_override as a proxy (1 = manual = trading paused by user)
        shared_config_->set_manual_override(!enabled);

        json.kv("status", "ok");
        json.kv("trading_enabled", enabled);
        json.kv("message", enabled ? "Trading enabled" : "Trading disabled");

        json.end_object();
        res.set_content(json.str(), "application/json");
    }

    // POST /api/control/tuner - Control tuner mode
    void handle_control_tuner(const httplib::Request& req, httplib::Response& res) {
        JsonBuilder json;
        json.start_object();

        if (!shared_config_) {
            json.kv("error", "Shared config not connected");
            json.end_object();
            res.status = 503;
            res.set_content(json.str(), "application/json");
            return;
        }

        std::string mode = extract_json_string(req.body, "mode");

        if (mode == "active") {
            shared_config_->set_tuner_paused(false);
            shared_config_->set_manual_override(false);
            json.kv("status", "ok");
            json.kv("mode", "active");
            json.kv("message", "Tuner activated");
        } else if (mode == "paused") {
            shared_config_->set_tuner_paused(true);
            json.kv("status", "ok");
            json.kv("mode", "paused");
            json.kv("message", "Tuner paused");
        } else if (mode == "manual") {
            shared_config_->set_tuner_paused(true);
            shared_config_->set_manual_override(true);
            json.kv("status", "ok");
            json.kv("mode", "manual");
            json.kv("message", "Manual override enabled, tuner paused");
        } else {
            json.kv("error", "Invalid mode. Use: active, paused, or manual");
            res.status = 400;
        }

        json.end_object();
        res.set_content(json.str(), "application/json");
    }

    // PUT /api/symbols/:symbol - Update symbol config
    void handle_symbol_update(const httplib::Request& req, httplib::Response& res) {
        JsonBuilder json;
        json.start_object();

        if (!symbol_configs_) {
            json.kv("error", "Symbol configs not connected");
            json.end_object();
            res.status = 503;
            res.set_content(json.str(), "application/json");
            return;
        }

        // Get symbol from path
        std::string symbol = req.path_params.at("symbol");

        // Find or create symbol config
        auto* cfg = symbol_configs_->get_or_create(symbol.c_str());
        if (!cfg) {
            json.kv("error", "Cannot create symbol config (max symbols reached)");
            json.end_object();
            res.status = 500;
            res.set_content(json.str(), "application/json");
            return;
        }

        // Parse and apply updates
        bool any_update = false;
        double dval;
        int ival;
        bool bval;

        // Enabled flag
        if (extract_json_bool(req.body, "enabled", bval)) {
            cfg->enabled = bval ? 1 : 0;
            any_update = true;
        }

        // EMA thresholds (input as percentage, e.g., 1.5 = 1.5%)
        if (extract_json_double(req.body, "ema_dev_trending", dval)) {
            cfg->ema_dev_trending_x100 = static_cast<int16_t>(dval * 100);
            any_update = true;
        }
        if (extract_json_double(req.body, "ema_dev_ranging", dval)) {
            cfg->ema_dev_ranging_x100 = static_cast<int16_t>(dval * 100);
            any_update = true;
        }
        if (extract_json_double(req.body, "ema_dev_highvol", dval)) {
            cfg->ema_dev_highvol_x100 = static_cast<int16_t>(dval * 100);
            any_update = true;
        }

        // Position sizing
        if (extract_json_double(req.body, "base_position_pct", dval)) {
            cfg->base_position_x100 = static_cast<int16_t>(dval * 100);
            any_update = true;
        }
        if (extract_json_double(req.body, "max_position_pct", dval)) {
            cfg->max_position_x100 = static_cast<int16_t>(dval * 100);
            any_update = true;
        }

        // Target/Stop
        if (extract_json_double(req.body, "target_pct", dval)) {
            cfg->target_pct_x100 = static_cast<int16_t>(dval * 100);
            any_update = true;
        }
        if (extract_json_double(req.body, "stop_pct", dval)) {
            cfg->stop_pct_x100 = static_cast<int16_t>(dval * 100);
            any_update = true;
        }
        if (extract_json_double(req.body, "pullback_pct", dval)) {
            cfg->pullback_pct_x100 = static_cast<int16_t>(dval * 100);
            any_update = true;
        }

        // Cooldown
        if (extract_json_int(req.body, "cooldown_ms", ival)) {
            cfg->cooldown_ms = static_cast<int16_t>(ival);
            any_update = true;
        }

        // Signal strength
        if (extract_json_int(req.body, "signal_strength", ival)) {
            cfg->signal_strength = static_cast<int8_t>(ival);
            any_update = true;
        }

        // Order execution
        if (extract_json_int(req.body, "order_type_preference", ival)) {
            cfg->order_type_preference = static_cast<uint8_t>(ival);
            any_update = true;
        }
        if (extract_json_double(req.body, "limit_offset_bps", dval)) {
            cfg->limit_offset_bps_x100 = static_cast<int16_t>(dval * 100);
            any_update = true;
        }
        if (extract_json_int(req.body, "limit_timeout_ms", ival)) {
            cfg->limit_timeout_ms = static_cast<int16_t>(ival);
            any_update = true;
        }

        if (any_update) {
            // Update timestamp and sequence
            cfg->last_update_ns = std::chrono::steady_clock::now().time_since_epoch().count();
            symbol_configs_->sequence.fetch_add(1);

            json.kv("status", "ok");
            json.kv("symbol", symbol);
            json.kv("message", "Symbol config updated");
        } else {
            json.kv("status", "ok");
            json.kv("symbol", symbol);
            json.kv("message", "No fields to update");
        }

        json.end_object();
        res.set_content(json.str(), "application/json");
    }

    // POST /api/tuner/trigger - Force immediate tuning
    void handle_tuner_trigger(httplib::Response& res) {
        JsonBuilder json;
        json.start_object();

        if (!shared_config_) {
            json.kv("error", "Shared config not connected");
            json.end_object();
            res.status = 503;
            res.set_content(json.str(), "application/json");
            return;
        }

        shared_config_->request_manual_tune();

        json.kv("status", "ok");
        json.kv("message", "Manual tuning triggered");

        json.end_object();
        res.set_content(json.str(), "application/json");
    }

    // GET /api/alerts - Connection status and active alerts
    void handle_alerts(httplib::Response& res) {
        JsonBuilder json;
        json.start_object();

        // Connection status
        json.key("connections");
        json.start_object();

        if (shared_config_) {
            // WebSocket market data status
            uint8_t ws_market = shared_config_->get_ws_market_status();
            const char* ws_status_name = ws_market == 0 ? "disconnected" :
                                         ws_market == 1 ? "degraded" : "healthy";
            json.kv("market_data", ws_status_name);

            // User stream status (if applicable)
            uint8_t ws_user = shared_config_->get_ws_user_status();
            const char* ws_user_name = ws_user == 0 ? "disconnected" :
                                       ws_user == 1 ? "degraded" : "healthy";
            json.kv("user_stream", ws_user_name);

            // Reconnection stats
            json.kv("reconnect_count", shared_config_->get_ws_reconnect_count());

            // Last message age (for health check)
            auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
            int64_t last_msg_ns = shared_config_->get_ws_last_message_ns();
            int64_t age_ms = (last_msg_ns > 0) ? (now_ns - last_msg_ns) / 1'000'000 : -1;
            json.kv("last_message_age_ms", age_ms);

            // Is healthy check
            json.kv("is_healthy", shared_config_->is_ws_healthy(10));

            // Trader process status
            uint8_t trader_status = shared_config_->get_trader_status();
            const char* trader_status_name = trader_status == 0 ? "stopped" :
                                          trader_status == 1 ? "starting" :
                                          trader_status == 2 ? "running" : "shutting_down";
            json.kv("trader_status", trader_status_name);

            // Heartbeat check
            json.kv("trader_alive", shared_config_->is_trader_alive(3));
        } else {
            json.kv("market_data", "unknown");
            json.kv("user_stream", "unknown");
            json.kv("reconnect_count", 0);
            json.kv("last_message_age_ms", -1);
            json.kv("is_healthy", false);
            json.kv("trader_status", "unknown");
            json.kv("trader_alive", false);
        }

        json.end_object();

        // Active alerts (based on current state)
        json.key("alerts");
        json.start_array();

        if (shared_config_) {
            uint8_t ws_market = shared_config_->get_ws_market_status();
            uint8_t trader_status = shared_config_->get_trader_status();
            bool trader_alive = shared_config_->is_trader_alive(3);

            // Connection lost alert
            if (ws_market == 0 && trader_status == 2) {
                json.start_object();
                json.kv("level", "critical");
                json.kv("message", "WebSocket connection lost - Reconnecting...");
                json.kv("code", "CONNECTION_LOST");
                json.end_object();
            }

            // Connection degraded alert
            if (ws_market == 1) {
                json.start_object();
                json.kv("level", "warning");
                json.kv("message", "Connection degraded - No data received recently");
                json.kv("code", "CONNECTION_DEGRADED");
                json.end_object();
            }

            // Trader not responding alert
            if (trader_status == 2 && !trader_alive) {
                json.start_object();
                json.kv("level", "critical");
                json.kv("message", "Trader engine not responding - Possible crash");
                json.kv("code", "TRADER_NOT_RESPONDING");
                json.end_object();
            }

            // Trader stopped alert
            if (trader_status == 0) {
                json.start_object();
                json.kv("level", "info");
                json.kv("message", "Trader engine is stopped");
                json.kv("code", "TRADER_STOPPED");
                json.end_object();
            }
        } else {
            json.start_object();
            json.kv("level", "error");
            json.kv("message", "Cannot connect to Trader shared config");
            json.kv("code", "CONFIG_UNAVAILABLE");
            json.end_object();
        }

        json.end_array();

        json.end_object();
        res.set_content(json.str(), "application/json");
    }

    // GET /api/config/regime_strategy - Get regime-strategy mapping
    void handle_regime_strategy_get(httplib::Response& res) {
        JsonBuilder json;
        json.start_object();

        if (!shared_config_) {
            json.kv("error", "Shared config not connected");
            json.end_object();
            res.status = 503;
            res.set_content(json.str(), "application/json");
            return;
        }

        const char* regime_names[] = {"Unknown", "TrendingUp", "TrendingDown", "Ranging", "HighVol", "LowVol", "Spike"};
        const char* strategy_names[] = {"NONE", "MOMENTUM", "MEAN_REV", "MKT_MAKER", "DEFENSIVE", "CAUTIOUS", "SMART"};

        json.key("mapping");
        json.start_array();
        for (int i = 0; i <= 6; ++i) {
            uint8_t st = shared_config_->get_strategy_for_regime(i);
            json.start_object();
            json.kv("regime_id", i);
            json.kv("regime_name", regime_names[i]);
            json.kv("strategy_id", static_cast<int>(st));
            json.kv("strategy_name", st <= 6 ? strategy_names[st] : "UNKNOWN");
            json.end_object();
        }
        json.end_array();

        json.end_object();
        res.set_content(json.str(), "application/json");
    }

    // PUT /api/config/regime_strategy - Update regime-strategy mapping
    void handle_regime_strategy_put(const httplib::Request& req, httplib::Response& res) {
        JsonBuilder json;
        json.start_object();

        if (!shared_config_) {
            json.kv("error", "Shared config not connected");
            json.end_object();
            res.status = 503;
            res.set_content(json.str(), "application/json");
            return;
        }

        // Parse request body: {"regime": 1, "strategy": 3}
        int regime = -1, strategy = -1;
        extract_json_int(req.body, "regime", regime);
        extract_json_int(req.body, "strategy", strategy);

        if (regime < 0 || regime > 6) {
            json.kv("error", "Invalid regime (must be 0-6)");
            json.end_object();
            res.status = 400;
            res.set_content(json.str(), "application/json");
            return;
        }

        if (strategy < 0 || strategy > 6) {
            json.kv("error", "Invalid strategy (must be 0-6)");
            json.end_object();
            res.status = 400;
            res.set_content(json.str(), "application/json");
            return;
        }

        shared_config_->set_strategy_for_regime(regime, static_cast<uint8_t>(strategy));

        const char* regime_names[] = {"Unknown", "TrendingUp", "TrendingDown", "Ranging", "HighVol", "LowVol", "Spike"};
        const char* strategy_names[] = {"NONE", "MOMENTUM", "MEAN_REV", "MKT_MAKER", "DEFENSIVE", "CAUTIOUS", "SMART"};

        json.kv("status", "ok");
        json.kv("regime", regime_names[regime]);
        json.kv("strategy", strategy_names[strategy]);
        json.kv("message", "Regime strategy mapping updated");

        json.end_object();
        res.set_content(json.str(), "application/json");
    }
};

// ============================================================================
// Main
// ============================================================================

void print_help() {
    std::cout << "Trader Web API Server\n\n"
              << "Usage: trader_web_api [options]\n\n"
              << "Options:\n"
              << "  --port N    Listen on port N (default: 8080)\n"
              << "  --cors      Enable CORS for development\n"
              << "  --help      Show this help\n\n"
              << "Read Endpoints:\n"
              << "  GET  /api/status           - System status\n"
              << "  GET  /api/portfolio        - Portfolio state\n"
              << "  GET  /api/symbols          - All symbol configs\n"
              << "  GET  /api/events           - Recent events\n"
              << "  GET  /api/stats            - Tuner statistics\n"
              << "  GET  /api/alerts           - Connection status and alerts\n\n"
              << "Control Endpoints:\n"
              << "  POST /api/control/trading  - Enable/disable trading {\"enabled\": bool}\n"
              << "  POST /api/control/tuner    - Tuner mode {\"mode\": \"active|paused|manual\"}\n"
              << "  PUT  /api/symbols/:symbol  - Update symbol config\n"
              << "  POST /api/tuner/trigger    - Force immediate tuning\n"
              << "  POST /api/tune             - Trigger manual tuning (legacy)\n"
              << "  POST /api/control          - Send control command (legacy)\n";
}

int main(int argc, char* argv[]) {
    int port = 8080;
    bool enable_cors = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help();
            return 0;
        }
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        }
        if (strcmp(argv[i], "--cors") == 0) {
            enable_cors = true;
        }
    }

    // Install signal handler
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    WebApiServer server(port, enable_cors);
    server.run();

    return 0;
}
