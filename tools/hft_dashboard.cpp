/**
 * HFT Dashboard - ImGui-based Real-time Monitor
 *
 * Professional trading dashboard using Dear ImGui
 * Like the tools used by real HFT firms
 *
 * Features:
 * - Real-time P&L tracking with chart
 * - Position monitoring per symbol
 * - Live event stream
 * - Trade statistics
 */

#include "../include/ipc/trade_event.hpp"
#include "../include/ipc/shared_ring_buffer.hpp"
#include "../include/ipc/shared_portfolio_state.hpp"
#include "../include/ipc/shared_config.hpp"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include <iostream>
#include <deque>
#include <map>
#include <vector>
#include <chrono>
#include <thread>
#include <cmath>
#include <csignal>
#include <atomic>
#include <algorithm>
#include <cstring>

using namespace hft::ipc;

// ============================================================================
// Global State
// ============================================================================

std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running = false;
}

// ============================================================================
// Position Tracking
// ============================================================================

// Market regime (must match hft's MarketRegime enum)
enum class MarketRegime : uint8_t {
    Unknown = 0,
    TrendingUp = 1,
    TrendingDown = 2,
    Ranging = 3,
    HighVolatility = 4,
    LowVolatility = 5
};

inline const char* regime_to_string(MarketRegime regime) {
    switch (regime) {
        case MarketRegime::TrendingUp: return "UP";
        case MarketRegime::TrendingDown: return "DOWN";
        case MarketRegime::Ranging: return "RANGE";
        case MarketRegime::HighVolatility: return "H.VOL";
        case MarketRegime::LowVolatility: return "L.VOL";
        default: return "?";
    }
}

// Strategy used for each regime (matches hft.cpp logic)
inline const char* regime_to_strategy(MarketRegime regime) {
    switch (regime) {
        case MarketRegime::TrendingUp: return "MOMENTUM";    // Buy on medium signal, ride trend
        case MarketRegime::TrendingDown: return "DEFENSIVE"; // No buying, wait for reversal
        case MarketRegime::Ranging: return "MEAN_REV";       // Buy dips, sell rips
        case MarketRegime::LowVolatility: return "MEAN_REV"; // Same as ranging
        case MarketRegime::HighVolatility: return "CAUTIOUS"; // Only strong signals
        default: return "NONE";
    }
}

inline ImVec4 regime_color(MarketRegime regime) {
    switch (regime) {
        case MarketRegime::TrendingUp: return ImVec4(0.2f, 1.0f, 0.2f, 1.0f);    // Green
        case MarketRegime::TrendingDown: return ImVec4(1.0f, 0.3f, 0.3f, 1.0f);  // Red
        case MarketRegime::Ranging: return ImVec4(0.9f, 0.9f, 0.2f, 1.0f);       // Yellow
        case MarketRegime::HighVolatility: return ImVec4(1.0f, 0.5f, 0.0f, 1.0f); // Orange
        case MarketRegime::LowVolatility: return ImVec4(0.5f, 0.5f, 0.8f, 1.0f);  // Blue-gray
        default: return ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
    }
}

struct Position {
    std::string symbol;
    double quantity = 0;
    double avg_entry_price = 0;
    double total_cost = 0;
    double last_price = 0;
    double realized_pnl = 0;
    int trades = 0;
    MarketRegime regime = MarketRegime::Unknown;

    double unrealized_pnl() const {
        if (quantity == 0) return 0;
        return quantity * (last_price - avg_entry_price);
    }

    double market_value() const {
        return quantity * last_price;
    }

    void add_buy(double qty, double price) {
        total_cost += qty * price;
        quantity += qty;
        avg_entry_price = quantity > 0 ? total_cost / quantity : 0;
        last_price = price;
        trades++;
    }

    void add_sell(double qty, double price) {
        if (quantity > 0) {
            double pnl = qty * (price - avg_entry_price);
            realized_pnl += pnl;
            total_cost -= qty * avg_entry_price;
            quantity -= qty;
        }
        last_price = price;
        trades++;
    }
};

// ============================================================================
// Dashboard Data
// ============================================================================

struct DashboardData {
    // Stats
    uint64_t total_events = 0;
    uint64_t fills = 0;
    uint64_t targets = 0;
    uint64_t stops = 0;

    // P&L
    double realized_pnl = 0.0;
    double total_profit = 0.0;
    double total_loss = 0.0;
    int winning_trades = 0;
    int losing_trades = 0;

    // Positions
    std::map<std::string, Position> positions;

    // P&L history for chart (sampled every 100ms)
    std::deque<float> pnl_history;
    static constexpr size_t MAX_HISTORY = 600;  // 60 seconds at 10 samples/sec

    // Price history per symbol (for sparklines)
    std::map<std::string, std::deque<float>> price_history;
    static constexpr size_t MAX_PRICE_HISTORY = 100;

    // Events
    struct EventEntry {
        std::string text;
        ImVec4 color;
        double timestamp;
    };
    std::deque<EventEntry> events;
    static constexpr size_t MAX_EVENTS = 50;

    // Timing
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point last_pnl_sample;
    uint64_t first_event_ts = 0;

    DashboardData()
        : start_time(std::chrono::steady_clock::now())
        , last_pnl_sample(std::chrono::steady_clock::now())
    {}

    // Load initial state from shared portfolio (for dashboard restarts)
    void load_from_shared_state(const SharedPortfolioState* state) {
        if (!state) return;

        // Load global stats
        fills = state->total_fills.load();
        targets = state->total_targets.load();
        stops = state->total_stops.load();
        total_events = state->total_events.load();
        realized_pnl = state->total_realized_pnl();
        winning_trades = state->winning_trades.load();
        losing_trades = state->losing_trades.load();

        // Calculate profit/loss totals
        if (realized_pnl >= 0) {
            total_profit = realized_pnl;
        } else {
            total_loss = std::abs(realized_pnl);
        }

        // Load positions
        for (size_t i = 0; i < MAX_PORTFOLIO_SYMBOLS; ++i) {
            const auto& slot = state->positions[i];
            if (!slot.active.load()) continue;

            std::string sym(slot.symbol);
            if (sym.empty()) continue;

            Position& pos = positions[sym];
            pos.symbol = sym;
            pos.quantity = slot.quantity();
            pos.avg_entry_price = slot.avg_price();
            pos.last_price = slot.last_price();
            pos.realized_pnl = slot.realized_pnl();
            pos.trades = slot.buy_count.load() + slot.sell_count.load();

            // Recalculate total_cost from avg_entry and qty
            pos.total_cost = pos.quantity * pos.avg_entry_price;
        }

        std::cout << "[IPC] Loaded state from shared memory: "
                  << "fills=" << fills << ", realized_pnl=$" << realized_pnl << "\n";
    }

    double win_rate() const {
        int total = winning_trades + losing_trades;
        return total > 0 ? (double)winning_trades / total * 100 : 0;
    }

    double total_unrealized_pnl() const {
        double total = 0;
        for (const auto& [sym, pos] : positions) {
            total += pos.unrealized_pnl();
        }
        return total;
    }

    double total_equity() const {
        return realized_pnl + total_unrealized_pnl();
    }

    void sample_pnl() {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_pnl_sample).count();

        if (elapsed >= 100) {  // Sample every 100ms
            pnl_history.push_back((float)total_equity());
            if (pnl_history.size() > MAX_HISTORY) {
                pnl_history.pop_front();
            }
            last_pnl_sample = now;
        }
    }

    void update_price_history(const std::string& symbol, double price) {
        auto& history = price_history[symbol];
        history.push_back((float)price);
        if (history.size() > MAX_PRICE_HISTORY) {
            history.pop_front();
        }
    }

    void process_event(const TradeEvent& e) {
        total_events++;
        if (first_event_ts == 0) first_event_ts = e.timestamp_ns;

        double rel_sec = (e.timestamp_ns - first_event_ts) / 1e9;
        char buf[256];
        std::string symbol(e.ticker, strnlen(e.ticker, sizeof(e.ticker)));

        switch (e.type) {
            case EventType::Fill: {
                fills++;

                // Update position
                auto& pos = positions[symbol];
                pos.symbol = symbol;
                if (e.side == 0) {  // Buy
                    pos.add_buy(e.quantity, e.price);
                } else {  // Sell
                    pos.add_sell(e.quantity, e.price);
                }

                update_price_history(symbol, e.price);

                snprintf(buf, sizeof(buf), "%6.1fs  %s  %s  %.0f @ $%.4f  [#%u]",
                    rel_sec, e.side == 0 ? "BUY " : "SELL",
                    symbol.c_str(), e.quantity, e.price, e.order_id);
                // Green for BUY, Yellow for SELL
                events.push_front({buf, e.side == 0 ?
                    ImVec4(0.2f, 0.9f, 0.2f, 1.0f) : ImVec4(0.9f, 0.9f, 0.2f, 1.0f), rel_sec});
                break;
            }
            case EventType::TargetHit: {
                targets++;
                winning_trades++;
                double pnl = e.pnl_cents / 100.0;
                realized_pnl += pnl;
                total_profit += pnl;

                // Close position
                auto& pos = positions[symbol];
                pos.add_sell(e.quantity, e.price);

                snprintf(buf, sizeof(buf), "%6.1fs  TARGET %s  %.0f @ $%.4f  +$%.2f",
                    rel_sec, symbol.c_str(), e.quantity, e.price, pnl);
                // Bright green for profit
                events.push_front({buf, ImVec4(0.2f, 1.0f, 0.2f, 1.0f), rel_sec});
                break;
            }
            case EventType::StopLoss: {
                stops++;
                losing_trades++;
                double pnl = e.pnl_cents / 100.0;
                realized_pnl += pnl;
                total_loss += std::abs(pnl);

                // Close position
                auto& pos = positions[symbol];
                pos.add_sell(e.quantity, e.price);

                snprintf(buf, sizeof(buf), "%6.1fs  STOP   %s  %.0f @ $%.4f  $%.2f",
                    rel_sec, symbol.c_str(), e.quantity, e.price, pnl);
                // Red for loss
                events.push_front({buf, ImVec4(1.0f, 0.2f, 0.2f, 1.0f), rel_sec});
                break;
            }
            case EventType::Signal: {
                snprintf(buf, sizeof(buf), "%6.1fs  SIGNAL %s  %s",
                    rel_sec, symbol.c_str(), e.side == 0 ? "BUY" : "SELL");
                events.push_front({buf, ImVec4(0.4f, 0.8f, 1.0f, 1.0f), rel_sec});
                break;
            }
            case EventType::RegimeChange: {
                // Update position's regime
                auto& pos = positions[symbol];
                pos.symbol = symbol;
                MarketRegime new_regime = static_cast<MarketRegime>(e.regime);
                pos.regime = new_regime;

                snprintf(buf, sizeof(buf), "%6.1fs  REGIME %s  -> %s",
                    rel_sec, symbol.c_str(), regime_to_string(new_regime));
                events.push_front({buf, regime_color(new_regime), rel_sec});
                break;
            }
            default:
                return;
        }

        if (events.size() > MAX_EVENTS) {
            events.pop_back();
        }
    }
};

// ============================================================================
// ImGui Rendering
// ============================================================================

void render_dashboard(DashboardData& data, const SharedPortfolioState* portfolio_state, SharedConfig* config) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - data.start_time).count();
    int hours = elapsed / 3600;
    int mins = (elapsed % 3600) / 60;
    int secs = elapsed % 60;

    // Sample P&L for chart
    data.sample_pnl();

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("HFT Dashboard", nullptr, flags);

    // =========================================================================
    // Header
    // =========================================================================
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
    ImGui::Text("HFT TRADING DASHBOARD");
    ImGui::PopStyleColor();

    // Show session ID if available
    if (portfolio_state) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        ImGui::Text("  Session: %08X", portfolio_state->session_id);
        ImGui::PopStyleColor();
    }

    ImGui::SameLine(ImGui::GetWindowWidth() - 280);
    ImGui::Text("Runtime: %02d:%02d:%02d  |  Events: %lu", hours, mins, secs, data.total_events);

    ImGui::Separator();

    // =========================================================================
    // Stats Row
    // =========================================================================
    float col_width = ImGui::GetWindowWidth() / 5.0f;
    ImGui::Columns(5, "stats", false);

    // Fills
    ImGui::Text("FILLS");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.9f, 0.4f, 1.0f));
    ImGui::SetWindowFontScale(1.5f);
    ImGui::Text("%lu", data.fills);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::NextColumn();

    // Targets
    ImGui::Text("TARGETS");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.2f, 1.0f));
    ImGui::SetWindowFontScale(1.5f);
    ImGui::Text("%lu", data.targets);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::NextColumn();

    // Stops
    ImGui::Text("STOPS");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
    ImGui::SetWindowFontScale(1.5f);
    ImGui::Text("%lu", data.stops);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::NextColumn();

    // Win Rate
    ImGui::Text("WIN RATE");
    ImGui::SetWindowFontScale(1.5f);
    ImGui::Text("%.0f%%", data.win_rate());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::NextColumn();

    // Active Positions
    int active_positions = 0;
    for (const auto& [sym, pos] : data.positions) {
        if (pos.quantity > 0) active_positions++;
    }
    ImGui::Text("POSITIONS");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.2f, 1.0f));
    ImGui::SetWindowFontScale(1.5f);
    ImGui::Text("%d", active_positions);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();

    ImGui::Columns(1);
    ImGui::Separator();

    // =========================================================================
    // Main Content - Two columns
    // =========================================================================
    float left_width = ImGui::GetWindowWidth() * 0.6f;

    ImGui::BeginChild("LeftPanel", ImVec2(left_width, 0), false);

    // P&L Section
    ImGui::BeginChild("PnL", ImVec2(0, 100), true);
    ImGui::Text("P&L SUMMARY");
    ImGui::Separator();

    ImGui::Columns(3, "pnl_cols", false);

    // Realized P&L
    ImGui::Text("Realized");
    if (data.realized_pnl >= 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.2f, 1.0f));
        ImGui::SetWindowFontScale(1.8f);
        ImGui::Text("+$%.2f", data.realized_pnl);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
        ImGui::SetWindowFontScale(1.8f);
        ImGui::Text("-$%.2f", std::abs(data.realized_pnl));
    }
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::NextColumn();

    // Unrealized P&L
    double unrealized = data.total_unrealized_pnl();
    ImGui::Text("Unrealized");
    if (unrealized >= 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.9f, 0.4f, 1.0f));
        ImGui::Text("+$%.2f", unrealized);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.4f, 0.4f, 1.0f));
        ImGui::Text("-$%.2f", std::abs(unrealized));
    }
    ImGui::PopStyleColor();
    ImGui::NextColumn();

    // Total Equity
    double equity = data.total_equity();
    ImGui::Text("Total Equity");
    if (equity >= 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.3f, 1.0f));
        ImGui::Text("+$%.2f", equity);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        ImGui::Text("-$%.2f", std::abs(equity));
    }
    ImGui::PopStyleColor();

    ImGui::Columns(1);
    ImGui::EndChild();

    // P&L Chart
    ImGui::BeginChild("Chart", ImVec2(0, 180), true);
    ImGui::Text("EQUITY CURVE");
    ImGui::Separator();

    if (!data.pnl_history.empty()) {
        std::vector<float> history(data.pnl_history.begin(), data.pnl_history.end());

        float min_val = *std::min_element(history.begin(), history.end());
        float max_val = *std::max_element(history.begin(), history.end());

        // Symmetric range around zero
        float range = std::max(std::abs(min_val), std::abs(max_val));
        if (range < 1.0f) range = 1.0f;
        range *= 1.1f;  // 10% padding

        // Draw zero line indicator
        ImGui::Text("%.2f", range);
        ImGui::SameLine(ImGui::GetWindowWidth() - 100);
        ImGui::Text("Last: $%.2f", history.back());

        ImGui::PlotLines("##equity", history.data(), history.size(),
            0, nullptr, -range, range, ImVec2(-1, 120));

        ImGui::Text("-%.2f", range);
    } else {
        ImGui::Text("Waiting for data...");
    }

    ImGui::EndChild();

    // Positions Table
    ImGui::BeginChild("Positions", ImVec2(0, 0), true);
    ImGui::Text("ACTIVE POSITIONS");
    ImGui::Separator();

    if (ImGui::BeginTable("positions_table", 7,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {

        ImGui::TableSetupColumn("Symbol", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Regime", ImGuiTableColumnFlags_WidthFixed, 55);
        ImGui::TableSetupColumn("Qty", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("Entry", ImGuiTableColumnFlags_WidthFixed, 75);
        ImGui::TableSetupColumn("Last", ImGuiTableColumnFlags_WidthFixed, 75);
        ImGui::TableSetupColumn("P&L", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Chart", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (const auto& [sym, pos] : data.positions) {
            if (pos.quantity <= 0) continue;

            ImGui::TableNextRow();

            // Symbol
            ImGui::TableNextColumn();
            ImGui::Text("%s", sym.c_str());

            // Regime (colored)
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, regime_color(pos.regime));
            ImGui::Text("%s", regime_to_string(pos.regime));
            ImGui::PopStyleColor();

            // Quantity
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", pos.quantity);

            // Entry price
            ImGui::TableNextColumn();
            ImGui::Text("$%.4f", pos.avg_entry_price);

            // Last price
            ImGui::TableNextColumn();
            ImGui::Text("$%.4f", pos.last_price);

            // Unrealized P&L
            ImGui::TableNextColumn();
            double pnl = pos.unrealized_pnl();
            if (pnl >= 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.2f, 1.0f));
                ImGui::Text("+$%.2f", pnl);
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
                ImGui::Text("-$%.2f", std::abs(pnl));
            }
            ImGui::PopStyleColor();

            // Sparkline chart
            ImGui::TableNextColumn();
            auto it = data.price_history.find(sym);
            if (it != data.price_history.end() && !it->second.empty()) {
                std::vector<float> prices(it->second.begin(), it->second.end());
                float min_p = *std::min_element(prices.begin(), prices.end());
                float max_p = *std::max_element(prices.begin(), prices.end());
                ImGui::PlotLines("##spark", prices.data(), prices.size(),
                    0, nullptr, min_p, max_p, ImVec2(100, 20));
            }
        }

        ImGui::EndTable();
    }

    ImGui::EndChild();
    ImGui::EndChild();  // LeftPanel

    ImGui::SameLine();

    // Right Panel - Config + Events
    ImGui::BeginChild("RightPanel", ImVec2(0, 0), false);

    // Config Panel (collapsible)
    if (config && ImGui::CollapsingHeader("STRATEGY CONFIG", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent(10);

        // ===== HFT CONNECTION STATUS =====
        {
            const char* status_names[] = { "STOPPED", "STARTING", "RUNNING", "SHUTTING DOWN" };
            const ImVec4 status_colors[] = {
                ImVec4(0.5f, 0.5f, 0.5f, 1.0f),  // STOPPED - gray
                ImVec4(1.0f, 0.8f, 0.2f, 1.0f),  // STARTING - yellow
                ImVec4(0.2f, 1.0f, 0.2f, 1.0f),  // RUNNING - green
                ImVec4(1.0f, 0.5f, 0.0f, 1.0f),  // SHUTTING DOWN - orange
            };

            uint8_t hft_status = config->get_hft_status();
            bool is_alive = config->is_hft_alive(3);  // 3 second timeout

            // Check if HFT is really alive (heartbeat check)
            if (hft_status == 2 && !is_alive) {
                // HFT was running but stopped unexpectedly (crashed or killed -9)
                hft_status = 0;  // Show as stopped
            }

            if (hft_status > 3) hft_status = 0;

            ImGui::Text("HFT:");
            ImGui::SameLine();

            if (hft_status == 2 && is_alive) {
                // Running and responsive
                ImGui::PushStyleColor(ImGuiCol_Text, status_colors[2]);
                ImGui::Text("[CONNECTED]");
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::TextDisabled("(%.8s)", config->get_build_hash());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Git commit: %.8s\nPID: %d\nView code: git show %.8s",
                                      config->get_build_hash(), config->get_hft_pid(), config->get_build_hash());
                }
            } else if (hft_status == 0 || !is_alive) {
                // Stopped or unresponsive
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                ImGui::Text("[DISCONNECTED]");
                ImGui::PopStyleColor();
            } else {
                // Starting or shutting down
                ImGui::PushStyleColor(ImGuiCol_Text, status_colors[hft_status]);
                ImGui::Text("[%s]", status_names[hft_status]);
                ImGui::PopStyleColor();
            }
        }

        ImGui::Separator();

        // ===== LIVE STATUS (from HFT) =====
        {
            const char* mode_names[] = { "AUTO", "AGGRESSIVE", "NORMAL", "CAUTIOUS", "DEFENSIVE", "EXIT_ONLY" };
            const ImVec4 mode_colors[] = {
                ImVec4(0.5f, 0.5f, 0.5f, 1.0f),  // AUTO - gray
                ImVec4(0.2f, 1.0f, 0.2f, 1.0f),  // AGGRESSIVE - green
                ImVec4(0.4f, 0.8f, 1.0f, 1.0f),  // NORMAL - cyan
                ImVec4(1.0f, 0.8f, 0.2f, 1.0f),  // CAUTIOUS - yellow
                ImVec4(1.0f, 0.5f, 0.0f, 1.0f),  // DEFENSIVE - orange
                ImVec4(1.0f, 0.2f, 0.2f, 1.0f),  // EXIT_ONLY - red
            };

            uint8_t active = config->get_active_mode();
            if (active > 5) active = 2;  // Default to NORMAL if invalid

            ImGui::Text("Active:");
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, mode_colors[active]);
            ImGui::Text("[%s]", mode_names[active]);
            ImGui::PopStyleColor();

            // Win/Loss streak
            int32_t wins = config->get_consecutive_wins();
            int32_t losses = config->get_consecutive_losses();
            ImGui::SameLine();
            if (losses > 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                ImGui::Text(" L:%d", losses);
                ImGui::PopStyleColor();
            } else if (wins > 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.3f, 1.0f));
                ImGui::Text(" W:%d", wins);
                ImGui::PopStyleColor();
            }

            // Active signals
            uint8_t signals = config->get_active_signals();
            if (signals > 0) {
                ImGui::SameLine();
                ImGui::TextDisabled(" Signals:%d", signals);
            }
        }

        ImGui::Separator();

        // ===== MASTER CONTROLS =====
        // Trading enabled toggle
        bool trading = config->trading_enabled.load() != 0;
        if (ImGui::Checkbox("Trading Enabled", &trading)) {
            config->set_trading_enabled(trading);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "MASTER SWITCH\n\n"
                "OFF: Watch only, no trades\n"
                "ON:  Execute trades on signals\n\n"
                "Use: Disable when observing market\n"
                "or testing strategy changes");
        }

        // Force Mode dropdown
        const char* mode_names[] = { "AUTO", "AGGRESSIVE", "NORMAL", "CAUTIOUS", "DEFENSIVE", "EXIT_ONLY" };
        int current_mode = config->get_force_mode();
        ImGui::SetNextItemWidth(120);
        if (ImGui::Combo("Force Mode", &current_mode, mode_names, 6)) {
            config->set_force_mode(static_cast<uint8_t>(current_mode));
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "STRATEGY MODE\n\n"
                "AUTO: System auto-selects (recommended)\n"
                "AGGRESSIVE: Max position, tight stops\n"
                "NORMAL: Standard parameters\n"
                "CAUTIOUS: Small position, wide stops\n"
                "DEFENSIVE: Protect existing positions only\n"
                "EXIT_ONLY: Close positions, no new trades\n\n"
                "Use: Manual override based on\n"
                "market conditions");
        }

        ImGui::Spacing();

        // ===== REVIEW GATE (Collapsible) =====
        if (ImGui::TreeNode("Review Gate")) {
            ImGui::TextDisabled("Lazy evaluation - checks only on losses");

            // Spread multiplier
            float spread_mult = config->spread_multiplier();
            if (ImGui::SliderFloat("Spread Mult", &spread_mult, 1.0f, 3.0f, "%.1fx")) {
                config->set_spread_multiplier(spread_mult);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "REGIME CHECK THRESHOLD\n\n"
                    "Formula: PnL < -(spread * multiplier)\n\n"
                    "1.0x: Very sensitive, check on small losses\n"
                    "1.5x: Balanced (recommended)\n"
                    "2.0x: Tolerant, check on larger losses\n"
                    "3.0x: Very tolerant, only severe losses\n\n"
                    "Example: spread=$10, mult=1.5x\n"
                    "Regime checked when PnL < -$15");
            }

            // Drawdown threshold
            float drawdown = config->drawdown_threshold() * 100;
            if (ImGui::SliderFloat("Max Drawdown", &drawdown, 1.0f, 10.0f, "%.1f%%")) {
                config->set_drawdown_threshold(drawdown / 100.0);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "MAX DRAWDOWN LIMIT\n\n"
                    "If portfolio drops this much from peak,\n"
                    "strategy switches to DEFENSIVE mode.\n\n"
                    "2%%: Aggressive protection (recommended)\n"
                    "5%%: Normal protection\n"
                    "10%%: Relaxed, for swing trading\n\n"
                    "Example: $10K portfolio, 2%% = $200 max DD");
            }

            // Loss streak
            int loss_streak = config->loss_streak();
            if (ImGui::SliderInt("Loss Streak", &loss_streak, 1, 5)) {
                config->set_loss_streak(loss_streak);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "CONSECUTIVE LOSS LIMIT\n\n"
                    "After this many losses in a row,\n"
                    "strategy switches to CAUTIOUS mode.\n\n"
                    "2: Sensitive, fast reaction (recommended)\n"
                    "3: Normal\n"
                    "5: Tolerant, for trend following\n\n"
                    "Why: Consecutive losses often signal\n"
                    "a regime change");
            }

            ImGui::TreePop();
        }

        // ===== POSITION SIZING (Collapsible) =====
        if (ImGui::TreeNode("Position Sizing")) {
            ImGui::TextDisabled("Kelly Criterion based dynamic sizing");

            // Base position
            float base_pos = config->base_position_pct() * 100;
            if (ImGui::SliderFloat("Base Size", &base_pos, 0.5f, 5.0f, "%.1f%%")) {
                config->set_base_position_pct(base_pos / 100.0);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "MINIMUM POSITION\n\n"
                    "Used for low-confidence signals.\n\n"
                    "1%%: Conservative\n"
                    "2%%: Normal (recommended)\n"
                    "3-5%%: Aggressive\n\n"
                    "Example: $10K portfolio, 2%% = $200/trade");
            }

            // Max position
            float max_pos = config->max_position_pct() * 100;
            if (ImGui::SliderFloat("Max Size", &max_pos, 1.0f, 10.0f, "%.1f%%")) {
                config->set_max_position_pct(max_pos / 100.0);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "MAXIMUM POSITION\n\n"
                    "Used for high-confidence signals.\n"
                    "Never exceeds this value.\n\n"
                    "3%%: Conservative\n"
                    "5%%: Normal (recommended)\n"
                    "10%%: Aggressive, testing only\n\n"
                    "Risk: Max > 5%% = large single trade loss");
            }

            // Show current sizing formula
            ImGui::TextDisabled("Size = Base + (Max-Base) * confidence");

            ImGui::TreePop();
        }

        // ===== TARGET / STOP (Collapsible) =====
        if (ImGui::TreeNode("Target / Stop")) {
            ImGui::TextDisabled("Take Profit and Stop Loss levels");

            // Target
            float target = config->target_pct() * 100;
            if (ImGui::SliderFloat("Target %%", &target, 0.5f, 5.0f, "%.2f%%")) {
                config->set_target_pct(target / 100.0);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "TAKE PROFIT\n\n"
                    "Close position when price rises this much.\n\n"
                    "0.5-1%%: Scalping, high win rate\n"
                    "1.5%%: Day trading (recommended)\n"
                    "3-5%%: Swing trading\n\n"
                    "Example: $100 entry, 1.5%% = $101.50 target");
            }

            // Stop
            float stop = config->stop_pct() * 100;
            if (ImGui::SliderFloat("Stop %%", &stop, 0.25f, 3.0f, "%.2f%%")) {
                config->set_stop_pct(stop / 100.0);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "STOP LOSS\n\n"
                    "Close position when price drops this much.\n\n"
                    "0.25-0.5%%: Tight stop, scalping\n"
                    "1%%: Normal (recommended)\n"
                    "2-3%%: Loose stop, swing trading\n\n"
                    "Warning: Tight = whipsaw risk\n"
                    "Loose = large loss risk");
            }

            // Risk:Reward display with color coding
            float rr = target / stop;
            const char* rr_label;
            ImVec4 rr_color;
            if (rr < 1.0f) {
                rr_label = "BAD";
                rr_color = ImVec4(1.0f, 0.2f, 0.2f, 1.0f);
            } else if (rr < 1.5f) {
                rr_label = "Low";
                rr_color = ImVec4(1.0f, 0.5f, 0.0f, 1.0f);
            } else if (rr < 2.0f) {
                rr_label = "OK";
                rr_color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
            } else {
                rr_label = "Ideal";
                rr_color = ImVec4(0.2f, 1.0f, 0.2f, 1.0f);
            }

            ImGui::Text("Risk:Reward = 1:%.2f", rr);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, rr_color);
            ImGui::Text("(%s)", rr_label);
            ImGui::PopStyleColor();

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "RISK:REWARD RATIO\n\n"
                    "< 1:1  BAD - Destined to lose\n"
                    "1:1.5  Low - Needs 60%%+ win rate\n"
                    "1:2    Ideal - 40%% win rate enough\n"
                    "1:3+   Excellent - For trend following\n\n"
                    "Math:\n"
                    "  Profit = RR * WinRate\n"
                    "  Loss = 1 * LossRate\n"
                    "  Profit > Loss required\n\n"
                    "RR=2, WR=40%% -> 0.8 > 0.6 = PROFITABLE");
            }

            // Win rate required calculation
            float required_wr = 100.0f / (1.0f + rr);
            ImGui::TextDisabled("Min WinRate: %.0f%%", required_wr);

            ImGui::TreePop();
        }

        ImGui::Unindent(10);
        ImGui::Spacing();
    }

    // Events Panel
    float events_height = config ? 0 : 0;  // Take remaining space
    ImGui::BeginChild("Events", ImVec2(0, events_height), true);
    ImGui::Text("LIVE EVENTS");
    ImGui::Separator();

    for (const auto& ev : data.events) {
        ImGui::PushStyleColor(ImGuiCol_Text, ev.color);
        ImGui::Text("%s", ev.text.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::EndChild();
    ImGui::EndChild();  // RightPanel

    ImGui::End();
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Initialize GLFW
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW\n";
        return 1;
    }

    // GL 3.3 + GLSL 330
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    // Create window
    GLFWwindow* window = glfwCreateWindow(1400, 900, "HFT Trading Dashboard", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create window\n";
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // VSync

    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Dark theme customization
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.05f, 0.07f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.07f, 0.07f, 0.09f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.10f, 0.12f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.15f, 0.15f, 0.18f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.20f, 0.25f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.12f, 0.12f, 0.15f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.08f, 0.08f, 0.10f, 1.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.10f, 0.10f, 0.12f, 1.0f);

    // Platform/Renderer bindings
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Connect to shared memory
    std::cout << "Connecting to HFT engine...\n";

    // Try to open shared portfolio state (read-only)
    SharedPortfolioState* portfolio_state = SharedPortfolioState::open("/hft_portfolio");
    if (portfolio_state) {
        std::cout << "Found portfolio state (cash=$" << portfolio_state->cash() << ")\n";
    }

    // Try to open shared config (read-write for dashboard control)
    SharedConfig* shared_config = SharedConfig::open_rw("/hft_config");
    if (!shared_config) {
        // Create if doesn't exist
        shared_config = SharedConfig::create("/hft_config");
    }
    if (shared_config) {
        std::cout << "Config connected (spread_mult=" << shared_config->spread_multiplier() << "x)\n";
    }

    SharedRingBuffer<TradeEvent>* buffer = nullptr;
    int retries = 0;

    while (!buffer && retries < 30 && g_running) {
        try {
            buffer = new SharedRingBuffer<TradeEvent>("/hft_events", false);
            std::cout << "Connected to event stream!\n";
        } catch (...) {
            retries++;
            std::cout << "Waiting for HFT engine... (" << retries << "/30)\n";
            glfwPollEvents();
            if (glfwWindowShouldClose(window)) {
                g_running = false;
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    if (!buffer) {
        std::cerr << "Could not connect to HFT engine\n";
        if (portfolio_state) munmap(portfolio_state, sizeof(SharedPortfolioState));
        if (shared_config) munmap(shared_config, sizeof(SharedConfig));
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // Main loop
    DashboardData data;
    TradeEvent event;

    // Load initial state from shared portfolio (if available)
    data.load_from_shared_state(portfolio_state);

    while (!glfwWindowShouldClose(window) && g_running) {
        glfwPollEvents();

        // Process events from shared memory
        while (buffer->pop(event)) {
            data.process_event(event);
        }

        // Update prices from shared state (10Hz for smoother charts)
        static auto last_price_update = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        auto price_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_price_update).count();

        if (price_elapsed >= 100 && portfolio_state && portfolio_state->trading_active.load()) {
            last_price_update = now;
            for (size_t i = 0; i < MAX_PORTFOLIO_SYMBOLS; ++i) {
                const auto& slot = portfolio_state->positions[i];
                if (!slot.active.load()) continue;

                std::string sym(slot.symbol);
                if (sym.empty()) continue;

                double price = slot.last_price();
                if (price > 0) {
                    // Update position's last price and regime
                    auto it = data.positions.find(sym);
                    if (it != data.positions.end()) {
                        it->second.last_price = price;
                        it->second.regime = static_cast<MarketRegime>(slot.regime.load());
                    } else {
                        // Create position entry if it doesn't exist (for regime display)
                        auto& pos = data.positions[sym];
                        pos.symbol = sym;
                        pos.last_price = price;
                        pos.regime = static_cast<MarketRegime>(slot.regime.load());
                    }
                    // Update price history for sparkline
                    data.update_price_history(sym, price);
                }
            }
        }

        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Render dashboard
        render_dashboard(data, portfolio_state, shared_config);

        // Render
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.05f, 0.05f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);

        // WSL2'de VSync düzgün çalışmayabilir, CPU'yu rahatlatmak için
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Cleanup
    delete buffer;
    if (portfolio_state) {
        munmap(portfolio_state, sizeof(SharedPortfolioState));
    }
    if (shared_config) {
        munmap(shared_config, sizeof(SharedConfig));
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    // Final summary
    std::cout << "\nFinal Summary:\n";
    std::cout << "  Events: " << data.total_events << "\n";
    std::cout << "  Realized P&L: " << (data.realized_pnl >= 0 ? "+" : "") << "$" << data.realized_pnl << "\n";
    std::cout << "  Win Rate: " << data.winning_trades << "W / " << data.losing_trades << "L\n";

    return 0;
}
