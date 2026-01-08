/**
 * HFT Observer - Real-time Dashboard for HFT Engine
 *
 * Beautiful TUI dashboard showing:
 * - Live event stream
 * - P&L tracking
 * - Position summary
 * - Statistics
 *
 * Usage:
 *   hft_observer              # Dashboard mode (default)
 *   hft_observer --stream     # Event stream only
 *   hft_observer --log FILE   # Log to file
 */

#include "../include/ipc/trade_event.hpp"
#include "../include/ipc/shared_ring_buffer.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <cmath>

using namespace hft::ipc;

// ============================================================================
// ANSI Terminal Colors & Control
// ============================================================================

namespace term {
    // Colors
    constexpr const char* RESET     = "\033[0m";
    constexpr const char* BOLD      = "\033[1m";
    constexpr const char* DIM       = "\033[2m";

    // Foreground
    constexpr const char* RED       = "\033[31m";
    constexpr const char* GREEN     = "\033[32m";
    constexpr const char* YELLOW    = "\033[33m";
    constexpr const char* BLUE      = "\033[34m";
    constexpr const char* MAGENTA   = "\033[35m";
    constexpr const char* CYAN      = "\033[36m";
    constexpr const char* WHITE     = "\033[37m";

    // Bright foreground
    constexpr const char* BRED      = "\033[91m";
    constexpr const char* BGREEN    = "\033[92m";
    constexpr const char* BYELLOW   = "\033[93m";
    constexpr const char* BBLUE     = "\033[94m";
    constexpr const char* BCYAN     = "\033[96m";
    constexpr const char* BWHITE    = "\033[97m";

    // Background
    constexpr const char* BG_BLACK  = "\033[40m";
    constexpr const char* BG_RED    = "\033[41m";
    constexpr const char* BG_GREEN  = "\033[42m";
    constexpr const char* BG_BLUE   = "\033[44m";

    // Control
    constexpr const char* CLEAR     = "\033[2J";
    constexpr const char* HOME      = "\033[H";
    constexpr const char* HIDE_CURSOR = "\033[?25l";
    constexpr const char* SHOW_CURSOR = "\033[?25h";

    void clear_screen() { std::cout << CLEAR << HOME; }
    void move_to(int row, int col) { std::cout << "\033[" << row << ";" << col << "H"; }
}

// ============================================================================
// Box Drawing Characters (Unicode)
// ============================================================================

namespace box {
    constexpr const char* TL = "╔";  // Top left
    constexpr const char* TR = "╗";  // Top right
    constexpr const char* BL = "╚";  // Bottom left
    constexpr const char* BR = "╝";  // Bottom right
    constexpr const char* H  = "═";  // Horizontal
    constexpr const char* V  = "║";  // Vertical
    constexpr const char* LT = "╠";  // Left T
    constexpr const char* RT = "╣";  // Right T
    constexpr const char* TT = "╦";  // Top T
    constexpr const char* BT = "╩";  // Bottom T
    constexpr const char* X  = "╬";  // Cross

    // Single line
    constexpr const char* HL = "─";  // Horizontal light
    constexpr const char* VL = "│";  // Vertical light
}

// ============================================================================
// Global State
// ============================================================================

std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running = false;
}

// ============================================================================
// Event Display Entry
// ============================================================================

struct DisplayEvent {
    uint64_t timestamp;
    std::string text;
    std::string color;
};

// ============================================================================
// Dashboard State
// ============================================================================

class Dashboard {
public:
    static constexpr int WIDTH = 80;
    static constexpr int INNER = WIDTH - 4;  // Content width (excluding borders and padding)
    static constexpr int EVENT_PANEL_HEIGHT = 15;
    static constexpr int MAX_EVENTS = 50;

    // Helper: pad string to exact width
    static std::string pad(const std::string& s, int width) {
        if ((int)s.length() >= width) return s.substr(0, width);
        return s + std::string(width - s.length(), ' ');
    }

    // Helper: create horizontal line
    static void hline(bool is_middle = true) {
        std::cout << term::BCYAN;
        std::cout << (is_middle ? box::LT : box::TL);
        for (int i = 0; i < WIDTH - 2; i++) std::cout << box::H;
        std::cout << (is_middle ? box::RT : box::TR);
        std::cout << term::RESET << "\n";
    }

    // Helper: print row with borders
    static void row(const std::string& content) {
        std::cout << term::BCYAN << box::V << term::RESET;
        std::cout << "  " << pad(content, INNER);
        std::cout << term::BCYAN << box::V << term::RESET << "\n";
    }

    // Statistics
    uint64_t total_events = 0;
    uint64_t fills = 0;
    uint64_t targets = 0;
    uint64_t stops = 0;

    // P&L
    double realized_pnl = 0;
    double total_profit = 0;
    double total_loss = 0;
    int winning_trades = 0;
    int losing_trades = 0;

    // Positions (symbol -> quantity, value)
    std::map<std::string, std::pair<double, double>> positions;

    // Recent events
    std::deque<DisplayEvent> recent_events;

    // Timing
    std::chrono::steady_clock::time_point start_time;
    uint64_t first_event_ts = 0;

    Dashboard() : start_time(std::chrono::steady_clock::now()) {}

    void add_event(const TradeEvent& e) {
        total_events++;

        if (first_event_ts == 0) first_event_ts = e.timestamp_ns;

        DisplayEvent de;
        de.timestamp = e.timestamp_ns;

        std::ostringstream ss;
        double rel_sec = (e.timestamp_ns - first_event_ts) / 1e9;

        switch (e.type) {
            case EventType::Fill: {
                fills++;
                std::string ticker(e.ticker, 3);
                std::string side = e.side == 0 ? "BUY " : "SELL";
                ss << std::fixed << std::setprecision(1) << std::setw(6) << rel_sec << "s  "
                   << (e.side == 0 ? "BUY  " : "SELL ")
                   << std::setw(4) << ticker << "  "
                   << std::setw(5) << e.quantity << " @ $"
                   << std::setprecision(2) << e.price;
                de.color = (e.side == 0) ? term::BGREEN : term::BYELLOW;

                // Update position tracking
                if (e.side == 0) {  // Buy
                    positions[ticker].first += e.quantity;
                    positions[ticker].second += e.quantity * e.price;
                }
                break;
            }

            case EventType::TargetHit: {
                targets++;
                winning_trades++;
                double pnl = e.pnl_cents / 100.0;
                realized_pnl += pnl;
                total_profit += pnl;

                std::string ticker(e.ticker, 3);
                ss << std::fixed << std::setprecision(1) << std::setw(6) << rel_sec << "s  "
                   << "TARGET " << std::setw(4) << ticker << "  +"
                   << std::setprecision(2) << "$" << pnl
                   << "  (entry:$" << e.price2 << " exit:$" << e.price << ")";
                de.color = term::BGREEN;

                // Update position
                positions[ticker].first -= e.quantity;
                positions[ticker].second -= e.quantity * e.price2;
                break;
            }

            case EventType::StopLoss: {
                stops++;
                losing_trades++;
                double pnl = e.pnl_cents / 100.0;
                realized_pnl += pnl;
                total_loss += std::abs(pnl);

                std::string ticker(e.ticker, 3);
                ss << std::fixed << std::setprecision(1) << std::setw(6) << rel_sec << "s  "
                   << "STOP   " << std::setw(4) << ticker << "  "
                   << std::setprecision(2) << "$" << pnl
                   << "  (entry:$" << e.price2 << " exit:$" << e.price << ")";
                de.color = term::BRED;

                // Update position
                positions[ticker].first -= e.quantity;
                positions[ticker].second -= e.quantity * e.price2;
                break;
            }

            case EventType::Signal: {
                std::string ticker(e.ticker, 3);
                ss << std::fixed << std::setprecision(1) << std::setw(6) << rel_sec << "s  "
                   << "SIGNAL " << std::setw(4) << ticker << "  "
                   << (e.side == 0 ? "BUY" : "SELL")
                   << " strength:" << (int)e.signal_strength;
                de.color = term::BCYAN;
                break;
            }

            default:
                return;  // Don't display other events
        }

        de.text = ss.str();
        recent_events.push_front(de);

        if (recent_events.size() > MAX_EVENTS) {
            recent_events.pop_back();
        }
    }

    void render() {
        std::cout << term::CLEAR << term::HOME << term::HIDE_CURSOR;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        int hours = elapsed / 3600;
        int mins = (elapsed % 3600) / 60;
        int secs = elapsed % 60;

        // Use fixed-width formatting with ostringstream
        std::ostringstream ss;

        // ═══════════════════════════════════════════════════════════════════
        // Header
        // ═══════════════════════════════════════════════════════════════════
        hline(false);  // Top border

        std::cout << term::BCYAN << box::V << term::RESET;
        std::cout << term::BOLD << term::BWHITE << "  HFT OBSERVER " << term::RESET << term::DIM << "- Real-time Monitor" << term::RESET;
        std::cout << std::string(INNER - 33, ' ');
        std::cout << term::BCYAN << box::V << term::RESET << "\n";

        hline();

        // ═══════════════════════════════════════════════════════════════════
        // Stats Row - fixed width fields
        // ═══════════════════════════════════════════════════════════════════
        ss.str(""); ss.clear();
        ss << "Runtime: " << std::setfill('0') << std::setw(2) << hours << ":"
           << std::setw(2) << mins << ":" << std::setw(2) << secs << std::setfill(' ')
           << "  |  Events: " << std::setw(6) << total_events
           << "  |  Rate: " << std::fixed << std::setprecision(1) << std::setw(6)
           << (elapsed > 0 ? (double)total_events / elapsed : 0.0) << "/s";
        row(ss.str());

        hline();

        // ═══════════════════════════════════════════════════════════════════
        // P&L Section - fixed width
        // ═══════════════════════════════════════════════════════════════════
        std::cout << term::BCYAN << box::V << term::RESET;
        std::cout << term::BOLD << "  P&L SUMMARY" << term::RESET;
        std::cout << std::string(INNER - 11, ' ');
        std::cout << term::BCYAN << box::V << term::RESET << "\n";

        // P&L row with colors - build plain string first, then add colors
        int total_trades = winning_trades + losing_trades;
        double win_rate = total_trades > 0 ? (double)winning_trades / total_trades * 100 : 0;

        std::cout << term::BCYAN << box::V << term::RESET << "  ";
        // P&L value (10 chars)
        ss.str(""); ss.clear();
        ss << std::fixed << std::setprecision(2) << (realized_pnl >= 0 ? "+" : "") << "$" << std::abs(realized_pnl);
        std::string pnl_str = ss.str();
        if (realized_pnl >= 0) {
            std::cout << term::BGREEN << term::BOLD << std::setw(10) << pnl_str << term::RESET;
        } else {
            std::cout << term::BRED << term::BOLD << std::setw(10) << pnl_str << term::RESET;
        }
        std::cout << "  |  ";
        std::cout << term::GREEN << "W:" << std::setw(3) << winning_trades << term::RESET << " ";
        std::cout << term::RED << "L:" << std::setw(3) << losing_trades << term::RESET;
        std::cout << "  |  WinRate: " << std::fixed << std::setprecision(0) << std::setw(3) << win_rate << "%";
        std::cout << std::string(INNER - 52, ' ');
        std::cout << term::BCYAN << box::V << term::RESET << "\n";

        // Profit/Loss breakdown
        std::cout << term::BCYAN << box::V << term::RESET << "  ";
        ss.str(""); ss.clear();
        ss << "Profit: +$" << std::fixed << std::setprecision(2) << std::setw(10) << total_profit;
        std::cout << term::GREEN << ss.str() << term::RESET << "  ";
        ss.str(""); ss.clear();
        ss << "Loss: -$" << std::fixed << std::setprecision(2) << std::setw(10) << total_loss;
        std::cout << term::RED << ss.str() << term::RESET;
        std::cout << std::string(INNER - 48, ' ');
        std::cout << term::BCYAN << box::V << term::RESET << "\n";

        hline();

        // ═══════════════════════════════════════════════════════════════════
        // Trade Stats - fixed width
        // ═══════════════════════════════════════════════════════════════════
        std::cout << term::BCYAN << box::V << term::RESET << "  ";
        std::cout << term::BGREEN << "Fills: " << std::setw(5) << fills << term::RESET << "  |  ";
        std::cout << term::GREEN << "Targets: " << std::setw(5) << targets << term::RESET << "  |  ";
        std::cout << term::RED << "Stops: " << std::setw(5) << stops << term::RESET;
        std::cout << std::string(INNER - 50, ' ');
        std::cout << term::BCYAN << box::V << term::RESET << "\n";

        hline();

        // ═══════════════════════════════════════════════════════════════════
        // Event Stream Header
        // ═══════════════════════════════════════════════════════════════════
        std::cout << term::BCYAN << box::V << term::RESET;
        std::cout << term::BOLD << "  LIVE EVENTS" << term::RESET;
        std::cout << std::string(INNER - 11, ' ');
        std::cout << term::BCYAN << box::V << term::RESET << "\n";

        // Events - fixed width per event
        int displayed = 0;
        for (const auto& ev : recent_events) {
            if (displayed >= EVENT_PANEL_HEIGHT) break;

            std::cout << term::BCYAN << box::V << term::RESET;
            std::cout << ev.color << "  " << pad(ev.text, INNER) << term::RESET;
            std::cout << term::BCYAN << box::V << term::RESET << "\n";
            displayed++;
        }

        // Fill empty rows
        while (displayed < EVENT_PANEL_HEIGHT) {
            std::cout << term::BCYAN << box::V << term::RESET;
            std::cout << std::string(INNER + 2, ' ');
            std::cout << term::BCYAN << box::V << term::RESET << "\n";
            displayed++;
        }

        // ═══════════════════════════════════════════════════════════════════
        // Footer
        // ═══════════════════════════════════════════════════════════════════
        std::cout << term::BCYAN << box::BL;
        for (int i = 0; i < WIDTH - 2; i++) std::cout << box::H;
        std::cout << box::BR << term::RESET << "\n";

        std::cout << term::DIM << "  Press Ctrl+C to exit" << term::RESET << "\n";

        std::cout << std::flush;
    }

    void cleanup() {
        std::cout << term::SHOW_CURSOR << term::RESET;
    }
};

// ============================================================================
// Main
// ============================================================================

void print_help() {
    std::cout << "Usage: hft_observer [options]\n";
    std::cout << "\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help       Show this help\n";
    std::cout << "  -s, --stream     Stream mode (no dashboard, just events)\n";
    std::cout << "  -l, --log FILE   Log events to CSV file\n";
    std::cout << "  -f, --filter T   Filter by event type (FILL, TARGET, STOP)\n";
    std::cout << "\n";
}

int main(int argc, char* argv[]) {
    bool stream_mode = false;
    std::string log_file;
    std::string filter_type;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_help();
            return 0;
        } else if (arg == "-s" || arg == "--stream") {
            stream_mode = true;
        } else if ((arg == "-l" || arg == "--log") && i + 1 < argc) {
            log_file = argv[++i];
        } else if ((arg == "-f" || arg == "--filter") && i + 1 < argc) {
            filter_type = argv[++i];
            stream_mode = true;  // Filter implies stream mode
        }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    if (!stream_mode) {
        std::cout << term::CLEAR << term::HOME;
        std::cout << term::BOLD << term::BCYAN;
        std::cout << "╔══════════════════════════════════════════╗\n";
        std::cout << "║     HFT OBSERVER - Connecting...         ║\n";
        std::cout << "╚══════════════════════════════════════════╝\n";
        std::cout << term::RESET;
    } else {
        std::cout << "HFT Observer - Stream Mode\n";
        std::cout << "Connecting to shared memory...\n";
    }

    // Connect to shared memory
    SharedRingBuffer<TradeEvent>* buffer = nullptr;
    int retries = 0;
    const int max_retries = 30;

    while (!buffer && retries < max_retries && g_running) {
        try {
            buffer = new SharedRingBuffer<TradeEvent>("/hft_events", false);
            if (!stream_mode) {
                std::cout << term::BGREEN << "Connected!" << term::RESET << "\n";
            } else {
                std::cout << "Connected! Buffer: " << buffer->capacity() << " events\n";
            }
        } catch (...) {
            retries++;
            if (!stream_mode) {
                std::cout << term::YELLOW << "  Waiting for HFT engine... ("
                          << retries << "/" << max_retries << ")\r" << term::RESET << std::flush;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    if (!buffer) {
        std::cerr << term::RED << "ERROR: Could not connect. Is HFT engine running?\n" << term::RESET;
        return 1;
    }

    // Open log file
    std::ofstream log_stream;
    if (!log_file.empty()) {
        log_stream.open(log_file, std::ios::app);
        if (!log_stream) {
            std::cerr << "ERROR: Could not open log file: " << log_file << "\n";
            return 1;
        }
        // CSV header
        log_stream << "timestamp,type,symbol,side,price,price2,quantity,pnl,order_id\n";
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Main loop
    Dashboard dashboard;
    TradeEvent event;
    auto last_render = std::chrono::steady_clock::now();

    while (g_running) {
        bool got_event = false;

        while (buffer->pop(event)) {
            got_event = true;
            dashboard.add_event(event);

            // Log to file
            if (log_stream) {
                const char* type_str = "";
                switch (event.type) {
                    case EventType::Fill: type_str = "FILL"; break;
                    case EventType::TargetHit: type_str = "TARGET"; break;
                    case EventType::StopLoss: type_str = "STOP"; break;
                    case EventType::Signal: type_str = "SIGNAL"; break;
                    default: type_str = "OTHER"; break;
                }

                log_stream << event.timestamp_ns << ","
                           << type_str << ","
                           << std::string(event.ticker, 3) << ","
                           << (int)event.side << ","
                           << event.price << ","
                           << event.price2 << ","
                           << event.quantity << ","
                           << event.pnl_cents << ","
                           << event.order_id << "\n";
            }

            // Stream mode: print immediately
            if (stream_mode && !dashboard.recent_events.empty()) {
                const auto& ev = dashboard.recent_events.front();

                // Filter check
                if (!filter_type.empty()) {
                    if (ev.text.find(filter_type) == std::string::npos) continue;
                }

                std::cout << ev.color << ev.text << term::RESET << "\n";
            }
        }

        // Dashboard mode: render periodically
        if (!stream_mode) {
            auto now = std::chrono::steady_clock::now();
            auto since_render = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_render).count();

            if (since_render >= 10 || got_event) {  // 100 FPS max refresh
                dashboard.render();
                last_render = now;
            }
        }

        if (!got_event) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // Cleanup
    dashboard.cleanup();

    // Final summary
    std::cout << "\n" << term::BOLD << "Final Summary:" << term::RESET << "\n";
    std::cout << "  Events: " << dashboard.total_events << "\n";
    std::cout << "  P&L: ";
    if (dashboard.realized_pnl >= 0) {
        std::cout << term::GREEN << "+$" << std::fixed << std::setprecision(2) << dashboard.realized_pnl;
    } else {
        std::cout << term::RED << "-$" << std::fixed << std::setprecision(2) << std::abs(dashboard.realized_pnl);
    }
    std::cout << term::RESET << "\n";
    std::cout << "  Win Rate: " << dashboard.winning_trades << "W / " << dashboard.losing_trades << "L\n";

    delete buffer;
    return 0;
}
