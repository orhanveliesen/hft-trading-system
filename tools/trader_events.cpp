/**
 * HFT Event Viewer
 *
 * CLI tool to monitor and query the event log.
 *
 * Usage:
 *   hft_events              - Follow live events
 *   hft_events --tail 100   - Show last 100 events
 *   hft_events --symbol BTC - Filter by symbol
 *   hft_events --type fill  - Filter by event type
 *   hft_events --stats      - Show statistics
 */

#include "../include/ipc/shared_event_log.hpp"

#include <chrono>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <thread>
#include <unistd.h>

using namespace hft::ipc;

// Global flag for clean shutdown
volatile sig_atomic_t g_running = 1;

void signal_handler(int) {
    g_running = 0;
}

// Format timestamp as HH:MM:SS.mmm
std::string format_time(uint64_t ns) {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    int64_t age_ms = (now - ns) / 1'000'000;

    if (age_ms < 1000) {
        return std::to_string(age_ms) + "ms ago";
    } else if (age_ms < 60000) {
        return std::to_string(age_ms / 1000) + "s ago";
    } else {
        return std::to_string(age_ms / 60000) + "m ago";
    }
}

// Format P&L with color
std::string format_pnl(int64_t pnl_x100) {
    double pnl = pnl_x100 / 100.0;
    char buf[32];

    if (pnl >= 0) {
        snprintf(buf, sizeof(buf), "\033[32m+$%.2f\033[0m", pnl); // Green
    } else {
        snprintf(buf, sizeof(buf), "\033[31m-$%.2f\033[0m", -pnl); // Red
    }
    return buf;
}

// Format event for display
void print_event(const TunerEvent& e) {
    // Severity color
    const char* sev_color = "";
    switch (e.severity) {
    case Severity::Warning:
        sev_color = "\033[33m";
        break; // Yellow
    case Severity::Critical:
        sev_color = "\033[31m";
        break; // Red
    default:
        break;
    }

    // Type color
    const char* type_color = "\033[0m";
    if (e.is_trade_event()) {
        type_color = "\033[36m"; // Cyan
    } else if (e.is_tuner_event()) {
        type_color = "\033[35m"; // Magenta
    } else if (e.is_market_event()) {
        type_color = "\033[34m"; // Blue
    }

    // Base output
    std::cout << std::setw(8) << e.sequence << " " << std::setw(10) << format_time(e.timestamp_ns) << " " << sev_color
              << type_color << std::setw(12) << e.type_name() << "\033[0m " << std::setw(10) << e.symbol << " ";

    // Type-specific output
    switch (e.type) {
    case TunerEventType::Signal:
    case TunerEventType::Order:
    case TunerEventType::Fill:
        std::cout << (e.payload.trade.side == TradeSide::Buy ? "BUY " : "SELL") << " " << std::fixed
                  << std::setprecision(2) << e.payload.trade.quantity << " @ " << std::setprecision(4)
                  << e.payload.trade.price;
        if (e.type == TunerEventType::Fill && e.payload.trade.pnl_x100 != 0) {
            std::cout << " " << format_pnl(e.payload.trade.pnl_x100);
        }
        break;

    case TunerEventType::ConfigChange:
        std::cout << e.payload.config.param_name << ": " << (e.payload.config.old_value_x100 / 100.0) << " -> "
                  << (e.payload.config.new_value_x100 / 100.0) << " (conf:" << (int)e.payload.config.ai_confidence
                  << "%)";
        break;

    case TunerEventType::RegimeChange:
        std::cout << "regime " << (int)e.payload.regime.old_regime << " -> " << (int)e.payload.regime.new_regime
                  << " (conf:" << std::fixed << std::setprecision(1) << (e.payload.regime.new_confidence * 100) << "%)";
        break;

    case TunerEventType::AIDecision:
        std::cout << "action=" << (int)e.payload.ai.action_taken << " conf=" << (int)e.payload.ai.confidence << "%"
                  << " lat=" << e.payload.ai.latency_ms << "ms";
        break;

    case TunerEventType::Error:
        std::cout << "\033[31m" << e.payload.error.component << " code=" << e.payload.error.error_code << "\033[0m";
        break;

    default:
        break;
    }

    // Reason if present
    if (e.reason[0] != '\0') {
        std::cout << " | " << e.reason;
    }

    std::cout << "\n";
}

void print_stats(const SharedEventLog* log) {
    std::cout << "\n=== Event Log Statistics ===\n\n";

    std::cout << "Total events: " << log->total_events.load() << "\n";
    std::cout << "Current position: " << log->current_position() << "\n";
    std::cout << "Session P&L: " << format_pnl(log->session_pnl_x100.load()) << "\n\n";

    // Symbol stats
    std::cout << "=== Per-Symbol Stats ===\n\n";
    std::cout << std::setw(12) << "Symbol" << std::setw(10) << "Signals" << std::setw(10) << "Fills" << std::setw(10)
              << "Win%" << std::setw(12) << "Session P&L" << std::setw(12) << "Total P&L" << std::setw(10) << "Configs"
              << "\n";
    std::cout << std::string(76, '-') << "\n";

    uint32_t count = log->symbol_count.load();
    for (uint32_t i = 0; i < count; ++i) {
        const auto& s = log->symbol_stats[i];
        if (s.is_empty())
            continue;

        std::cout << std::setw(12) << s.symbol << std::setw(10) << s.signal_count.load() << std::setw(10)
                  << s.fill_count.load() << std::setw(9) << std::fixed << std::setprecision(1) << s.win_rate() << "%"
                  << std::setw(12) << format_pnl(s.session_pnl_x100.load()) << std::setw(12)
                  << format_pnl(s.total_pnl_x100.load()) << std::setw(10) << s.config_changes.load() << "\n";
    }

    // Tuner stats
    std::cout << "\n=== Tuner Stats ===\n\n";
    const auto& t = log->tuner_stats;
    std::cout << "AI Decisions: " << t.total_decisions.load() << "\n";
    std::cout << "Config Changes: " << t.config_changes.load() << "\n";
    std::cout << "Pauses: " << t.pauses_triggered.load() << "\n";
    std::cout << "Emergency Exits: " << t.emergency_exits.load() << "\n";
    std::cout << "Avg Latency: " << std::fixed << std::setprecision(1) << t.avg_latency_ms() << " ms\n";
    std::cout << "Total Cost: $" << std::setprecision(4) << t.total_cost() << "\n";
}

void print_help() {
    std::cout << "HFT Event Viewer\n\n"
              << "Usage: hft_events [options]\n\n"
              << "Options:\n"
              << "  --tail N        Show last N events (default: follow live)\n"
              << "  --symbol SYM    Filter by symbol (e.g., BTCUSDT)\n"
              << "  --type TYPE     Filter by type (signal, fill, config, etc.)\n"
              << "  --stats         Show statistics only\n"
              << "  --help          Show this help\n\n"
              << "Event Types:\n"
              << "  signal, order, fill, cancel, config, pause, resume,\n"
              << "  emergency, ai, regime, news, error\n";
}

TunerEventType parse_event_type(const char* type) {
    if (strcmp(type, "signal") == 0)
        return TunerEventType::Signal;
    if (strcmp(type, "order") == 0)
        return TunerEventType::Order;
    if (strcmp(type, "fill") == 0)
        return TunerEventType::Fill;
    if (strcmp(type, "cancel") == 0)
        return TunerEventType::Cancel;
    if (strcmp(type, "config") == 0)
        return TunerEventType::ConfigChange;
    if (strcmp(type, "pause") == 0)
        return TunerEventType::PauseSymbol;
    if (strcmp(type, "resume") == 0)
        return TunerEventType::ResumeSymbol;
    if (strcmp(type, "emergency") == 0)
        return TunerEventType::EmergencyExit;
    if (strcmp(type, "ai") == 0)
        return TunerEventType::AIDecision;
    if (strcmp(type, "regime") == 0)
        return TunerEventType::RegimeChange;
    if (strcmp(type, "news") == 0)
        return TunerEventType::NewsEvent;
    if (strcmp(type, "error") == 0)
        return TunerEventType::Error;
    return TunerEventType::Signal; // Default
}

int main(int argc, char* argv[]) {
    // Parse arguments
    int tail_count = 0;
    const char* filter_symbol = nullptr;
    TunerEventType filter_type = TunerEventType::Signal;
    bool has_type_filter = false;
    bool show_stats = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help();
            return 0;
        }
        if (strcmp(argv[i], "--stats") == 0) {
            show_stats = true;
        }
        if (strcmp(argv[i], "--tail") == 0 && i + 1 < argc) {
            tail_count = atoi(argv[++i]);
        }
        if (strcmp(argv[i], "--symbol") == 0 && i + 1 < argc) {
            filter_symbol = argv[++i];
        }
        if (strcmp(argv[i], "--type") == 0 && i + 1 < argc) {
            filter_type = parse_event_type(argv[++i]);
            has_type_filter = true;
        }
    }

    // Open event log
    auto* log = SharedEventLog::open_readonly();
    if (!log) {
        std::cerr << "Error: Could not open event log. Is hft running?\n";
        return 1;
    }

    // Stats mode
    if (show_stats) {
        print_stats(log);
        return 0;
    }

    // Install signal handler
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Print header
    std::cout << std::setw(8) << "SEQ" << std::setw(11) << "AGE" << std::setw(13) << "TYPE" << std::setw(11) << "SYMBOL"
              << "DETAILS\n";
    std::cout << std::string(80, '-') << "\n";

    // Tail mode: show last N events
    if (tail_count > 0) {
        uint64_t current = log->current_position();
        uint64_t start = current > static_cast<uint64_t>(tail_count) ? current - tail_count : 0;

        for (uint64_t seq = start; seq < current; ++seq) {
            const TunerEvent* e = log->get_event(seq);
            if (!e)
                continue;

            // Apply filters
            if (filter_symbol && strcmp(e->symbol, filter_symbol) != 0)
                continue;
            if (has_type_filter && e->type != filter_type)
                continue;

            print_event(*e);
        }
        return 0;
    }

    // Follow mode: stream live events
    std::cout << "[Following live events. Press Ctrl+C to exit]\n\n";

    uint64_t last_pos = log->current_position();

    while (g_running) {
        uint64_t current = log->current_position();

        // Check for new events
        for (uint64_t seq = last_pos; seq < current; ++seq) {
            const TunerEvent* e = log->get_event(seq);
            if (!e)
                continue;

            // Apply filters
            if (filter_symbol && strcmp(e->symbol, filter_symbol) != 0)
                continue;
            if (has_type_filter && e->type != filter_type)
                continue;

            print_event(*e);
        }

        last_pos = current;

        // Poll interval
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\n[Stopped]\n";
    return 0;
}
