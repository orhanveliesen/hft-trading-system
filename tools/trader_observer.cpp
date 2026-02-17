/**
 * HFT Observer - Real-time Dashboard for HFT Engine
 *
 * MVC Architecture:
 * - Model: Data objects (StatsModel, PnLModel, EventsModel)
 * - View: Screen regions with position info
 * - Controller: Updates views when models change (dirty flag)
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
#include <vector>
#include <functional>
#include <sys/ioctl.h>
#include <unistd.h>

using namespace hft::ipc;

// ============================================================================
// Terminal Utilities
// ============================================================================

namespace term {
    constexpr const char* RESET      = "\033[0m";
    constexpr const char* BOLD       = "\033[1m";
    constexpr const char* DIM        = "\033[2m";
    constexpr const char* RED        = "\033[31m";
    constexpr const char* GREEN      = "\033[32m";
    constexpr const char* YELLOW     = "\033[33m";
    constexpr const char* CYAN       = "\033[36m";
    constexpr const char* BRED       = "\033[91m";
    constexpr const char* BGREEN     = "\033[92m";
    constexpr const char* BYELLOW    = "\033[93m";
    constexpr const char* BCYAN      = "\033[96m";
    constexpr const char* BWHITE     = "\033[97m";
    constexpr const char* CLEAR      = "\033[2J";
    constexpr const char* HOME       = "\033[H";
    constexpr const char* HIDE_CURSOR = "\033[?25l";
    constexpr const char* SHOW_CURSOR = "\033[?25h";
    constexpr const char* CLEAR_LINE = "\033[K";

    void move_to(int row, int col) {
        std::cout << "\033[" << row << ";" << col << "H";
    }
}

namespace box {
    constexpr const char* TL = "╔";
    constexpr const char* TR = "╗";
    constexpr const char* BL = "╚";
    constexpr const char* BR = "╝";
    constexpr const char* H  = "═";
    constexpr const char* V  = "║";
    constexpr const char* LT = "╠";
    constexpr const char* RT = "╣";
}

// ============================================================================
// MODELS - Pure data, no rendering logic
// ============================================================================

struct StatsModel {
    uint64_t total_events = 0;
    uint64_t total_status = 0;  // Status events count
    int64_t elapsed_seconds = 0;
    double rate = 0.0;
    bool dirty = true;

    void update(uint64_t events, uint64_t status_cnt, int64_t elapsed) {
        if (total_events != events || total_status != status_cnt || elapsed_seconds != elapsed) {
            total_events = events;
            total_status = status_cnt;
            elapsed_seconds = elapsed;
            rate = elapsed > 0 ? (double)events / elapsed : 0.0;
            dirty = true;
        }
    }
};

struct PnLModel {
    double realized_pnl = 0.0;
    double total_profit = 0.0;
    double total_loss = 0.0;
    int winning_trades = 0;
    int losing_trades = 0;
    bool dirty = true;

    double win_rate() const {
        int total = winning_trades + losing_trades;
        return total > 0 ? (double)winning_trades / total * 100 : 0;
    }

    void add_win(double pnl) {
        winning_trades++;
        realized_pnl += pnl;
        total_profit += pnl;
        dirty = true;
    }

    void add_loss(double pnl) {
        losing_trades++;
        realized_pnl += pnl;
        total_loss += std::abs(pnl);
        dirty = true;
    }
};

struct TradeStatsModel {
    uint64_t fills = 0;
    uint64_t targets = 0;
    uint64_t stops = 0;
    uint64_t status_events = 0;  // Status/debug events
    bool dirty = true;

    void add_fill() { fills++; dirty = true; }
    void add_target() { targets++; dirty = true; }
    void add_stop() { stops++; dirty = true; }
    void add_status() { status_events++; dirty = true; }
};

struct EventEntry {
    std::string text;
    std::string color;
};

struct EventsModel {
    std::deque<EventEntry> events;
    static constexpr size_t MAX_EVENTS = 100;
    bool dirty = true;

    void add(const std::string& text, const std::string& color) {
        events.push_front({text, color});
        if (events.size() > MAX_EVENTS) {
            events.pop_back();
        }
        dirty = true;
    }
};

// ============================================================================
// VIEW - Screen region with position, knows how to render itself
// ============================================================================

class View {
protected:
    int start_row_;
    int width_;
    std::vector<std::string> lines_;  // Cached rendered lines

    std::string pad(const std::string& s, int w) const {
        if ((int)s.length() >= w) return s.substr(0, w);
        return s + std::string(w - s.length(), ' ');
    }

    std::string hline(bool is_top = false, bool is_bottom = false) const {
        std::ostringstream ss;
        ss << term::BCYAN;
        if (is_top) ss << box::TL;
        else if (is_bottom) ss << box::BL;
        else ss << box::LT;
        for (int i = 0; i < width_ - 2; i++) ss << box::H;
        if (is_top) ss << box::TR;
        else if (is_bottom) ss << box::BR;
        else ss << box::RT;
        ss << term::RESET;
        return ss.str();
    }

    std::string row(const std::string& content) const {
        std::ostringstream ss;
        ss << term::BCYAN << box::V << term::RESET;
        ss << "  " << pad(content, width_ - 4);
        ss << term::BCYAN << box::V << term::RESET;
        return ss.str();
    }

public:
    View(int start_row, int width) : start_row_(start_row), width_(width) {}
    virtual ~View() = default;

    int start_row() const { return start_row_; }
    int height() const { return lines_.size(); }
    void set_start_row(int r) { start_row_ = r; }

    // Render to screen (only changed lines)
    void render_to_screen(const std::vector<std::string>& prev_lines) {
        std::cout << term::HIDE_CURSOR;
        for (size_t i = 0; i < lines_.size(); i++) {
            bool need_update = (i >= prev_lines.size()) || (lines_[i] != prev_lines[i]);
            if (need_update) {
                term::move_to(start_row_ + i, 1);
                std::cout << lines_[i] << term::CLEAR_LINE;
            }
        }
    }

    const std::vector<std::string>& get_lines() const { return lines_; }
};

// Header View
class HeaderView : public View {
public:
    HeaderView(int row, int width) : View(row, width) {}

    void update() {
        lines_.clear();
        lines_.push_back(hline(true));

        std::ostringstream ss;
        ss << term::BCYAN << box::V << term::RESET;
        ss << term::BOLD << term::BWHITE << "  HFT OBSERVER " << term::RESET;
        ss << term::DIM << "- Real-time Monitor" << term::RESET;
        ss << std::string(std::max(0, width_ - 37), ' ');
        ss << term::BCYAN << box::V << term::RESET;
        lines_.push_back(ss.str());

        lines_.push_back(hline());
    }
};

// Stats View
class StatsView : public View {
    std::vector<std::string> prev_lines_;
public:
    StatsView(int row, int width) : View(row, width) {}

    void update(const StatsModel& model) {
        prev_lines_ = lines_;
        lines_.clear();

        int hours = model.elapsed_seconds / 3600;
        int mins = (model.elapsed_seconds % 3600) / 60;
        int secs = model.elapsed_seconds % 60;

        std::ostringstream ss;
        ss << term::BCYAN << box::V << term::RESET << "  ";
        ss << "Runtime: " << std::setfill('0') << std::setw(2) << hours << ":"
           << std::setw(2) << mins << ":" << std::setw(2) << secs << std::setfill(' ');
        ss << "  |  Events: " << std::setw(8) << model.total_events;
        ss << "  |  Rate: " << std::fixed << std::setprecision(1) << std::setw(8) << model.rate << "/s";
        ss << std::string(std::max(0, width_ - 68), ' ');
        ss << term::BCYAN << box::V << term::RESET;
        lines_.push_back(ss.str());

        lines_.push_back(hline());
    }

    void render_if_dirty(bool dirty) {
        if (dirty) render_to_screen(prev_lines_);
    }
};

// P&L View
class PnLView : public View {
    std::vector<std::string> prev_lines_;
public:
    PnLView(int row, int width) : View(row, width) {}

    void update(const PnLModel& model) {
        prev_lines_ = lines_;
        lines_.clear();

        // Header
        std::ostringstream ss;
        ss << term::BCYAN << box::V << term::RESET;
        ss << term::BOLD << "  P&L SUMMARY" << term::RESET;
        ss << std::string(std::max(0, width_ - 15), ' ');
        ss << term::BCYAN << box::V << term::RESET;
        lines_.push_back(ss.str());

        // P&L value row
        ss.str(""); ss.clear();
        ss << term::BCYAN << box::V << term::RESET << "  ";

        std::ostringstream pnl_ss;
        pnl_ss << std::fixed << std::setprecision(2) << (model.realized_pnl >= 0 ? "+" : "") << "$" << std::abs(model.realized_pnl);

        if (model.realized_pnl >= 0) {
            ss << term::BGREEN << term::BOLD << std::setw(12) << pnl_ss.str() << term::RESET;
        } else {
            ss << term::BRED << term::BOLD << std::setw(12) << pnl_ss.str() << term::RESET;
        }
        ss << "  |  ";
        ss << term::GREEN << "W:" << std::setw(4) << model.winning_trades << term::RESET << " ";
        ss << term::RED << "L:" << std::setw(4) << model.losing_trades << term::RESET;
        ss << "  |  WinRate: " << std::fixed << std::setprecision(0) << std::setw(3) << model.win_rate() << "%";
        ss << std::string(std::max(0, width_ - 60), ' ');
        ss << term::BCYAN << box::V << term::RESET;
        lines_.push_back(ss.str());

        // Profit/Loss breakdown
        ss.str(""); ss.clear();
        ss << term::BCYAN << box::V << term::RESET << "  ";
        ss << term::GREEN << "Profit: +$" << std::fixed << std::setprecision(2) << std::setw(10) << model.total_profit << term::RESET << "  ";
        ss << term::RED << "Loss: -$" << std::fixed << std::setprecision(2) << std::setw(10) << model.total_loss << term::RESET;
        ss << std::string(std::max(0, width_ - 52), ' ');
        ss << term::BCYAN << box::V << term::RESET;
        lines_.push_back(ss.str());

        lines_.push_back(hline());
    }

    void render_if_dirty(bool dirty) {
        if (dirty) render_to_screen(prev_lines_);
    }
};

// Trade Stats View
class TradeStatsView : public View {
    std::vector<std::string> prev_lines_;
public:
    TradeStatsView(int row, int width) : View(row, width) {}

    void update(const TradeStatsModel& model) {
        prev_lines_ = lines_;
        lines_.clear();

        std::ostringstream ss;
        ss << term::BCYAN << box::V << term::RESET << "  ";
        ss << term::BGREEN << "Fills: " << std::setw(5) << model.fills << term::RESET << "  |  ";
        ss << term::GREEN << "Targets: " << std::setw(5) << model.targets << term::RESET << "  |  ";
        ss << term::RED << "Stops: " << std::setw(5) << model.stops << term::RESET << "  |  ";
        ss << term::CYAN << "Status: " << std::setw(5) << model.status_events << term::RESET;
        ss << std::string(std::max(0, width_ - 72), ' ');
        ss << term::BCYAN << box::V << term::RESET;
        lines_.push_back(ss.str());

        lines_.push_back(hline());
    }

    void render_if_dirty(bool dirty) {
        if (dirty) render_to_screen(prev_lines_);
    }
};

// Events View
class EventsView : public View {
    std::vector<std::string> prev_lines_;
    int visible_rows_;
public:
    EventsView(int row, int width, int visible_rows)
        : View(row, width), visible_rows_(visible_rows) {}

    void set_visible_rows(int rows) { visible_rows_ = rows; }

    void update(const EventsModel& model) {
        prev_lines_ = lines_;
        lines_.clear();

        // Header
        std::ostringstream ss;
        ss << term::BCYAN << box::V << term::RESET;
        ss << term::BOLD << "  LIVE EVENTS (" << visible_rows_ << " rows)" << term::RESET;
        ss << std::string(std::max(0, width_ - 24), ' ');
        ss << term::BCYAN << box::V << term::RESET;
        lines_.push_back(ss.str());

        // Events
        int displayed = 0;
        for (const auto& ev : model.events) {
            if (displayed >= visible_rows_) break;

            ss.str(""); ss.clear();
            ss << term::BCYAN << box::V << term::RESET;
            ss << ev.color << "  " << pad(ev.text, width_ - 4) << term::RESET;
            ss << term::BCYAN << box::V << term::RESET;
            lines_.push_back(ss.str());
            displayed++;
        }

        // Fill empty rows
        while (displayed < visible_rows_) {
            ss.str(""); ss.clear();
            ss << term::BCYAN << box::V << term::RESET;
            ss << std::string(width_ - 2, ' ');
            ss << term::BCYAN << box::V << term::RESET;
            lines_.push_back(ss.str());
            displayed++;
        }
    }

    void render_if_dirty(bool dirty) {
        if (dirty) render_to_screen(prev_lines_);
    }
};

// Footer View
class FooterView : public View {
public:
    FooterView(int row, int width) : View(row, width) {}

    void update(int term_width, int term_height) {
        lines_.clear();
        lines_.push_back(hline(false, true));

        std::ostringstream ss;
        ss << term::DIM << "  Press Ctrl+C to exit  |  Terminal: " << term_width << "x" << term_height << term::RESET;
        lines_.push_back(ss.str());
    }
};

// ============================================================================
// CONTROLLER - Connects models to views, manages updates
// ============================================================================

class DashboardController {
    // Models
    StatsModel stats_;
    PnLModel pnl_;
    TradeStatsModel trade_stats_;
    EventsModel events_;

    // Views
    HeaderView header_view_;
    StatsView stats_view_;
    PnLView pnl_view_;
    TradeStatsView trade_stats_view_;
    EventsView events_view_;
    FooterView footer_view_;

    // State
    int term_width_ = 80;
    int term_height_ = 24;
    std::chrono::steady_clock::time_point start_time_;
    uint64_t first_event_ts_ = 0;
    bool first_render_ = true;

    void update_term_size() {
        struct winsize w;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
            term_width_ = std::max(60, (int)w.ws_col);
            term_height_ = std::max(20, (int)w.ws_row);
        }
    }

    void layout_views() {
        // Calculate positions
        // Header: 3 rows, Stats: 2 rows, PnL: 4 rows, TradeStats: 2 rows, Footer: 2 rows = 13 fixed
        int event_rows = std::max(5, term_height_ - 14);
        events_view_.set_visible_rows(event_rows);

        int row = 1;
        header_view_.set_start_row(row); row += 3;
        stats_view_.set_start_row(row); row += 2;
        pnl_view_.set_start_row(row); row += 4;
        trade_stats_view_.set_start_row(row); row += 2;
        events_view_.set_start_row(row); row += event_rows + 1;
        footer_view_.set_start_row(row);
    }

public:
    DashboardController()
        : header_view_(1, 80)
        , stats_view_(4, 80)
        , pnl_view_(6, 80)
        , trade_stats_view_(10, 80)
        , events_view_(12, 80, 10)
        , footer_view_(23, 80)
        , start_time_(std::chrono::steady_clock::now())
    {}

    void process_event(const TradeEvent& e) {
        if (first_event_ts_ == 0) first_event_ts_ = e.timestamp_ns;

        double rel_sec = (e.timestamp_ns - first_event_ts_) / 1e9;
        std::ostringstream ss;

        switch (e.type) {
            case EventType::Fill: {
                trade_stats_.add_fill();
                std::string ticker(e.ticker, 3);
                ss << std::fixed << std::setprecision(1) << std::setw(6) << rel_sec << "s  "
                   << (e.side == 0 ? "BUY  " : "SELL ")
                   << std::setw(4) << ticker << "  "
                   << std::setw(5) << e.quantity << " @ $"
                   << std::setprecision(2) << e.price;
                events_.add(ss.str(), (e.side == 0) ? term::BGREEN : term::BYELLOW);
                break;
            }
            case EventType::TargetHit: {
                trade_stats_.add_target();
                pnl_.add_win(e.pnl);

                std::string ticker(e.ticker, 3);
                ss << std::fixed << std::setprecision(1) << std::setw(6) << rel_sec << "s  "
                   << "TARGET " << std::setw(4) << ticker << "  +"
                   << std::setprecision(2) << "$" << e.pnl;
                events_.add(ss.str(), term::BGREEN);
                break;
            }
            case EventType::StopLoss: {
                trade_stats_.add_stop();
                pnl_.add_loss(e.pnl);

                std::string ticker(e.ticker, 3);
                ss << std::fixed << std::setprecision(1) << std::setw(6) << rel_sec << "s  "
                   << "STOP   " << std::setw(4) << ticker << "  "
                   << std::setprecision(2) << "$" << e.pnl;
                events_.add(ss.str(), term::BRED);
                break;
            }
            case EventType::Signal: {
                std::string ticker(e.ticker, 3);
                ss << std::fixed << std::setprecision(1) << std::setw(6) << rel_sec << "s  "
                   << "SIGNAL " << std::setw(4) << ticker << "  "
                   << (e.side == 0 ? "BUY" : "SELL");
                events_.add(ss.str(), term::BCYAN);
                break;
            }
            case EventType::Status: {
                trade_stats_.add_status();
                std::string ticker(e.ticker, 4);
                const char* code_name = TradeEvent::status_code_name(e.get_status_code());

                // Format status message based on code
                ss << std::fixed << std::setprecision(1) << std::setw(6) << rel_sec << "s  "
                   << std::setw(10) << code_name << "  " << std::setw(4) << ticker;

                // Add price if available
                if (e.price > 0) {
                    ss << "  $" << std::setprecision(2) << e.price;
                }

                // Add signal strength if relevant
                if (e.signal_strength > 0) {
                    ss << "  Str:" << (int)e.signal_strength;
                }

                // Color based on status type
                const char* color = term::DIM;
                StatusCode sc = e.get_status_code();
                if (sc == StatusCode::Heartbeat) {
                    color = term::DIM;  // Dim for routine heartbeat
                } else if (sc == StatusCode::AutoTuneRelaxed) {
                    color = term::BGREEN;  // Green - good news (relaxed params)
                } else if (sc == StatusCode::IndicatorsWarmup ||
                           sc == StatusCode::AutoTuneCooldown ||
                           sc == StatusCode::AutoTuneSignal ||
                           sc == StatusCode::AutoTuneMinTrade) {
                    color = term::YELLOW;  // Yellow - warning/adjustment
                } else if (sc == StatusCode::CashLow ||
                           sc == StatusCode::TradingDisabled ||
                           sc == StatusCode::AutoTunePaused ||
                           sc == StatusCode::VolatilitySpike ||
                           sc == StatusCode::DrawdownAlert) {
                    color = term::BRED;  // Red - alert!
                }

                events_.add(ss.str(), color);
                break;
            }
            default:
                return;
        }
    }

    void render() {
        update_term_size();

        // Update stats model
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
        stats_.update(
            trade_stats_.fills + trade_stats_.targets + trade_stats_.stops,
            trade_stats_.status_events,
            elapsed
        );

        // First render - full screen clear and draw
        if (first_render_) {
            std::cout << term::CLEAR << term::HOME << term::HIDE_CURSOR;

            // Set widths
            header_view_ = HeaderView(1, term_width_);
            stats_view_ = StatsView(4, term_width_);
            pnl_view_ = PnLView(6, term_width_);
            trade_stats_view_ = TradeStatsView(10, term_width_);
            events_view_ = EventsView(12, term_width_, 10);
            footer_view_ = FooterView(23, term_width_);

            layout_views();

            // Update all views
            header_view_.update();
            stats_view_.update(stats_);
            pnl_view_.update(pnl_);
            trade_stats_view_.update(trade_stats_);
            events_view_.update(events_);
            footer_view_.update(term_width_, term_height_);

            // Render all
            for (const auto& line : header_view_.get_lines()) {
                std::cout << line << term::CLEAR_LINE << "\n";
            }
            for (const auto& line : stats_view_.get_lines()) {
                std::cout << line << term::CLEAR_LINE << "\n";
            }
            for (const auto& line : pnl_view_.get_lines()) {
                std::cout << line << term::CLEAR_LINE << "\n";
            }
            for (const auto& line : trade_stats_view_.get_lines()) {
                std::cout << line << term::CLEAR_LINE << "\n";
            }
            for (const auto& line : events_view_.get_lines()) {
                std::cout << line << term::CLEAR_LINE << "\n";
            }
            for (const auto& line : footer_view_.get_lines()) {
                std::cout << line << term::CLEAR_LINE << "\n";
            }

            // Clear dirty flags
            stats_.dirty = false;
            pnl_.dirty = false;
            trade_stats_.dirty = false;
            events_.dirty = false;
            first_render_ = false;
        } else {
            // Differential update - only dirty views
            layout_views();

            if (stats_.dirty) {
                stats_view_.update(stats_);
                stats_view_.render_if_dirty(true);
                stats_.dirty = false;
            }

            if (pnl_.dirty) {
                pnl_view_.update(pnl_);
                pnl_view_.render_if_dirty(true);
                pnl_.dirty = false;
            }

            if (trade_stats_.dirty) {
                trade_stats_view_.update(trade_stats_);
                trade_stats_view_.render_if_dirty(true);
                trade_stats_.dirty = false;
            }

            if (events_.dirty) {
                events_view_.update(events_);
                events_view_.render_if_dirty(true);
                events_.dirty = false;
            }
        }

        std::cout << std::flush;
    }

    void cleanup() {
        std::cout << term::SHOW_CURSOR << term::RESET;
    }

    // Getters for final summary
    uint64_t total_events() const { return trade_stats_.fills + trade_stats_.targets + trade_stats_.stops + trade_stats_.status_events; }
    uint64_t total_status() const { return trade_stats_.status_events; }
    double realized_pnl() const { return pnl_.realized_pnl; }
    int wins() const { return pnl_.winning_trades; }
    int losses() const { return pnl_.losing_trades; }
};

// ============================================================================
// Global State
// ============================================================================

std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running = false;
}

// ============================================================================
// Main
// ============================================================================

void print_help() {
    std::cout << "Usage: hft_observer [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help       Show this help\n";
    std::cout << "  -s, --stream     Stream mode (no dashboard)\n";
    std::cout << "  -l, --log FILE   Log events to CSV file\n";
}

int main(int argc, char* argv[]) {
    bool stream_mode = false;
    std::string log_file;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_help();
            return 0;
        } else if (arg == "-s" || arg == "--stream") {
            stream_mode = true;
        } else if ((arg == "-l" || arg == "--log") && i + 1 < argc) {
            log_file = argv[++i];
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
        std::cout << "HFT Observer - Stream Mode\nConnecting...\n";
    }

    // Connect to shared memory
    SharedRingBuffer<TradeEvent>* buffer = nullptr;
    int retries = 0;

    while (!buffer && retries < 30 && g_running) {
        try {
            buffer = new SharedRingBuffer<TradeEvent>("/trader_events", false);
            std::cout << term::BGREEN << "Connected!" << term::RESET << "\n";
        } catch (...) {
            retries++;
            std::cout << term::YELLOW << "  Waiting... (" << retries << "/30)\r" << term::RESET << std::flush;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    if (!buffer) {
        std::cerr << term::RED << "ERROR: Could not connect.\n" << term::RESET;
        return 1;
    }

    // Open log file
    std::ofstream log_stream;
    if (!log_file.empty()) {
        log_stream.open(log_file, std::ios::app);
        log_stream << "timestamp,type,symbol,side,price,quantity,pnl\n";
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Main loop
    DashboardController controller;
    TradeEvent event;
    auto last_render = std::chrono::steady_clock::now();

    while (g_running) {
        bool got_event = false;

        while (buffer->pop(event)) {
            got_event = true;
            controller.process_event(event);

            // Stream mode
            if (stream_mode) {
                const char* type_str = "";
                switch (event.type) {
                    case EventType::Fill: type_str = "FILL"; break;
                    case EventType::TargetHit: type_str = "TARGET"; break;
                    case EventType::StopLoss: type_str = "STOP"; break;
                    case EventType::Status: {
                        // Show status with code name
                        std::cout << "STATUS " << std::string(event.ticker, 4) << " "
                                  << TradeEvent::status_code_name(event.get_status_code())
                                  << " $" << event.price << "\n";
                        continue;
                    }
                    default: continue;
                }
                std::cout << type_str << " " << std::string(event.ticker, 3)
                          << " " << event.price << "\n";
            }

            // Log
            if (log_stream) {
                log_stream << event.timestamp_ns << ","
                           << (int)event.type << ","
                           << std::string(event.ticker, 3) << ","
                           << (int)event.side << ","
                           << event.price << ","
                           << event.quantity << ","
                           << event.pnl << "\n";
            }
        }

        // Dashboard mode: render at fixed interval
        if (!stream_mode) {
            auto now = std::chrono::steady_clock::now();
            auto since_render = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_render).count();

            if (since_render >= 100) {  // 10 FPS
                controller.render();
                last_render = now;
            }
        }

        if (!got_event) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // Cleanup
    controller.cleanup();

    // Final summary
    std::cout << "\n" << term::BOLD << "Final Summary:" << term::RESET << "\n";
    std::cout << "  Events: " << controller.total_events() << " (Status: " << controller.total_status() << ")\n";
    std::cout << "  P&L: ";
    if (controller.realized_pnl() >= 0) {
        std::cout << term::GREEN << "+$" << std::fixed << std::setprecision(2) << controller.realized_pnl();
    } else {
        std::cout << term::RED << "-$" << std::fixed << std::setprecision(2) << std::abs(controller.realized_pnl());
    }
    std::cout << term::RESET << "\n";
    std::cout << "  Win Rate: " << controller.wins() << "W / " << controller.losses() << "L\n";

    delete buffer;
    return 0;
}
