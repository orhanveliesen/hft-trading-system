/**
 * HFT AI Tuner
 *
 * AI-driven parameter tuning using Claude API.
 * Monitors trading performance and adjusts per-symbol configurations.
 *
 * Usage:
 *   hft_tuner                  # Start tuner (connects to HFT via shared memory)
 *   hft_tuner --dry-run        # Don't apply changes, just log recommendations
 *   hft_tuner --interval 300   # Tune every 300 seconds (default: 300)
 *   hft_tuner --verbose        # Verbose logging
 *
 * Environment:
 *   CLAUDE_API_KEY             # Required: Anthropic API key
 *   HFT_TUNER_MODEL            # Optional: Model to use (default: claude-3-haiku-20240307)
 */

#include "../include/ipc/symbol_config.hpp"
#include "../include/ipc/shared_event_log.hpp"
#include "../include/ipc/shared_config.hpp"
#include "../include/ipc/shared_portfolio_state.hpp"
#include "../include/ipc/shared_ring_buffer.hpp"
#include "../include/ipc/trade_event.hpp"
#include "../include/tuner/claude_client.hpp"
#include "../include/tuner/news_client.hpp"
#include "../include/tuner/rag_client.hpp"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <cstring>
#include <thread>
#include <chrono>
#include <csignal>
#include <unistd.h>
#include <vector>
#include <cstdlib>
#include <ctime>
#include <sstream>

using namespace hft::ipc;
using namespace hft::tuner;

// ============================================================================
// Timestamp Helper
// ============================================================================

std::string format_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::tm tm_buf;
    localtime_r(&time_t_now, &tm_buf);

    std::ostringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
       << "." << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

std::string format_timestamp_file() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);

    std::tm tm_buf;
    localtime_r(&time_t_now, &tm_buf);

    std::ostringstream ss;
    ss << std::put_time(&tm_buf, "%Y%m%d");
    return ss.str();
}

// ============================================================================
// Tuning History Logger
// ============================================================================

class TuningHistoryLogger {
public:
    TuningHistoryLogger(const std::string& log_dir = "../logs") {
        // Create log directory if needed (relative to build dir)
        std::string cmd = "mkdir -p " + log_dir;
        (void)system(cmd.c_str());

        // Open log file with date
        std::string filename = log_dir + "/tuning_history_" + format_timestamp_file() + ".log";
        file_.open(filename, std::ios::app);

        if (file_.is_open()) {
            std::cout << "[HISTORY] Logging to " << filename << "\n";
            // Write header if file is empty/new
            file_.seekp(0, std::ios::end);
            if (file_.tellp() == 0) {
                file_ << "# HFT AI Tuner History Log\n";
                file_ << "# Format: timestamp | trigger | action | symbol | confidence | reason | config_changes | applied\n";
                file_ << "# Started: " << format_timestamp() << "\n";
                file_ << std::string(100, '-') << "\n";
                file_.flush();
            }
        }
    }

    ~TuningHistoryLogger() {
        if (file_.is_open()) {
            file_.close();
        }
    }

    void log_tuning_decision(
        TriggerReason trigger,
        const ClaudeResponse& response,
        bool applied,
        const std::string& news_context = "")
    {
        std::ostringstream console_log;
        std::ostringstream file_log;

        std::string ts = format_timestamp();
        const auto& cmd = response.command;

        // Console output (always show)
        console_log << "\n";
        console_log << "╔══════════════════════════════════════════════════════════════════════════════╗\n";
        console_log << "║ TUNING DECISION @ " << ts << std::string(40, ' ') << "║\n";
        console_log << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
        console_log << "║ Trigger:    " << std::left << std::setw(65) << trigger_name(trigger) << "║\n";
        console_log << "║ Action:     " << std::left << std::setw(65) << action_name(cmd.action) << "║\n";
        console_log << "║ Symbol:     " << std::left << std::setw(65) << (cmd.symbol[0] ? cmd.symbol : "*") << "║\n";
        console_log << "║ Confidence: " << std::left << std::setw(65) << std::to_string((int)cmd.confidence) + "%" << "║\n";
        console_log << "║ Urgency:    " << std::left << std::setw(65) << std::to_string((int)cmd.urgency) << "║\n";

        // Reason (may be long, truncate for display)
        std::string reason_display = cmd.reason;
        if (reason_display.length() > 60) {
            reason_display = reason_display.substr(0, 57) + "...";
        }
        console_log << "║ Reason:     " << std::left << std::setw(65) << reason_display << "║\n";

        // Config changes if applicable
        if (cmd.action == TunerCommand::Action::UpdateSymbolConfig) {
            console_log << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
            console_log << "║ CONFIG CHANGES:                                                              ║\n";
            console_log << "║   EMA Dev (Trending): " << std::left << std::setw(54)
                       << std::to_string(cmd.config.ema_dev_trending_x100 / 100.0) + "%" << "║\n";
            console_log << "║   EMA Dev (Ranging):  " << std::left << std::setw(54)
                       << std::to_string(cmd.config.ema_dev_ranging_x100 / 100.0) + "%" << "║\n";
            console_log << "║   Base Position:      " << std::left << std::setw(54)
                       << std::to_string(cmd.config.base_position_x100 / 100.0) + "%" << "║\n";
            console_log << "║   Cooldown:           " << std::left << std::setw(54)
                       << std::to_string(cmd.config.cooldown_ms) + "ms" << "║\n";
            console_log << "║   Target:             " << std::left << std::setw(54)
                       << std::to_string(cmd.config.target_pct_x100 / 100.0) + "%" << "║\n";
            console_log << "║   Stop Loss:          " << std::left << std::setw(54)
                       << std::to_string(cmd.config.stop_pct_x100 / 100.0) + "%" << "║\n";
        }

        console_log << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
        console_log << "║ API Stats:  " << "HTTP " << response.http_code
                   << " | Latency: " << response.latency_ms << "ms"
                   << " | Tokens: " << response.input_tokens << "/" << response.output_tokens;
        int padding = 77 - 14 - std::to_string(response.http_code).length() -
                     std::to_string(response.latency_ms).length() -
                     std::to_string(response.input_tokens).length() -
                     std::to_string(response.output_tokens).length() - 25;
        console_log << std::string(std::max(0, padding), ' ') << "║\n";

        std::string status = applied ? "✓ APPLIED" : (response.success ? "○ NOT APPLIED (dry-run or no-change)" : "✗ FAILED");
        console_log << "║ Status:     " << std::left << std::setw(65) << status << "║\n";
        console_log << "╚══════════════════════════════════════════════════════════════════════════════╝\n";

        std::cout << console_log.str();
        std::cout.flush();

        // File log (structured, one line per decision)
        if (file_.is_open()) {
            file_log << ts << " | "
                    << trigger_name(trigger) << " | "
                    << action_name(cmd.action) << " | "
                    << (cmd.symbol[0] ? cmd.symbol : "*") << " | "
                    << (int)cmd.confidence << "% | "
                    << cmd.reason << " | ";

            if (cmd.action == TunerCommand::Action::UpdateSymbolConfig) {
                file_log << "ema_trend=" << (cmd.config.ema_dev_trending_x100 / 100.0)
                        << ",ema_range=" << (cmd.config.ema_dev_ranging_x100 / 100.0)
                        << ",pos=" << (cmd.config.base_position_x100 / 100.0)
                        << ",cool=" << cmd.config.cooldown_ms
                        << ",target=" << (cmd.config.target_pct_x100 / 100.0)
                        << ",stop=" << (cmd.config.stop_pct_x100 / 100.0);
            } else {
                file_log << "-";
            }

            file_log << " | " << (applied ? "APPLIED" : "NOT_APPLIED") << "\n";

            file_ << file_log.str();
            file_.flush();
        }
    }

    void log_error(const std::string& error, TriggerReason trigger) {
        std::string ts = format_timestamp();

        std::cout << "\n[" << ts << "] [ERROR] Tuning failed: " << error << "\n";

        if (file_.is_open()) {
            file_ << ts << " | " << trigger_name(trigger)
                 << " | ERROR | - | - | " << error << " | - | FAILED\n";
            file_.flush();
        }
    }

    void log_no_change(TriggerReason trigger, const ClaudeResponse& response) {
        std::string ts = format_timestamp();

        std::cout << "[" << ts << "] [TUNING] No changes recommended"
                 << " (HTTP " << response.http_code
                 << ", " << response.latency_ms << "ms"
                 << ", tokens: " << response.input_tokens << "/" << response.output_tokens << ")\n";

        if (file_.is_open()) {
            file_ << ts << " | " << trigger_name(trigger)
                 << " | NO_CHANGE | * | " << (int)response.command.confidence << "% | "
                 << response.command.reason << " | - | OK\n";
            file_.flush();
        }
    }

private:
    std::ofstream file_;

    const char* trigger_name(TriggerReason trigger) {
        switch (trigger) {
            case TriggerReason::Scheduled: return "SCHEDULED";
            case TriggerReason::LossThreshold: return "LOSS_THRESHOLD";
            case TriggerReason::ConsecutiveLosses: return "CONSEC_LOSSES";
            case TriggerReason::WinStreak: return "WIN_STREAK";
            case TriggerReason::VolatilitySpike: return "VOLATILITY";
            case TriggerReason::NewsTriggered: return "NEWS";
            case TriggerReason::ManualRequest: return "MANUAL";
            case TriggerReason::StartupInit: return "STARTUP";
            case TriggerReason::RegimeChange: return "REGIME_CHANGE";
            case TriggerReason::DrawdownAlert: return "DRAWDOWN";
            default: return "UNKNOWN";
        }
    }

    const char* action_name(TunerCommand::Action action) {
        switch (action) {
            case TunerCommand::Action::NoChange: return "NO_CHANGE";
            case TunerCommand::Action::UpdateSymbolConfig: return "UPDATE_CONFIG";
            case TunerCommand::Action::PauseSymbol: return "PAUSE";
            case TunerCommand::Action::ResumeSymbol: return "RESUME";
            case TunerCommand::Action::PauseAllTrading: return "PAUSE_ALL";
            case TunerCommand::Action::ResumeAllTrading: return "RESUME_ALL";
            case TunerCommand::Action::EmergencyExitSymbol: return "EMERGENCY_EXIT";
            case TunerCommand::Action::EmergencyExitAll: return "EMERGENCY_EXIT_ALL";
            default: return "UNKNOWN";
        }
    }
};

// Global flag for clean shutdown
volatile sig_atomic_t g_running = 1;

void signal_handler(int) {
    g_running = 0;
}

// ============================================================================
// CLI Arguments
// ============================================================================

struct TunerArgs {
    bool dry_run = false;
    bool verbose = false;
    int interval_sec = 300;     // 5 minutes default
    int loss_threshold = 3;     // Tune after N consecutive losses
    double loss_pct_trigger = 2.0;  // Tune if symbol loses >N%
    std::string model;          // Claude model (empty = use default/env)
};

TunerArgs parse_args(int argc, char* argv[]) {
    TunerArgs args;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--dry-run") == 0) {
            args.dry_run = true;
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            args.verbose = true;
        } else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
            args.interval_sec = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--loss-threshold") == 0 && i + 1 < argc) {
            args.loss_threshold = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            args.model = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            std::cout << "HFT AI Tuner - Claude-powered parameter optimization\n\n"
                      << "Usage: hft_tuner [options]\n\n"
                      << "Options:\n"
                      << "  --dry-run          Don't apply changes, just log recommendations\n"
                      << "  --verbose, -v      Verbose logging\n"
                      << "  --interval N       Tune every N seconds (default: 300)\n"
                      << "  --loss-threshold N Tune after N consecutive losses (default: 3)\n"
                      << "  --model MODEL      Claude model to use (default: claude-3-opus-20240229)\n"
                      << "  --help, -h         Show this help\n\n"
                      << "Models:\n"
                      << "  claude-3-opus-20240229     Best reasoning (default, recommended)\n"
                      << "  claude-3-5-sonnet-20241022 Good balance of speed/quality\n"
                      << "  claude-3-haiku-20240307    Fastest, basic reasoning\n\n"
                      << "Environment:\n"
                      << "  CLAUDE_API_KEY     Required: Anthropic API key\n"
                      << "  HFT_TUNER_MODEL    Optional: Model (overridden by --model)\n";
            exit(0);
        }
    }
    return args;
}

// ============================================================================
// Performance Tracker (aggregates data for tuning decisions)
// ============================================================================

struct SymbolPerformance {
    char symbol[16];
    int32_t trades_session = 0;
    int32_t wins_session = 0;
    int32_t consecutive_losses = 0;
    int32_t consecutive_wins = 0;
    double pnl_session = 0.0;
    double current_price = 0.0;
    uint8_t current_regime = 0;
    uint64_t last_fill_ns = 0;
    uint64_t last_tuned_ns = 0;

    double win_rate() const {
        return trades_session > 0 ? (100.0 * wins_session / trades_session) : 0.0;
    }

    bool needs_tuning(int loss_threshold, double loss_pct) const {
        // Tune if consecutive losses exceed threshold
        if (consecutive_losses >= loss_threshold) return true;

        // Tune if session loss exceeds percentage of initial capital
        // (This would need initial capital info, simplified for now)
        if (pnl_session < -100.0) return true;  // Placeholder

        return false;
    }
};

// ============================================================================
// Tuner Application
// ============================================================================

class TunerApp {
public:
    explicit TunerApp(const TunerArgs& args)
        : args_(args)
        , history_logger_("../logs")
    {
        // Open shared memory connections
        symbol_configs_ = SharedSymbolConfigs::open_rw("/trader_symbol_configs");
        if (!symbol_configs_) {
            // Try to create if doesn't exist
            symbol_configs_ = SharedSymbolConfigs::create("/trader_symbol_configs");
        }
        if (symbol_configs_) {
            symbol_configs_->tuner_connected.store(1);
            std::cout << "[IPC] Connected to symbol configs (symbols: "
                      << symbol_configs_->symbol_count.load() << ")\n";
        } else {
            std::cerr << "[WARN] Could not connect to symbol configs\n";
        }

        event_log_ = SharedEventLog::open_readwrite();
        if (event_log_) {
            std::cout << "[IPC] Connected to event log (events: "
                      << event_log_->total_events.load() << ")\n";
        } else {
            std::cerr << "[WARN] Could not connect to event log\n";
        }

        shared_config_ = SharedConfig::open_rw("/trader_config");
        if (shared_config_) {
            std::cout << "[IPC] Connected to shared config\n";
        } else {
            std::cerr << "[WARN] Could not connect to shared config - manual tune requests will not work!\n";
        }

        portfolio_state_ = SharedPortfolioState::open("/trader_portfolio");
        if (portfolio_state_) {
            std::cout << "[IPC] Connected to portfolio state (cash: $"
                      << std::fixed << std::setprecision(2)
                      << portfolio_state_->cash() << ")\n";
        }

        // Connect to trade event ring buffer (for dashboard visibility)
        try {
            trade_events_ = new SharedRingBuffer<TradeEvent>("/trader_events", false);
            std::cout << "[IPC] Connected to trade events ring buffer\n";
        } catch (const std::exception& e) {
            std::cerr << "[WARN] Could not connect to trade events: " << e.what() << "\n";
            trade_events_ = nullptr;
        }

        // Initialize Claude client
        if (!args_.model.empty()) {
            claude_.set_model(args_.model);
        }
        if (!claude_.is_valid()) {
            std::cerr << "[WARN] Claude API not configured, running in monitor-only mode\n";
        } else {
            std::cout << "[AI] Using model: " << claude_.model() << "\n";
        }

        // Initialize news client
        if (news_.is_valid()) {
            std::cout << "[NEWS] News client initialized\n";
        } else {
            std::cerr << "[WARN] News client not available\n";
        }

        // Initialize RAG client
        auto rag_health = rag_.health_check();
        if (rag_health.success && rag_health.is_healthy) {
            std::cout << "[RAG] Connected to RAG service (docs: "
                      << rag_health.collection_size << ", model: "
                      << rag_health.model << ")\n";
        } else {
            std::cerr << "[WARN] RAG service not available: " << rag_health.error << "\n";
            std::cerr << "[WARN] Start with: cd rag_service && python rag_server.py\n";
        }
    }

    ~TunerApp() {
        if (symbol_configs_) {
            symbol_configs_->tuner_connected.store(0);
        }
    }

    void run() {
        std::cout << "[TUNER] Starting AI tuner (interval: "
                  << args_.interval_sec << "s, dry_run: "
                  << (args_.dry_run ? "true" : "false") << ")\n";

        uint64_t last_scheduled_tune = 0;
        uint64_t last_event_seq = event_log_ ? event_log_->current_position() : 0;

        while (g_running) {
            uint64_t now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
            TriggerReason trigger = TriggerReason::None;

            // Check for manual tune request (dashboard/API triggered)
            if (shared_config_ && shared_config_->should_tune_now()) {
                trigger = TriggerReason::ManualRequest;
                shared_config_->clear_manual_tune_request();
                std::cout << "[" << format_timestamp()
                          << "] [TUNING] Manual tune request received\n";
            }

            // Check for scheduled tuning (only if no manual request)
            if (trigger == TriggerReason::None) {
                uint64_t interval_ns = args_.interval_sec * 1'000'000'000ULL;
                if (now_ns - last_scheduled_tune > interval_ns) {
                    trigger = TriggerReason::Scheduled;
                    last_scheduled_tune = now_ns;
                }
            }

            // Process new events
            if (event_log_) {
                process_events(last_event_seq, trigger);
                last_event_seq = event_log_->current_position();
            }

            // Run tuning if triggered
            if (trigger != TriggerReason::None) {
                // Check pause flags (skip scheduled but allow manual)
                bool is_paused = shared_config_ && shared_config_->is_tuner_paused();
                bool is_manual_override = shared_config_ && shared_config_->is_manual_override();

                if (is_paused && trigger != TriggerReason::ManualRequest) {
                    if (args_.verbose) {
                        std::cout << "[" << format_timestamp()
                                  << "] [TUNING] Skipped - Tuner is paused\n";
                    }
                } else if (is_manual_override && trigger != TriggerReason::ManualRequest) {
                    if (args_.verbose) {
                        std::cout << "[" << format_timestamp()
                                  << "] [TUNING] Skipped - Manual Override active\n";
                    }
                } else {
                    run_tuning(trigger);
                }
            }

            // Sleep between checks
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        std::cout << "[TUNER] Shutting down\n";
    }

private:
    TunerArgs args_;
    SharedSymbolConfigs* symbol_configs_ = nullptr;
    SharedEventLog* event_log_ = nullptr;
    SharedConfig* shared_config_ = nullptr;
    SharedPortfolioState* portfolio_state_ = nullptr;
    SharedRingBuffer<TradeEvent>* trade_events_ = nullptr;
    ClaudeClient claude_;
    NewsClient news_;
    RagClient rag_;
    TuningHistoryLogger history_logger_;
    std::atomic<uint32_t> event_seq_{0};  // Sequence number for trade events

    // Per-symbol performance tracking
    std::array<SymbolPerformance, 32> symbol_perf_{};

    // Publish TradeEvent to ring buffer for dashboard visibility
    void publish_trade_event(const char* symbol, StatusCode code, uint8_t confidence) {
        if (!trade_events_) return;

        uint64_t now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
        uint32_t seq = event_seq_.fetch_add(1);

        // Extract short ticker (first 3 chars)
        char ticker[4] = {0};
        for (int i = 0; i < 3 && symbol[i]; ++i) {
            ticker[i] = symbol[i];
        }

        TradeEvent e = TradeEvent::tuner_config(seq, now_ns, 0, ticker, code, confidence);
        trade_events_->push(e);

        if (args_.verbose) {
            std::cout << "[EVENT] Published " << TradeEvent::status_code_name(code)
                      << " for " << symbol << " [" << (int)confidence << "% conf]\n";
        }
    }

    void process_events(uint64_t since_seq, TriggerReason& trigger) {
        uint64_t current = event_log_->current_position();
        if (since_seq >= current) return;

        for (uint64_t seq = since_seq; seq < current; ++seq) {
            const TunerEvent* e = event_log_->get_event(seq);
            if (!e) continue;

            // Find or create symbol performance entry
            SymbolPerformance* perf = find_or_create_perf(e->symbol);
            if (!perf) continue;

            switch (e->type) {
                case TunerEventType::Fill:
                    perf->trades_session++;
                    if (e->payload.trade.pnl_x100 >= 0) {
                        perf->wins_session++;
                        perf->consecutive_wins++;
                        perf->consecutive_losses = 0;
                    } else {
                        perf->consecutive_losses++;
                        perf->consecutive_wins = 0;

                        // Check if we should trigger tuning
                        if (perf->consecutive_losses >= args_.loss_threshold) {
                            trigger = TriggerReason::ConsecutiveLosses;
                            if (args_.verbose) {
                                std::cout << "[TRIGGER] " << e->symbol
                                          << " hit " << perf->consecutive_losses
                                          << " consecutive losses\n";
                            }
                        }
                    }
                    perf->pnl_session += e->payload.trade.pnl_x100 / 100.0;
                    perf->last_fill_ns = e->timestamp_ns;
                    break;

                case TunerEventType::RegimeChange:
                    perf->current_regime = e->payload.regime.new_regime;
                    if (args_.verbose) {
                        std::cout << "[REGIME] " << e->symbol
                                  << " changed to regime " << (int)perf->current_regime << "\n";
                    }
                    break;

                default:
                    break;
            }
        }
    }

    void run_tuning(TriggerReason trigger) {
        std::string ts = format_timestamp();
        std::cout << "[" << ts << "] [TUNING] Starting (trigger: " << (int)trigger << ")...\n";

        // Collect symbol performance data and convert to SymbolTuningData
        std::vector<SymbolTuningData> symbols;
        for (const auto& perf : symbol_perf_) {
            if (perf.symbol[0] != '\0') {
                SymbolTuningData data;
                std::strncpy(data.symbol, perf.symbol, 15);
                data.current_price = perf.current_price;
                data.ema_20 = 0;
                data.price_change_1m = 0;
                data.price_change_5m = 0;
                data.atr_14 = 0;
                data.trades_session = perf.trades_session;
                data.wins_session = perf.wins_session;
                data.pnl_session = perf.pnl_session;
                data.consecutive_losses = perf.consecutive_losses;
                data.consecutive_wins = perf.consecutive_wins;
                data.current_regime = perf.current_regime;

                // Get current config from shared memory
                if (symbol_configs_) {
                    const auto* cfg = symbol_configs_->find(perf.symbol);
                    if (cfg) {
                        data.current_config = *cfg;
                    } else {
                        data.current_config.init(perf.symbol);
                    }
                }

                // Get market snapshot from portfolio state
                data.has_snapshot = false;
                if (portfolio_state_) {
                    for (size_t i = 0; i < MAX_PORTFOLIO_SYMBOLS; ++i) {
                        const auto& pos = portfolio_state_->positions[i];
                        if (pos.active.load() && std::strncmp(pos.symbol, perf.symbol, 15) == 0) {
                            const auto& snap = pos.snapshot;
                            data.snapshot.price_high = snap.price_high();
                            data.snapshot.price_low = snap.price_low();
                            data.snapshot.price_open = snap.price_open();
                            data.snapshot.ema_20 = snap.ema_20();
                            data.snapshot.atr_14 = snap.atr_14();
                            data.snapshot.volume_sum = snap.volume_sum();
                            data.snapshot.volatility_pct = snap.volatility_pct();
                            data.snapshot.price_range_pct = snap.price_range_pct();
                            data.snapshot.tick_count = snap.tick_count.load();
                            data.snapshot.trend_direction = snap.trend_direction.load();
                            data.has_snapshot = (snap.tick_count.load() > 0);

                            // Update EMA and ATR from snapshot if available
                            if (data.snapshot.ema_20 > 0) data.ema_20 = data.snapshot.ema_20;
                            if (data.snapshot.atr_14 > 0) data.atr_14 = data.snapshot.atr_14;
                            break;
                        }
                    }
                }

                symbols.push_back(data);
            }
        }

        // Get portfolio state
        double portfolio_pnl = portfolio_state_ ?
            portfolio_state_->total_realized_pnl() : 0.0;
        double portfolio_cash = portfolio_state_ ?
            portfolio_state_->cash() : 10000.0;

        // Build cost metrics for AI analysis (CRITICAL for profitability)
        CostMetrics cost_metrics;
        bool has_cost_data = false;
        if (portfolio_state_) {
            cost_metrics.total_commissions = portfolio_state_->total_commissions();
            cost_metrics.total_slippage = portfolio_state_->total_slippage();
            cost_metrics.total_costs = portfolio_state_->total_costs();
            cost_metrics.total_volume = portfolio_state_->total_volume();
            cost_metrics.total_fills = portfolio_state_->total_fills.load();
            cost_metrics.total_targets = portfolio_state_->total_targets.load();
            cost_metrics.total_stops = portfolio_state_->total_stops.load();
            cost_metrics.cost_per_trade = portfolio_state_->cost_per_trade();
            cost_metrics.avg_trade_value = portfolio_state_->avg_trade_value();
            cost_metrics.cost_pct_per_trade = portfolio_state_->cost_pct_per_trade();
            cost_metrics.round_trip_cost_pct = cost_metrics.cost_pct_per_trade * 2;

            // P&L metrics
            cost_metrics.gross_pnl = portfolio_state_->gross_pnl();
            cost_metrics.net_pnl = portfolio_state_->total_realized_pnl() +
                                   portfolio_state_->total_unrealized_pnl();

            // Win rate
            uint32_t wins = portfolio_state_->winning_trades.load();
            uint32_t losses = portfolio_state_->losing_trades.load();
            cost_metrics.win_rate = (wins + losses) > 0 ?
                (100.0 * wins / (wins + losses)) : 0.0;

            // Profit factor (simplified: targets vs stops)
            cost_metrics.profit_factor = cost_metrics.total_stops > 0 ?
                (double)cost_metrics.total_targets / cost_metrics.total_stops : 0.0;

            // Trading frequency
            uint64_t start_ns = portfolio_state_->start_time_ns.load();
            uint64_t now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
            cost_metrics.session_duration_sec = (now_ns - start_ns) / 1'000'000'000ULL;

            // Avoid division by zero
            if (cost_metrics.session_duration_sec > 0) {
                double hours = cost_metrics.session_duration_sec / 3600.0;
                cost_metrics.trades_per_hour = hours > 0 ?
                    cost_metrics.total_fills / hours : 0.0;
            } else {
                cost_metrics.trades_per_hour = 0.0;
            }

            has_cost_data = (cost_metrics.total_fills > 0);

            if (args_.verbose && has_cost_data) {
                std::cout << "[" << format_timestamp() << "] [COST] "
                          << "Fills: " << cost_metrics.total_fills
                          << ", Costs: $" << std::fixed << std::setprecision(2) << cost_metrics.total_costs
                          << ", Trades/hr: " << std::setprecision(1) << cost_metrics.trades_per_hour
                          << ", Win%: " << std::setprecision(1) << cost_metrics.win_rate << "%\n";
            }
        }

        // Build context from RAG knowledge base
        std::string rag_context;
        auto rag_health = rag_.health_check();
        if (rag_health.success && rag_health.is_healthy) {
            // Get the primary symbol and regime for context building
            std::string primary_symbol;
            std::string primary_regime = "unknown";
            int max_losses = 0;
            double min_winrate = 100.0;

            // First, try to get symbol from trade events
            for (const auto& s : symbols) {
                if (s.consecutive_losses > max_losses) {
                    max_losses = s.consecutive_losses;
                    primary_symbol = s.symbol;
                    primary_regime = regime_name(s.current_regime);
                }
                double wr = s.trades_session > 0 ?
                    (100.0 * s.wins_session / s.trades_session) : 100.0;
                if (wr < min_winrate) {
                    min_winrate = wr;
                }
            }

            // Fallback: If no trade events, get symbol from config
            if (primary_symbol.empty() && symbol_configs_) {
                uint32_t count = symbol_configs_->symbol_count.load();
                if (count > 0) {
                    primary_symbol = symbol_configs_->symbols[0].symbol;
                    std::cout << "[" << format_timestamp() << "] [RAG] Using config symbol: "
                              << primary_symbol << " (no trades yet)\n";
                }
            }

            if (!primary_symbol.empty()) {
                rag_context = rag_.build_tuner_context(
                    primary_symbol,
                    primary_regime,
                    max_losses,
                    min_winrate
                );
                if (!rag_context.empty()) {
                    std::cout << "[" << format_timestamp() << "] [RAG] Retrieved "
                              << rag_context.size() << " bytes of context\n";
                }
            }
        }

        // Fetch news context
        std::string news_context;
        if (news_.is_valid()) {
            auto news = news_.fetch_all();
            if (news.success) {
                news_context = news.summary_for_prompt(5);
                std::cout << "[" << format_timestamp() << "] [NEWS] Fetched "
                          << news.items.size() << " news items\n";

                // Check for news-triggered tuning
                for (const auto& item : news.items) {
                    if (item.is_recent(300) && item.importance >= 80) {
                        // High-impact recent news
                        if (trigger == TriggerReason::None ||
                            trigger == TriggerReason::Scheduled) {
                            trigger = TriggerReason::NewsTriggered;
                            std::cout << "[" << format_timestamp() << "] [NEWS] High-impact news: "
                                      << item.title << "\n";
                        }
                    }
                }
            }
        }

        // Combine RAG and news context
        std::string combined_context;
        if (!rag_context.empty()) {
            combined_context += "## Knowledge Base Context\n";
            combined_context += rag_context + "\n";
        }
        if (!news_context.empty()) {
            combined_context += news_context;
        }

        // Request tuning from Claude (with cost analysis if available)
        std::cout << "[" << format_timestamp() << "] [API] Calling Claude ("
                  << claude_.model() << ")";
        if (has_cost_data) {
            std::cout << " [with cost data: " << cost_metrics.total_fills << " fills, $"
                      << std::fixed << std::setprecision(2) << cost_metrics.total_costs << " costs]";
        }
        std::cout << "...\n";
        std::cout.flush();

        auto api_start = std::chrono::steady_clock::now();
        ClaudeResponse response = claude_.request_tuning(
            symbols, portfolio_pnl, portfolio_cash, trigger, combined_context,
            has_cost_data ? &cost_metrics : nullptr);
        auto api_end = std::chrono::steady_clock::now();

        // Calculate latency if not set
        if (response.latency_ms == 0) {
            response.latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                api_end - api_start).count();
        }

        // Handle error case
        if (!response.success) {
            history_logger_.log_error(response.error, trigger);
            // Still count this as a tuning attempt (even though it failed)
            if (symbol_configs_) {
                symbol_configs_->tune_count.fetch_add(1);
                symbol_configs_->last_tune_ns.store(
                    std::chrono::steady_clock::now().time_since_epoch().count());
            }
            return;
        }

        // Check for actual change recommendation
        TunerCommand& cmd = response.command;
        bool has_action = cmd.is_valid() && cmd.action != TunerCommand::Action::NoChange;
        bool applied = false;

        if (has_action) {
            if (args_.dry_run) {
                // Log the decision but don't apply
                history_logger_.log_tuning_decision(trigger, response, false, news_context);
            } else {
                // Apply and log
                apply_command(cmd);
                applied = true;
                history_logger_.log_tuning_decision(trigger, response, true, news_context);
            }
        } else {
            // No change recommended
            history_logger_.log_no_change(trigger, response);
        }

        // Log tuning event to shared memory
        if (event_log_) {
            TunerEvent e = TunerEvent::make_ai_decision(
                cmd.confidence, cmd.urgency,
                static_cast<uint8_t>(cmd.action), response.latency_ms,
                cmd.reason);
            e.trigger = trigger;
            e.payload.ai.tokens_input = response.input_tokens;
            e.payload.ai.tokens_output = response.output_tokens;
            event_log_->log(e);
        }

        // Update tuner stats
        if (event_log_) {
            event_log_->tuner_stats.total_latency_ms.fetch_add(response.latency_ms);
            event_log_->tuner_stats.total_tokens_in.fetch_add(response.input_tokens);
            event_log_->tuner_stats.total_tokens_out.fetch_add(response.output_tokens);
        }

        // Update tune count (regardless of whether command was applied)
        // This tracks ALL tuning attempts, not just applied changes
        if (symbol_configs_) {
            symbol_configs_->tune_count.fetch_add(1);
            symbol_configs_->last_tune_ns.store(
                std::chrono::steady_clock::now().time_since_epoch().count());
        }
    }

    const char* action_name(TunerCommand::Action action) {
        switch (action) {
            case TunerCommand::Action::NoChange: return "NO_CHANGE";
            case TunerCommand::Action::UpdateSymbolConfig: return "UPDATE_CONFIG";
            case TunerCommand::Action::PauseSymbol: return "PAUSE";
            case TunerCommand::Action::ResumeSymbol: return "RESUME";
            case TunerCommand::Action::PauseAllTrading: return "PAUSE_ALL";
            case TunerCommand::Action::ResumeAllTrading: return "RESUME_ALL";
            case TunerCommand::Action::EmergencyExitSymbol: return "EMERGENCY_EXIT";
            case TunerCommand::Action::EmergencyExitAll: return "EMERGENCY_EXIT_ALL";
            default: return "UNKNOWN";
        }
    }

    void apply_command(const TunerCommand& cmd) {
        if (!symbol_configs_) return;

        switch (cmd.action) {
            case TunerCommand::Action::UpdateSymbolConfig:
                if (symbol_configs_->update(cmd.symbol, cmd.config)) {
                    std::cout << "[APPLY] Updated symbol config for " << cmd.symbol << "\n";

                    // Also sync to global shared_config_ so HFT uses these values
                    if (shared_config_) {
                        // EMA deviation thresholds
                        // Scale conversions: SymbolTuningConfig x100 -> SharedConfig (percentage * 10)
                        // x100=80 means 0.8% -> store as 8 (0.8 * 10)
                        shared_config_->ema_dev_trending_x1000.store(cmd.config.ema_dev_trending_x100 / 10);
                        shared_config_->ema_dev_ranging_x1000.store(cmd.config.ema_dev_ranging_x100 / 10);
                        shared_config_->ema_dev_highvol_x1000.store(cmd.config.ema_dev_highvol_x100 / 10);

                        // Position sizing
                        shared_config_->base_position_pct_x100.store(cmd.config.base_position_x100);
                        shared_config_->max_position_pct_x100.store(cmd.config.max_position_x100);

                        // Trade filtering
                        shared_config_->cooldown_ms.store(cmd.config.cooldown_ms);
                        shared_config_->signal_strength.store(cmd.config.signal_strength);

                        // Profit targets
                        shared_config_->target_pct_x100.store(cmd.config.target_pct_x100);
                        shared_config_->stop_pct_x100.store(cmd.config.stop_pct_x100);
                        shared_config_->pullback_pct_x100.store(cmd.config.pullback_pct_x100);

                        // Order execution preferences
                        shared_config_->order_type_default.store(cmd.config.order_type_preference);
                        shared_config_->limit_offset_bps_x100.store(cmd.config.limit_offset_bps_x100);
                        shared_config_->limit_timeout_ms.store(cmd.config.limit_timeout_ms);

                        shared_config_->sequence.fetch_add(1);

                        // Order type names for logging
                        const char* order_type_names[] = {"Auto", "MarketOnly", "LimitOnly", "Adaptive"};
                        uint8_t ot = cmd.config.order_type_preference;
                        const char* order_type_name = (ot < 4) ? order_type_names[ot] : "Auto";

                        std::cout << "[APPLY] Synced ALL params to global config (seq: "
                                  << shared_config_->sequence.load() << ")\n";
                        std::cout << "        EMA: trend=" << (cmd.config.ema_dev_trending_x100 / 100.0)
                                  << "%, range=" << (cmd.config.ema_dev_ranging_x100 / 100.0)
                                  << "%, hvol=" << (cmd.config.ema_dev_highvol_x100 / 100.0) << "%\n";
                        std::cout << "        Pos: base=" << (cmd.config.base_position_x100 / 100.0)
                                  << "%, max=" << (cmd.config.max_position_x100 / 100.0) << "%\n";
                        std::cout << "        T/S: target=" << (cmd.config.target_pct_x100 / 100.0)
                                  << "%, stop=" << (cmd.config.stop_pct_x100 / 100.0)
                                  << "%, pullback=" << (cmd.config.pullback_pct_x100 / 100.0) << "%\n";
                        std::cout << "        Order: type=" << order_type_name
                                  << ", offset=" << (cmd.config.limit_offset_bps_x100 / 100.0) << "bps"
                                  << ", timeout=" << cmd.config.limit_timeout_ms << "ms\n";
                    }

                    // Log config change event (to SharedEventLog for tuner stats)
                    if (event_log_) {
                        TunerEvent e;
                        e.init(TunerEventType::ConfigChange, cmd.symbol);
                        e.payload.config.ai_confidence = cmd.confidence;
                        e.set_reason(cmd.reason);
                        event_log_->log(e);
                    }

                    // Publish to TradeEvent ring buffer (for dashboard visibility)
                    publish_trade_event(cmd.symbol, StatusCode::TunerConfigUpdate, cmd.confidence);
                }
                break;

            case TunerCommand::Action::PauseSymbol: {
                auto* cfg = symbol_configs_->get_or_create(cmd.symbol);
                if (cfg) {
                    cfg->enabled = 0;
                    symbol_configs_->sequence.fetch_add(1);
                    std::cout << "[APPLY] Paused trading for " << cmd.symbol << "\n";

                    if (event_log_) {
                        TunerEvent e;
                        e.init(TunerEventType::PauseSymbol, cmd.symbol);
                        e.set_reason(cmd.reason);
                        event_log_->log(e);
                    }

                    // Publish to dashboard
                    publish_trade_event(cmd.symbol, StatusCode::TunerPauseSymbol, cmd.confidence);
                }
                break;
            }

            case TunerCommand::Action::ResumeSymbol: {
                auto* cfg = symbol_configs_->get_or_create(cmd.symbol);
                if (cfg) {
                    cfg->enabled = 1;
                    symbol_configs_->sequence.fetch_add(1);
                    std::cout << "[APPLY] Resumed trading for " << cmd.symbol << "\n";

                    if (event_log_) {
                        TunerEvent e;
                        e.init(TunerEventType::ResumeSymbol, cmd.symbol);
                        e.set_reason(cmd.reason);
                        event_log_->log(e);
                    }

                    // Publish to dashboard
                    publish_trade_event(cmd.symbol, StatusCode::TunerResumeSymbol, cmd.confidence);
                }
                break;
            }

            // TODO: Implement emergency exit actions
            default:
                break;
        }
        // Note: tune_count is now updated in run_tuning() after this returns
    }

    const char* regime_name(uint8_t regime) {
        switch (regime) {
            case 0: return "Unknown";
            case 1: return "TRENDING_UP";
            case 2: return "TRENDING_DOWN";
            case 3: return "RANGING";
            case 4: return "HIGH_VOLATILITY";
            case 5: return "LOW_VOLATILITY";
            case 6: return "SPIKE";
            default: return "Unknown";
        }
    }

    SymbolPerformance* find_or_create_perf(const char* symbol) {
        if (symbol[0] == '*' || symbol[0] == '\0') return nullptr;

        // Find existing
        for (auto& perf : symbol_perf_) {
            if (strcmp(perf.symbol, symbol) == 0) {
                return &perf;
            }
        }

        // Find empty slot
        for (auto& perf : symbol_perf_) {
            if (perf.symbol[0] == '\0') {
                strncpy(perf.symbol, symbol, 15);
                return &perf;
            }
        }

        return nullptr;
    }
};

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    TunerArgs args = parse_args(argc, argv);

    // Install signal handler
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    TunerApp app(args);
    app.run();

    return 0;
}
