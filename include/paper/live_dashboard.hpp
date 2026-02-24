#pragma once

#include "../strategy/regime_detector.hpp"
#include "paper_trading_engine.hpp"

#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace hft {
namespace paper {

/**
 * Terminal color codes
 */
namespace color {
constexpr const char* RESET = "\033[0m";
constexpr const char* RED = "\033[31m";
constexpr const char* GREEN = "\033[32m";
constexpr const char* YELLOW = "\033[33m";
constexpr const char* BLUE = "\033[34m";
constexpr const char* MAGENTA = "\033[35m";
constexpr const char* CYAN = "\033[36m";
constexpr const char* WHITE = "\033[37m";
constexpr const char* BOLD = "\033[1m";
constexpr const char* DIM = "\033[2m";
} // namespace color

/**
 * Dashboard Configuration
 */
struct DashboardConfig {
    uint64_t refresh_interval_ms = 100; // Update every 100ms
    bool use_colors = true;
    bool show_regime = true;
    bool show_positions = true;
    bool show_orders = true;
    bool show_pnl = true;
    bool show_latency = true;
    bool clear_screen = true;
};

/**
 * Live Dashboard
 *
 * Displays real-time trading information with minimal overhead.
 * Updates at fixed intervals rather than per-tick.
 *
 * Usage:
 *   LiveDashboard dashboard(engine);
 *   // In your event loop:
 *   dashboard.update();  // Only refreshes if interval elapsed
 */
class LiveDashboard {
public:
    explicit LiveDashboard(PaperTradingEngine& engine, const DashboardConfig& config = {})
        : engine_(engine), config_(config), last_update_ms_(0), frame_count_(0) {}

    /**
     * Update dashboard (respects refresh interval)
     */
    void update() {
        auto now = std::chrono::steady_clock::now();
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

        if (now_ms - last_update_ms_ < static_cast<int64_t>(config_.refresh_interval_ms)) {
            return; // Too soon
        }

        last_update_ms_ = now_ms;
        frame_count_++;

        render();
    }

    /**
     * Force immediate refresh
     */
    void refresh() { render(); }

    void set_symbol_info(Symbol id, const std::string& ticker, Price bid, Price ask) {
        symbols_[id] = {ticker, bid, ask};
    }

private:
    PaperTradingEngine& engine_;
    DashboardConfig config_;
    int64_t last_update_ms_;
    uint64_t frame_count_;

    struct SymbolInfo {
        std::string ticker;
        Price bid;
        Price ask;
    };
    std::unordered_map<Symbol, SymbolInfo> symbols_;

    void render() {
        std::ostringstream out;

        if (config_.clear_screen) {
            out << "\033[2J\033[H"; // Clear screen and move to top
        }

        render_header(out);
        render_regime(out);
        render_positions(out);
        render_pnl(out);
        render_orders(out);
        render_footer(out);

        std::cout << out.str() << std::flush;
    }

    void render_header(std::ostringstream& out) {
        const char* c = config_.use_colors ? color::CYAN : "";
        const char* b = config_.use_colors ? color::BOLD : "";
        const char* r = config_.use_colors ? color::RESET : "";

        out << b << c;
        out << "╔════════════════════════════════════════════════════════════╗\n";
        out << "║              HFT Paper Trading Dashboard                   ║\n";
        out << "╚════════════════════════════════════════════════════════════╝\n";
        out << r;

        // Timestamp
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        out << "  Time: " << std::put_time(std::localtime(&time), "%H:%M:%S");

        // Status
        if (engine_.is_halted()) {
            out << (config_.use_colors ? color::RED : "") << "  [HALTED]" << r;
        } else {
            out << (config_.use_colors ? color::GREEN : "") << "  [RUNNING]" << r;
        }

        out << "  Frame: " << frame_count_ << "\n\n";
    }

    void render_regime(std::ostringstream& out) {
        if (!config_.show_regime)
            return;

        const char* b = config_.use_colors ? color::BOLD : "";
        const char* r = config_.use_colors ? color::RESET : "";

        out << b << "── Market Regime ──────────────────────────────────────────\n" << r;

        auto regime = engine_.current_regime();
        const char* regime_color = "";

        if (config_.use_colors) {
            switch (regime) {
            case strategy::MarketRegime::TrendingUp:
                regime_color = color::GREEN;
                break;
            case strategy::MarketRegime::TrendingDown:
                regime_color = color::RED;
                break;
            case strategy::MarketRegime::Ranging:
                regime_color = color::BLUE;
                break;
            case strategy::MarketRegime::HighVolatility:
                regime_color = color::YELLOW;
                break;
            case strategy::MarketRegime::LowVolatility:
                regime_color = color::CYAN;
                break;
            default:
                regime_color = color::DIM;
                break;
            }
        }

        out << "  Regime: " << regime_color << strategy::regime_to_string(regime) << r;
        out << "  Confidence: " << std::fixed << std::setprecision(1) << (engine_.regime_confidence() * 100) << "%\n";

        out << "  Volatility: " << std::setprecision(2) << (engine_.volatility() * 100) << "%";
        out << "  Trend: ";

        double trend = engine_.trend_strength();
        if (trend > 0.1) {
            out << (config_.use_colors ? color::GREEN : "") << "+" << std::setprecision(1) << trend << r;
        } else if (trend < -0.1) {
            out << (config_.use_colors ? color::RED : "") << std::setprecision(1) << trend << r;
        } else {
            out << (config_.use_colors ? color::DIM : "") << "neutral" << r;
        }
        out << "\n\n";
    }

    void render_positions(std::ostringstream& out) {
        if (!config_.show_positions)
            return;

        const char* b = config_.use_colors ? color::BOLD : "";
        const char* r = config_.use_colors ? color::RESET : "";

        out << b << "── Positions ──────────────────────────────────────────────\n" << r;
        out << "  " << std::left << std::setw(8) << "Symbol" << std::right << std::setw(10) << "Qty" << std::setw(12)
            << "Entry" << std::setw(12) << "Bid" << std::setw(12) << "Ask" << std::setw(12) << "Unreal P&L"
            << "\n";
        out << "  " << std::string(68, '-') << "\n";

        for (const auto& [id, info] : symbols_) {
            const auto& pos = engine_.get_position(id);

            out << "  " << std::left << std::setw(8) << info.ticker;
            out << std::right << std::setw(10) << pos.quantity;
            out << std::setw(12) << std::fixed << std::setprecision(4) << (pos.avg_entry_price / 10000.0);
            out << std::setw(12) << (info.bid / 10000.0);
            out << std::setw(12) << (info.ask / 10000.0);

            // Unrealized P&L with color
            if (config_.use_colors) {
                if (pos.unrealized_pnl > 0) {
                    out << color::GREEN;
                } else if (pos.unrealized_pnl < 0) {
                    out << color::RED;
                }
            }
            out << std::setw(12) << std::setprecision(2) << pos.unrealized_pnl << r;
            out << "\n";
        }
        out << "\n";
    }

    void render_pnl(std::ostringstream& out) {
        if (!config_.show_pnl)
            return;

        const char* b = config_.use_colors ? color::BOLD : "";
        const char* r = config_.use_colors ? color::RESET : "";

        out << b << "── P&L Summary ────────────────────────────────────────────\n" << r;

        double total_pnl = engine_.total_pnl();
        double equity = engine_.equity();
        double drawdown = engine_.drawdown();

        out << "  Equity: $" << std::fixed << std::setprecision(2) << equity;

        out << "  P&L: ";
        if (config_.use_colors) {
            out << (total_pnl >= 0 ? color::GREEN : color::RED);
        }
        out << (total_pnl >= 0 ? "+" : "") << "$" << total_pnl << r;

        out << "  Drawdown: ";
        if (config_.use_colors && drawdown > 0.01) {
            out << color::YELLOW;
        }
        out << std::setprecision(2) << (drawdown * 100) << "%" << r;
        out << "\n\n";
    }

    void render_orders(std::ostringstream& out) {
        if (!config_.show_orders)
            return;

        const char* b = config_.use_colors ? color::BOLD : "";
        const char* r = config_.use_colors ? color::RESET : "";

        out << b << "── Order Statistics ───────────────────────────────────────\n" << r;
        out << "  Total Orders: " << engine_.total_orders();
        out << "  Fills: " << engine_.total_fills();
        out << "  Pending: " << std::setw(4) << engine_.config().fill_config.min_latency_ns / 1000 << "us";
        out << "\n\n";
    }

    void render_footer(std::ostringstream& out) {
        const char* d = config_.use_colors ? color::DIM : "";
        const char* r = config_.use_colors ? color::RESET : "";

        out << d << "───────────────────────────────────────────────────────────────\n";
        out << "  Press Ctrl+C to exit" << r << "\n";
    }
};

/**
 * Compact status line (single line, for tight loops)
 *
 * Format: [HH:MM:SS] REGIME | AAPL: +100 @ 150.00 P&L: +$123.45 | DD: 0.5%
 */
class StatusLine {
public:
    explicit StatusLine(PaperTradingEngine& engine) : engine_(engine), last_update_ms_(0) {}

    /**
     * Print status line (max 10 updates/sec)
     */
    void print() {
        auto now = std::chrono::steady_clock::now();
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

        if (now_ms - last_update_ms_ < 100) {
            return;
        }
        last_update_ms_ = now_ms;

        // Get timestamp
        auto time_now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(time_now);
        std::tm* tm = std::localtime(&time);

        // Clear line and print
        std::printf("\r\033[K[%02d:%02d:%02d] %s | P&L: %+.2f | DD: %.1f%% | Orders: %lu  ", tm->tm_hour, tm->tm_min,
                    tm->tm_sec, strategy::regime_to_string(engine_.current_regime()).c_str(), engine_.total_pnl(),
                    engine_.drawdown() * 100, engine_.total_orders());
        std::fflush(stdout);
    }

private:
    PaperTradingEngine& engine_;
    int64_t last_update_ms_;
};

} // namespace paper
} // namespace hft
