/**
 * HFT Observer - Real-time event monitoring for HFT engine
 *
 * Reads events from shared memory ring buffer (published by hft engine)
 * and displays/logs them without impacting the hot path.
 *
 * Usage:
 *   hft_observer              # Real-time event stream
 *   hft_observer --stats      # Show statistics only
 *   hft_observer --log FILE   # Log events to file
 *   hft_observer -h           # Help
 */

#include "../include/ipc/trade_event.hpp"
#include "../include/ipc/shared_ring_buffer.hpp"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>
#include <cstring>
#include <map>

using namespace hft::ipc;

// ============================================================================
// Global State
// ============================================================================

std::atomic<bool> g_running{true};

void signal_handler(int) {
    std::cout << "\n[SHUTDOWN] Stopping observer...\n";
    g_running = false;
}

// ============================================================================
// Statistics
// ============================================================================

struct ObserverStats {
    uint64_t total_events = 0;
    uint64_t quotes = 0;
    uint64_t signals = 0;
    uint64_t orders = 0;
    uint64_t fills = 0;
    uint64_t targets = 0;
    uint64_t stops = 0;
    uint64_t regime_changes = 0;
    uint64_t errors = 0;

    int64_t total_pnl_cents = 0;
    int64_t realized_profit = 0;
    int64_t realized_loss = 0;

    std::map<uint32_t, std::string> symbols;  // symbol_id -> ticker

    void record(const TradeEvent& e) {
        total_events++;

        switch (e.type) {
            case EventType::Quote:
                quotes++;
                break;
            case EventType::Signal:
                signals++;
                break;
            case EventType::OrderSent:
                orders++;
                break;
            case EventType::Fill:
                fills++;
                break;
            case EventType::TargetHit:
                targets++;
                total_pnl_cents += e.pnl_cents;
                realized_profit += e.pnl_cents;
                break;
            case EventType::StopLoss:
                stops++;
                total_pnl_cents += e.pnl_cents;
                realized_loss += std::abs(e.pnl_cents);
                break;
            case EventType::RegimeChange:
                regime_changes++;
                break;
            case EventType::Error:
                errors++;
                break;
            default:
                break;
        }

        // Track symbols
        if (e.symbol_id > 0 && e.ticker[0] != '\0') {
            symbols[e.symbol_id] = std::string(e.ticker, 3);
        }
    }

    void print() const {
        std::cout << "\n";
        std::cout << "================================================================\n";
        std::cout << "  OBSERVER STATISTICS\n";
        std::cout << "================================================================\n";
        std::cout << "  Total Events:    " << total_events << "\n";
        std::cout << "  --------------------------------\n";
        std::cout << "  Quotes:          " << quotes << "\n";
        std::cout << "  Signals:         " << signals << "\n";
        std::cout << "  Orders:          " << orders << "\n";
        std::cout << "  Fills:           " << fills << "\n";
        std::cout << "  Targets Hit:     " << targets << "\n";
        std::cout << "  Stop Losses:     " << stops << "\n";
        std::cout << "  Regime Changes:  " << regime_changes << "\n";
        std::cout << "  Errors:          " << errors << "\n";
        std::cout << "  --------------------------------\n";
        std::cout << "  Realized P&L:    $" << std::fixed << std::setprecision(2)
                  << (total_pnl_cents / 100.0) << "\n";
        std::cout << "    Profit:        $" << (realized_profit / 100.0) << "\n";
        std::cout << "    Loss:          $" << (realized_loss / 100.0) << "\n";
        std::cout << "  Symbols Seen:    " << symbols.size() << "\n";
        std::cout << "================================================================\n";
    }
};

// ============================================================================
// Event Formatting
// ============================================================================

const char* event_type_str(EventType type) {
    switch (type) {
        case EventType::None:         return "NONE";
        case EventType::Quote:        return "QUOTE";
        case EventType::Signal:       return "SIGNAL";
        case EventType::OrderSent:    return "ORDER";
        case EventType::Fill:         return "FILL";
        case EventType::TargetHit:    return "TARGET";
        case EventType::StopLoss:     return "STOP";
        case EventType::RegimeChange: return "REGIME";
        case EventType::Error:        return "ERROR";
        default:                      return "???";
    }
}

const char* side_str(uint8_t side) {
    return side == 0 ? "BUY" : "SELL";
}

const char* regime_str(uint8_t regime) {
    switch (regime) {
        case 0: return "Unknown";
        case 1: return "TrendUp";
        case 2: return "TrendDn";
        case 3: return "Ranging";
        case 4: return "HighVol";
        case 5: return "LowVol";
        default: return "???";
    }
}

void print_event(const TradeEvent& e, bool verbose = true) {
    // Timestamp (relative, in ms)
    static uint64_t first_ts = 0;
    if (first_ts == 0) first_ts = e.timestamp_ns;
    double rel_ms = (e.timestamp_ns - first_ts) / 1'000'000.0;

    std::cout << std::fixed << std::setprecision(1);
    std::cout << "[" << std::setw(10) << rel_ms << "ms] ";
    std::cout << std::setw(7) << event_type_str(e.type) << " ";
    std::cout << std::setw(4) << std::string(e.ticker, 3) << " ";

    switch (e.type) {
        case EventType::Quote:
            std::cout << "bid=" << std::setprecision(2) << e.price
                      << " ask=" << e.price2;
            break;

        case EventType::Signal:
            std::cout << side_str(e.side) << " strength=" << (int)e.signal_strength
                      << " @ $" << std::setprecision(2) << e.price;
            break;

        case EventType::OrderSent:
            std::cout << side_str(e.side) << " " << e.quantity
                      << " @ $" << std::setprecision(2) << e.price
                      << " (order#" << e.order_id << ")";
            break;

        case EventType::Fill:
            std::cout << side_str(e.side) << " " << e.quantity
                      << " @ $" << std::setprecision(2) << e.price
                      << " (order#" << e.order_id << ")";
            break;

        case EventType::TargetHit:
            std::cout << "PROFIT! qty=" << e.quantity
                      << " entry=$" << std::setprecision(2) << e.price2
                      << " exit=$" << e.price
                      << " pnl=$" << (e.pnl_cents / 100.0);
            break;

        case EventType::StopLoss:
            std::cout << "LOSS! qty=" << e.quantity
                      << " entry=$" << std::setprecision(2) << e.price2
                      << " exit=$" << e.price
                      << " pnl=$" << (e.pnl_cents / 100.0);
            break;

        case EventType::RegimeChange:
            std::cout << "-> " << regime_str(e.regime);
            break;

        case EventType::Error:
            std::cout << "ERROR";
            break;

        default:
            break;
    }

    std::cout << "\n";
}

// ============================================================================
// Main
// ============================================================================

void print_usage() {
    std::cout << "Usage: hft_observer [options]\n";
    std::cout << "\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help       Show this help\n";
    std::cout << "  -s, --stats      Show statistics only (no event stream)\n";
    std::cout << "  -q, --quiet      Quiet mode (stats only, less output)\n";
    std::cout << "  -l, --log FILE   Log events to file\n";
    std::cout << "  -f, --filter T   Filter by event type (FILL, TARGET, STOP, etc.)\n";
    std::cout << "\n";
    std::cout << "The observer connects to shared memory created by the hft engine.\n";
    std::cout << "Make sure hft is running with --paper mode before starting observer.\n";
}

int main(int argc, char* argv[]) {
    // Parse arguments
    bool stats_only = false;
    bool quiet = false;
    std::string log_file;
    std::string filter_type;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        } else if (arg == "-s" || arg == "--stats") {
            stats_only = true;
        } else if (arg == "-q" || arg == "--quiet") {
            quiet = true;
        } else if ((arg == "-l" || arg == "--log") && i + 1 < argc) {
            log_file = argv[++i];
        } else if ((arg == "-f" || arg == "--filter") && i + 1 < argc) {
            filter_type = argv[++i];
        }
    }

    // Setup signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "================================================================\n";
    std::cout << "  HFT OBSERVER - Real-time Event Monitor\n";
    std::cout << "================================================================\n";
    std::cout << "  Connecting to shared memory...\n";

    // Open shared memory (consumer mode)
    SharedRingBuffer<TradeEvent>* buffer = nullptr;
    int retries = 0;
    const int max_retries = 10;

    while (!buffer && retries < max_retries && g_running) {
        try {
            buffer = new SharedRingBuffer<TradeEvent>("/hft_events", false);
            std::cout << "  Connected! Buffer capacity: " << buffer->capacity() << " events\n";
        } catch (const std::exception& e) {
            retries++;
            std::cout << "  Waiting for hft engine... (" << retries << "/" << max_retries << ")\n";
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    if (!buffer) {
        std::cerr << "  ERROR: Could not connect to hft engine. Is it running?\n";
        return 1;
    }

    // Open log file if specified
    std::ofstream log_stream;
    if (!log_file.empty()) {
        log_stream.open(log_file, std::ios::app);
        if (!log_stream) {
            std::cerr << "  ERROR: Could not open log file: " << log_file << "\n";
            return 1;
        }
        std::cout << "  Logging to: " << log_file << "\n";
    }

    std::cout << "================================================================\n";
    if (!stats_only) {
        std::cout << "  Press Ctrl+C to stop\n";
        std::cout << "================================================================\n\n";
    }

    // Main loop
    ObserverStats stats;
    TradeEvent event;
    uint64_t last_stats_print = 0;
    auto start_time = std::chrono::steady_clock::now();

    while (g_running) {
        // Try to read events
        bool got_event = false;
        while (buffer->pop(event)) {
            got_event = true;
            stats.record(event);

            // Filter if specified
            if (!filter_type.empty()) {
                std::string type_str = event_type_str(event.type);
                if (type_str.find(filter_type) == std::string::npos) {
                    continue;
                }
            }

            // Print event
            if (!stats_only && !quiet) {
                print_event(event);
            }

            // Log to file
            if (log_stream) {
                log_stream << event.timestamp_ns << ","
                           << event_type_str(event.type) << ","
                           << event.symbol_id << ","
                           << std::string(event.ticker, 3) << ","
                           << event.side << ","
                           << event.price << ","
                           << event.price2 << ","
                           << event.quantity << ","
                           << event.pnl_cents << ","
                           << event.order_id << "\n";
            }
        }

        // Print stats periodically
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();

        if (stats_only && elapsed > last_stats_print + 2) {
            last_stats_print = elapsed;
            // Clear screen and print stats
            std::cout << "\033[2J\033[H";  // ANSI clear screen
            stats.print();
            std::cout << "\n  Running for " << elapsed << " seconds...\n";
            std::cout << "  Buffer: " << buffer->size() << "/" << buffer->capacity()
                      << " (" << buffer->total_produced() << " produced, "
                      << buffer->total_consumed() << " consumed)\n";
        }

        // Sleep if no events (avoid busy spinning)
        if (!got_event) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    // Final stats
    stats.print();

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count();
    std::cout << "\n  Duration: " << duration << " seconds\n";
    std::cout << "  Events/sec: " << (duration > 0 ? stats.total_events / duration : 0) << "\n";

    // Cleanup
    delete buffer;

    return 0;
}
