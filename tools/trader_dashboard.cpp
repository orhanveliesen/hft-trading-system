/**
 * Trader Dashboard - ImGui-based Real-time Monitor
 *
 * Professional trading dashboard using Dear ImGui
 * Like the tools used by real trading firms
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
#include "../include/ipc/shared_paper_config.hpp"
#include "../include/ipc/symbol_config.hpp"

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
    LowVolatility = 5,
    Spike = 6   // DANGER: Sudden abnormal move
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

// Strategy type to string (for display)
inline const char* strategy_type_to_display(uint8_t st) {
    switch (st) {
        case 0: return "NONE";
        case 1: return "MOMENTUM";
        case 2: return "MEAN_REV";
        case 3: return "MKT_MAKER";
        case 4: return "DEFENSIVE";
        case 5: return "CAUTIOUS";
        case 6: return "SMART";
        default: return "?";
    }
}

// Convert MarketRegime to index for config lookup
inline int regime_to_index(MarketRegime regime) {
    switch (regime) {
        case MarketRegime::Unknown: return 0;
        case MarketRegime::TrendingUp: return 1;
        case MarketRegime::TrendingDown: return 2;
        case MarketRegime::Ranging: return 3;
        case MarketRegime::HighVolatility: return 4;
        case MarketRegime::LowVolatility: return 5;
        case MarketRegime::Spike: return 6;
        default: return 0;
    }
}

// Legacy fallback (used when config not available)
inline const char* regime_to_strategy_fallback(MarketRegime regime) {
    switch (regime) {
        case MarketRegime::TrendingUp: return "MOMENTUM";
        case MarketRegime::TrendingDown: return "DEFENSIVE";
        case MarketRegime::Ranging: return "MKT_MAKER";
        case MarketRegime::LowVolatility: return "MKT_MAKER";
        case MarketRegime::HighVolatility: return "CAUTIOUS";
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

    // Cash tracking (for correct equity calculation)
    double current_cash = 0.0;
    double initial_cash = 0.0;

    // Trading costs
    double total_commissions = 0.0;
    double total_spread_cost = 0.0;
    double total_slippage = 0.0;
    double total_volume = 0.0;

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

    // Status messages (debug/info from Trader)
    std::deque<EventEntry> status_messages;
    static constexpr size_t MAX_STATUS_MESSAGES = 30;
    uint64_t status_events = 0;

    // Timing
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point last_pnl_sample;
    uint64_t first_event_ts = 0;

    // Splitter state (0.0 - 1.0 ratio)
    float main_split_ratio = 0.6f;      // Left/Right panel split
    float left_upper_height = 385.0f;   // P&L+Chart+Costs+AutoTune fixed height (adjustable)
    float right_events_ratio = 0.6f;    // Events / Status split ratio

    // Symbol config panel state
    std::string selected_symbol;        // Currently selected symbol for config editing
    bool show_symbol_config = false;    // Show the symbol config panel

    // Tuner control state
    uint64_t last_tune_ns = 0;          // Last tuning timestamp from tuner
    uint32_t tune_count = 0;            // Total tuning operations
    bool tuner_connected = false;       // Is tuner process connected

    // Alert banner state
    struct AlertInfo {
        std::string message;
        ImVec4 color;
        bool is_critical;
        bool acknowledged;
        std::chrono::steady_clock::time_point timestamp;

        AlertInfo() : is_critical(false), acknowledged(false) {}
        AlertInfo(const std::string& msg, ImVec4 col, bool critical)
            : message(msg), color(col), is_critical(critical), acknowledged(false)
            , timestamp(std::chrono::steady_clock::now()) {}
    };
    std::deque<AlertInfo> active_alerts;
    static constexpr size_t MAX_ALERTS = 10;

    // Connection state tracking
    uint8_t last_ws_market_status = 2;  // Start as healthy
    uint8_t last_ws_user_status = 2;
    int64_t last_trader_start_time_ns = 0;  // For restart detection

    void add_alert(const std::string& message, bool is_critical, ImVec4 color) {
        // Remove acknowledged alerts if queue is full
        if (active_alerts.size() >= MAX_ALERTS) {
            active_alerts.erase(
                std::remove_if(active_alerts.begin(), active_alerts.end(),
                    [](const AlertInfo& a) { return a.acknowledged; }),
                active_alerts.end()
            );
        }
        // If still full, remove oldest
        if (active_alerts.size() >= MAX_ALERTS) {
            active_alerts.pop_back();
        }
        active_alerts.push_front(AlertInfo(message, color, is_critical));
    }

    void check_connection_alerts(const SharedConfig* config) {
        if (!config) return;

        uint8_t ws_market = config->get_ws_market_status();
        uint8_t ws_user = config->get_ws_user_status();
        int64_t start_time = config->get_trader_start_time_ns();

        // Check for Trader restart
        if (last_trader_start_time_ns != 0 && start_time != 0 &&
            start_time != last_trader_start_time_ns) {
            add_alert("Trader Engine Restarted - Session recovered",
                      false, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));  // Blue/info
        }
        last_trader_start_time_ns = start_time;

        // Check market data WebSocket status changes
        if (ws_market != last_ws_market_status) {
            if (ws_market == 0 && last_ws_market_status == 2) {
                // Healthy -> Disconnected
                add_alert("CONNECTION LOST - Reconnecting...",
                          true, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));  // Red
            } else if (ws_market == 1 && last_ws_market_status == 2) {
                // Healthy -> Degraded
                add_alert("Connection Degraded - No data received",
                          true, ImVec4(1.0f, 0.6f, 0.0f, 1.0f));  // Orange
            } else if (ws_market == 2 && last_ws_market_status < 2) {
                // Reconnected
                add_alert("Connection Restored",
                          false, ImVec4(0.2f, 1.0f, 0.2f, 1.0f));  // Green
            }
            last_ws_market_status = ws_market;
        }
    }

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

        // Update cash for correct equity calculation
        current_cash = state->cash();
        initial_cash = state->initial_cash();

        // Calculate profit/loss totals
        if (realized_pnl >= 0) {
            total_profit = realized_pnl;
        } else {
            total_loss = std::abs(realized_pnl);
        }

        // Load trading costs
        total_commissions = state->total_commissions();
        total_spread_cost = state->total_spread_cost();
        total_slippage = state->total_slippage();
        total_volume = state->total_volume();

        // Load positions
        int active_slots = 0;
        int loaded_positions = 0;
        for (size_t i = 0; i < MAX_PORTFOLIO_SYMBOLS; ++i) {
            const auto& slot = state->positions[i];
            if (!slot.active.load()) continue;
            active_slots++;

            std::string sym(slot.symbol);
            if (sym.empty()) {
                std::cout << "[DEBUG] Slot " << i << " active but empty symbol\n";
                continue;
            }

            double qty = slot.quantity();
            double avg = slot.avg_price();
            double last = slot.last_price();

            std::cout << "[DEBUG] Slot " << i << ": " << sym
                      << " qty=" << qty << " avg=$" << avg << " last=$" << last << "\n";

            Position& pos = positions[sym];
            pos.symbol = sym;
            pos.quantity = qty;
            pos.avg_entry_price = avg;
            pos.last_price = last;
            pos.realized_pnl = slot.realized_pnl();
            pos.trades = slot.buy_count.load() + slot.sell_count.load();

            // Recalculate total_cost from avg_entry and qty
            pos.total_cost = pos.quantity * pos.avg_entry_price;
            loaded_positions++;
        }

        std::cout << "[IPC] Loaded: active_slots=" << active_slots
                  << ", loaded_positions=" << loaded_positions
                  << ", fills=" << fills << ", realized_pnl=$" << realized_pnl << "\n";
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

    // Total market value of all positions (qty * last_price)
    double total_market_value() const {
        double total = 0;
        for (const auto& [sym, pos] : positions) {
            total += pos.market_value();
        }
        return total;
    }

    // Equity = cash + position market value
    double total_equity() const {
        return current_cash + total_market_value();
    }

    // Total P&L from initial capital = current equity - initial
    double total_pnl() const {
        return total_equity() - initial_cash;
    }

    double total_pnl_pct() const {
        if (initial_cash <= 0) return 0;
        return (total_pnl() / initial_cash) * 100.0;
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

                // NOTE: Don't modify positions here - positions come from shared memory
                // Events are for logging only to avoid double-counting
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
                double pnl = e.pnl;
                total_profit += pnl;

                // NOTE: Don't modify positions here - use shared memory for position state
                // realized_pnl comes from shared memory, not event accumulation

                snprintf(buf, sizeof(buf), "%6.1fs  TARGET %s  %.0f @ $%.4f  +$%.2f",
                    rel_sec, symbol.c_str(), e.quantity, e.price, pnl);
                // Bright green for profit
                events.push_front({buf, ImVec4(0.2f, 1.0f, 0.2f, 1.0f), rel_sec});
                break;
            }
            case EventType::StopLoss: {
                stops++;
                double pnl = e.pnl;
                total_loss += std::abs(pnl);

                // NOTE: Don't modify positions here - use shared memory for position state

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
            case EventType::Status: {
                status_events++;
                StatusCode code = e.get_status_code();
                const char* code_name = TradeEvent::status_code_name(code);

                // Format based on status type
                if (e.price > 0) {
                    snprintf(buf, sizeof(buf), "%6.1fs  %s  %s  $%.2f",
                        rel_sec, code_name, symbol.c_str(), e.price);
                } else {
                    snprintf(buf, sizeof(buf), "%6.1fs  %s  %s",
                        rel_sec, code_name, symbol.c_str());
                }

                // Color based on status type
                ImVec4 color;
                if (code == StatusCode::Heartbeat) {
                    color = ImVec4(0.5f, 0.5f, 0.5f, 0.7f);  // Dim gray
                } else if (code == StatusCode::AutoTuneRelaxed) {
                    color = ImVec4(0.2f, 1.0f, 0.2f, 1.0f);  // Bright green - good news
                } else if (code == StatusCode::IndicatorsWarmup) {
                    color = ImVec4(0.6f, 0.6f, 0.8f, 1.0f);  // Blue-gray - info
                } else if (code == StatusCode::AutoTuneCooldown ||
                           code == StatusCode::AutoTuneSignal ||
                           code == StatusCode::AutoTuneMinTrade) {
                    color = ImVec4(1.0f, 0.8f, 0.2f, 1.0f);  // Orange - warning
                } else if (code == StatusCode::AutoTunePaused ||
                           code == StatusCode::DrawdownAlert ||
                           code == StatusCode::VolatilitySpike ||
                           code == StatusCode::CashLow) {
                    color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);  // Red - alert!
                } else {
                    color = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);  // Light gray - default
                }

                status_messages.push_front({buf, color, rel_sec});
                if (status_messages.size() > MAX_STATUS_MESSAGES) {
                    status_messages.pop_back();
                }
                break;
            }
            case EventType::TunerConfig: {
                // AI Tuner config change event
                StatusCode code = e.get_status_code();
                const char* code_name = TradeEvent::status_code_name(code);
                uint8_t confidence = e.signal_strength;  // Reused for AI confidence

                snprintf(buf, sizeof(buf), "%6.1fs  %s  %s  [%d%% conf]",
                    rel_sec, code_name, symbol.c_str(), confidence);

                // Purple color for AI events
                ImVec4 color;
                if (code == StatusCode::TunerEmergencyExit) {
                    color = ImVec4(1.0f, 0.2f, 0.2f, 1.0f);  // Red for emergency
                } else if (code == StatusCode::TunerPauseSymbol) {
                    color = ImVec4(1.0f, 0.6f, 0.0f, 1.0f);  // Orange for pause
                } else {
                    color = ImVec4(0.8f, 0.4f, 1.0f, 1.0f);  // Purple for AI
                }

                // Add to main events (visible in LIVE EVENTS panel)
                events.push_front({buf, color, rel_sec});
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

void render_dashboard(DashboardData& data, const SharedPortfolioState* portfolio_state, SharedConfig* config, SharedPaperConfig* paper_config, SharedSymbolConfigs* symbol_configs) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - data.start_time).count();
    int hours = elapsed / 3600;
    int mins = (elapsed % 3600) / 60;
    int secs = elapsed % 60;

    // Display format settings from config (or defaults)
    int price_dec = config ? config->get_price_decimals() : 4;
    int money_dec = config ? config->get_money_decimals() : 2;
    int qty_dec = config ? config->get_qty_decimals() : 4;

    // Clamp to reasonable values
    price_dec = std::clamp(price_dec, 0, 8);
    money_dec = std::clamp(money_dec, 0, 4);
    qty_dec = std::clamp(qty_dec, 0, 8);

    // Sample P&L for chart
    data.sample_pnl();

    // Check for connection alerts
    data.check_connection_alerts(config);

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("Trader Dashboard", nullptr, flags);

    // =========================================================================
    // Header
    // =========================================================================
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
    ImGui::Text("TRADING DASHBOARD");
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

    // =========================================================================
    // Alert Banner (flashing for critical alerts)
    // =========================================================================
    if (!data.active_alerts.empty()) {
        auto& alert = data.active_alerts.front();
        if (!alert.acknowledged) {
            // Flash effect for critical alerts (toggle every 0.5 seconds)
            bool flash = alert.is_critical &&
                (static_cast<int>(ImGui::GetTime() * 2.0f) % 2 == 0);

            ImVec4 bg_color = flash ?
                ImVec4(1.0f, 0.1f, 0.1f, 0.9f) :  // Bright red flash
                ImVec4(alert.color.x * 0.3f, alert.color.y * 0.3f, alert.color.z * 0.3f, 0.9f);

            ImGui::PushStyleColor(ImGuiCol_ChildBg, bg_color);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));

            ImGui::BeginChild("AlertBanner", ImVec2(0, 35), true);

            // Alert icon
            if (alert.is_critical) {
                ImGui::Text("!");
            } else {
                ImGui::Text("i");
            }
            ImGui::SameLine();

            // Alert message
            ImGui::SetWindowFontScale(1.1f);
            ImGui::Text("%s", alert.message.c_str());
            ImGui::SetWindowFontScale(1.0f);

            // Acknowledge button (right-aligned)
            float btn_width = 100;
            ImGui::SameLine(ImGui::GetWindowWidth() - btn_width - 15);
            if (ImGui::Button("Acknowledge", ImVec2(btn_width, 0))) {
                alert.acknowledged = true;
            }

            ImGui::EndChild();
            ImGui::PopStyleColor(2);
        }
    }

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
    // Account Info Row (inline, no scroll)
    // =========================================================================
    if (portfolio_state) {
        ImGui::Spacing();

        // Calculate invested (cost basis) and market value separately
        double invested = 0;      // What we actually paid (qty * avg_price)
        double market_val = 0;    // Current worth (qty * last_price)
        for (size_t i = 0; i < MAX_PORTFOLIO_SYMBOLS; ++i) {
            const auto& slot = portfolio_state->positions[i];
            if (slot.active.load() && slot.quantity() > 0) {
                invested += slot.quantity() * slot.avg_price();   // Cost basis
                market_val += slot.market_value();                 // Current value
            }
        }

        double total_equity = portfolio_state->cash() + market_val;
        double pnl_pct = portfolio_state->initial_cash() > 0
            ? ((total_equity / portfolio_state->initial_cash()) - 1.0) * 100.0
            : 0.0;
        ImVec4 equity_color = total_equity >= portfolio_state->initial_cash()
            ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f)
            : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);

        // Single line display
        ImGui::Text("Initial:");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
        ImGui::Text("$%.0f", portfolio_state->initial_cash());
        ImGui::PopStyleColor();

        ImGui::SameLine(0, 30);
        ImGui::Text("Cash:");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.9f, 0.4f, 1.0f));
        ImGui::Text("$%.0f", portfolio_state->cash());
        ImGui::PopStyleColor();

        ImGui::SameLine(0, 30);
        ImGui::Text("Invested:");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.8f, 0.2f, 1.0f));
        ImGui::Text("$%.0f", invested);
        ImGui::PopStyleColor();

        ImGui::SameLine(0, 30);
        ImGui::Text("Equity:");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, equity_color);
        ImGui::Text("$%.0f", total_equity);
        ImGui::PopStyleColor();

        ImGui::SameLine(0, 30);
        ImGui::Text("P&L:");
        ImGui::SameLine();
        double pnl_dollars = total_equity - portfolio_state->initial_cash();
        ImGui::PushStyleColor(ImGuiCol_Text, equity_color);
        ImGui::Text("%s$%.2f (%s%.2f%%)",
            pnl_dollars >= 0 ? "+" : "", pnl_dollars,
            pnl_pct >= 0 ? "+" : "", pnl_pct);
        ImGui::PopStyleColor();

        ImGui::Spacing();
    }

    ImGui::Separator();

    // =========================================================================
    // Main Content - Two columns with resizable splitter
    // =========================================================================
    float available_width = ImGui::GetContentRegionAvail().x;
    float available_height = ImGui::GetContentRegionAvail().y;

    // Calculate sizes from ratio
    float left_width = available_width * data.main_split_ratio;
    float right_width = available_width - left_width - 8.0f;  // 8px for splitter

    // Draw left panel
    float left_panel_height = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("LeftPanel", ImVec2(left_width, 0), false);

    // Clamp upper height to valid range
    data.left_upper_height = std::clamp(data.left_upper_height, 150.0f, left_panel_height - 100.0f);
    float positions_height = left_panel_height - data.left_upper_height - 8.0f;  // 8px for splitter

    // Upper section (P&L, Chart, Costs, AutoTune)
    ImGui::BeginChild("LeftUpper", ImVec2(0, data.left_upper_height), false);

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
    ImGui::BeginChild("Chart", ImVec2(0, 150), true);
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
            0, nullptr, -range, range, ImVec2(-1, 90));

        ImGui::Text("-%.2f", range);
    } else {
        ImGui::Text("Waiting for data...");
    }

    ImGui::EndChild();

    // Trading Costs Section
    ImGui::BeginChild("Costs", ImVec2(0, 70), true);
    ImGui::Text("TRADING COSTS");
    ImGui::Separator();

    ImGui::Columns(5, "costs_cols", false);

    // Commission
    ImGui::Text("Commission");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
    ImGui::Text("$%.2f", data.total_commissions);
    ImGui::PopStyleColor();
    ImGui::NextColumn();

    // Spread Cost
    ImGui::Text("Spread");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
    ImGui::Text("$%.2f", data.total_spread_cost);
    ImGui::PopStyleColor();
    ImGui::NextColumn();

    // Slippage Cost
    ImGui::Text("Slippage");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.8f, 1.0f));  // Pink/magenta
    ImGui::Text("$%.2f", data.total_slippage);
    ImGui::PopStyleColor();
    ImGui::NextColumn();

    // Total Costs (only commission affects cash - slippage/spread are informational)
    ImGui::Text("Actual Cost");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
    ImGui::Text("$%.2f", data.total_commissions);  // Only commission is actual cost
    ImGui::PopStyleColor();
    ImGui::NextColumn();

    // Volume
    ImGui::Text("Volume");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
    if (data.total_volume >= 1000000) {
        ImGui::Text("$%.2fM", data.total_volume / 1000000.0);
    } else if (data.total_volume >= 1000) {
        ImGui::Text("$%.1fK", data.total_volume / 1000.0);
    } else {
        ImGui::Text("$%.0f", data.total_volume);
    }
    ImGui::PopStyleColor();

    ImGui::Columns(1);
    ImGui::EndChild();

    // P&L Reconciliation Section
    ImGui::BeginChild("PnLRecon", ImVec2(0, 55), true);
    ImGui::Text("P&L RECONCILIATION");
    ImGui::Separator();

    // Calculate both P&L methods
    double equity_pnl = data.total_pnl();  // cash + market_value - initial (THE TRUTH)
    double component_pnl = data.realized_pnl + data.total_unrealized_pnl() - data.total_commissions;
    double difference = equity_pnl - component_pnl;

    ImGui::Columns(4, "recon_cols", false);

    // Equity P&L (truth)
    ImGui::Text("Equity P&L");
    ImVec4 eq_color = equity_pnl >= 0 ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f) : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, eq_color);
    ImGui::Text("$%.2f", equity_pnl);
    ImGui::PopStyleColor();
    ImGui::NextColumn();

    // Component P&L
    ImGui::Text("R+U-C");
    ImVec4 comp_color = component_pnl >= 0 ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f) : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, comp_color);
    ImGui::Text("$%.2f", component_pnl);
    ImGui::PopStyleColor();
    ImGui::NextColumn();

    // Difference
    ImGui::Text("Diff");
    ImVec4 diff_color = std::abs(difference) < 1.0 ? ImVec4(0.5f, 0.5f, 0.5f, 1.0f) : ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, diff_color);
    ImGui::Text("$%.2f", difference);
    ImGui::PopStyleColor();
    ImGui::NextColumn();

    // Note about slippage
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Slip in R/U");

    ImGui::Columns(1);
    ImGui::EndChild();

    // Auto-Tune & Trade Filtering Section
    ImGui::BeginChild("AutoTune", ImVec2(0, 65), true);
    ImGui::Text("AUTO-TUNE & FILTERING");
    ImGui::Separator();

    ImGui::Columns(6, "tune_cols", false);

    // Tuner Status
    ImGui::Text("Tuner");
    bool tuner_on = config && config->is_tuner_on();
    ImGui::PushStyleColor(ImGuiCol_Text, tuner_on ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
    ImGui::Text("%s", tuner_on ? "ON" : "OFF");
    ImGui::PopStyleColor();
    ImGui::NextColumn();

    // Win Streak
    ImGui::Text("Win Streak");
    int32_t wins = config ? config->get_consecutive_wins() : 0;
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.2f, 1.0f));
    ImGui::Text("%d", wins);
    ImGui::PopStyleColor();
    ImGui::NextColumn();

    // Loss Streak
    ImGui::Text("Loss Streak");
    int32_t losses = config ? config->get_consecutive_losses() : 0;
    ImVec4 loss_color = losses >= 5 ? ImVec4(1.0f, 0.0f, 0.0f, 1.0f) :
                        losses >= 3 ? ImVec4(1.0f, 0.5f, 0.0f, 1.0f) :
                        losses >= 2 ? ImVec4(1.0f, 0.8f, 0.0f, 1.0f) :
                                      ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, loss_color);
    ImGui::Text("%d", losses);
    ImGui::PopStyleColor();
    ImGui::NextColumn();

    // Cost Per Trade
    ImGui::Text("Cost/Trade");
    double total_costs = data.total_commissions + data.total_spread_cost + data.total_slippage;
    double cost_per_trade = data.fills > 0 ? total_costs / data.fills : 0.0;
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
    ImGui::Text("$%.3f", cost_per_trade);
    ImGui::PopStyleColor();
    ImGui::NextColumn();

    // Cooldown
    ImGui::Text("Cooldown");
    int32_t cooldown = config ? config->get_cooldown_ms() : 0;
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
    ImGui::Text("%dms", cooldown);
    ImGui::PopStyleColor();
    ImGui::NextColumn();

    // Min Trade Value
    ImGui::Text("Min Trade");
    double min_trade = config ? config->min_trade_value() : 0.0;
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
    ImGui::Text("$%.0f", min_trade);
    ImGui::PopStyleColor();

    ImGui::Columns(1);
    ImGui::EndChild();

    ImGui::EndChild();  // LeftUpper

    // Horizontal splitter bar (resizes vertically)
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.4f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.5f, 0.5f, 0.7f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.6f, 0.8f, 1.0f));
    ImGui::Button("##LeftSplitter", ImVec2(-1, 8.0f));
    if (ImGui::IsItemActive()) {
        float delta = ImGui::GetIO().MouseDelta.y;
        data.left_upper_height += delta;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    ImGui::PopStyleColor(3);

    // Positions Table
    ImGui::BeginChild("Positions", ImVec2(0, positions_height), true);
    ImGui::Text("ACTIVE POSITIONS");
    ImGui::Separator();

    if (ImGui::BeginTable("positions_table", 9,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {

        ImGui::TableSetupColumn("Symbol", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Regime", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("Strategy", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Qty", ImGuiTableColumnFlags_WidthFixed, 85);
        ImGui::TableSetupColumn("Entry", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Last", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("P&L", ImGuiTableColumnFlags_WidthFixed, 65);
        ImGui::TableSetupColumn("Chart", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Cfg", ImGuiTableColumnFlags_WidthFixed, 30);
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

            // Strategy (from config mapping or SMART when tuner is on)
            ImGui::TableNextColumn();
            if (config && config->is_tuner_on()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.4f, 1.0f, 1.0f));  // Purple for AI
                ImGui::Text("SMART");
                ImGui::PopStyleColor();
            } else if (config) {
                // Get strategy from config mapping
                int regime_idx = regime_to_index(pos.regime);
                uint8_t strategy = config->get_strategy_for_regime(regime_idx);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
                ImGui::Text("%s", strategy_type_to_display(strategy));
                ImGui::PopStyleColor();
            } else {
                // Fallback when config not available
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
                ImGui::Text("%s", regime_to_strategy_fallback(pos.regime));
                ImGui::PopStyleColor();
            }

            // Quantity (use configurable decimals for crypto fractional amounts)
            ImGui::TableNextColumn();
            char qty_fmt[16];
            snprintf(qty_fmt, sizeof(qty_fmt), "%%.%df", qty_dec);
            ImGui::Text(qty_fmt, pos.quantity);

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

            // Config button
            ImGui::TableNextColumn();
            ImGui::PushID(sym.c_str());
            if (ImGui::SmallButton("C")) {
                data.selected_symbol = sym;
                data.show_symbol_config = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Edit %s config", sym.c_str());
            }
            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    ImGui::EndChild();
    ImGui::EndChild();  // LeftPanel

    // Splitter bar (vertical, resizes horizontally)
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.4f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.5f, 0.5f, 0.7f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.6f, 0.8f, 1.0f));
    ImGui::Button("##MainSplitter", ImVec2(8.0f, available_height));
    if (ImGui::IsItemActive()) {
        float delta = ImGui::GetIO().MouseDelta.x;
        float new_ratio = data.main_split_ratio + delta / available_width;
        data.main_split_ratio = std::clamp(new_ratio, 0.2f, 0.8f);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    ImGui::PopStyleColor(3);

    ImGui::SameLine();

    // Right Panel - Config + Events
    ImGui::BeginChild("RightPanel", ImVec2(right_width, 0), false);

    // Config Panel (collapsible)
    if (config && ImGui::CollapsingHeader("STRATEGY CONFIG", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent(10);

        // ===== TRADER CONNECTION STATUS =====
        {
            const char* status_names[] = { "STOPPED", "STARTING", "RUNNING", "SHUTTING DOWN" };
            const ImVec4 status_colors[] = {
                ImVec4(0.5f, 0.5f, 0.5f, 1.0f),  // STOPPED - gray
                ImVec4(1.0f, 0.8f, 0.2f, 1.0f),  // STARTING - yellow
                ImVec4(0.2f, 1.0f, 0.2f, 1.0f),  // RUNNING - green
                ImVec4(1.0f, 0.5f, 0.0f, 1.0f),  // SHUTTING DOWN - orange
            };

            uint8_t trader_status = config->get_trader_status();
            bool is_alive = config->is_trader_alive(3);  // 3 second timeout
            bool is_paper = config->is_paper_trading();
            bool is_manual = config->is_manual_override();
            bool is_tuner_mode = config->is_tuner_on();

            // Check if Trader is really alive (heartbeat check)
            if (trader_status == 2 && !is_alive) {
                // Trader was running but stopped unexpectedly (crashed or killed -9)
                trader_status = 0;  // Show as stopped
            }

            if (trader_status > 3) trader_status = 0;

            ImGui::Text("Trader:");
            ImGui::SameLine();

            if (trader_status == 2 && is_alive) {
                // Running and responsive - show mode
                if (is_paper) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));  // Cyan for paper
                    ImGui::Text("[PAPER MODE]");
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, status_colors[2]);  // Green for live
                    ImGui::Text("[LIVE MODE]");
                }
                ImGui::PopStyleColor();

                // Show override/tuner mode indicators
                ImGui::SameLine();
                if (is_manual) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.0f, 1.0f));  // Orange
                    ImGui::Text("[MANUAL]");
                    ImGui::PopStyleColor();
                } else if (is_tuner_mode) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.4f, 1.0f, 1.0f));  // Purple
                    ImGui::Text("[AI TUNER]");
                    ImGui::PopStyleColor();
                }

                ImGui::SameLine();
                ImGui::TextDisabled("(%.8s)", config->get_build_hash());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Git commit: %.8s\nPID: %d\nMode: %s\nTuner: %s\nManual: %s",
                                      config->get_build_hash(),
                                      config->get_trader_pid(),
                                      is_paper ? "Paper Trading" : "Live Trading",
                                      is_tuner_mode ? "ON" : "OFF",
                                      is_manual ? "ON" : "OFF");
                }
            } else if (trader_status == 0) {
                // Explicitly stopped
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                ImGui::Text("[STOPPED]");
                ImGui::PopStyleColor();
            } else if (!is_alive) {
                // Was running but heartbeat stale (crashed?)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                ImGui::Text("[NO HEARTBEAT]");
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Trader engine is not responding.\nLast known PID: %d\nPossible crash or kill -9.",
                                      config->get_trader_pid());
                }
            } else {
                // Starting or shutting down
                ImGui::PushStyleColor(ImGuiCol_Text, status_colors[trader_status]);
                ImGui::Text("[%s]", status_names[trader_status]);
                ImGui::PopStyleColor();
            }

            // WebSocket connection status
            ImGui::SameLine();
            ImGui::Text("  WS:");
            ImGui::SameLine();

            uint8_t ws_status = config->get_ws_market_status();
            const char* ws_status_names[] = { "DISCONNECTED", "DEGRADED", "CONNECTED" };
            const ImVec4 ws_status_colors[] = {
                ImVec4(1.0f, 0.2f, 0.2f, 1.0f),  // Disconnected - red
                ImVec4(1.0f, 0.6f, 0.0f, 1.0f),  // Degraded - orange
                ImVec4(0.2f, 1.0f, 0.2f, 1.0f),  // Connected - green
            };

            if (ws_status > 2) ws_status = 0;
            ImGui::PushStyleColor(ImGuiCol_Text, ws_status_colors[ws_status]);
            ImGui::Text("[%s]", ws_status_names[ws_status]);
            ImGui::PopStyleColor();

            // Show reconnect count if any
            uint32_t reconnect_count = config->get_ws_reconnect_count();
            if (reconnect_count > 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("(R:%u)", reconnect_count);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Total reconnection attempts: %u", reconnect_count);
                }
            }
        }

        ImGui::Separator();

        // ===== LIVE STATUS (from Trader) =====
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

            // Win/Loss streak (clamp to reasonable values to avoid garbage display)
            int32_t wins = std::clamp(config->get_consecutive_wins(), 0, 999);
            int32_t losses = std::clamp(config->get_consecutive_losses(), 0, 999);
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

        // ===== REGIME → STRATEGY MAPPING =====
        if (ImGui::TreeNode("Regime Strategy Mapping")) {
            ImGui::TextDisabled("Strategy selection per market regime");

            // Strategy names for dropdown
            const char* strategy_names[] = { "NONE", "MOMENTUM", "MEAN_REV", "MKT_MAKER", "DEFENSIVE", "CAUTIOUS", "SMART" };
            const char* regime_names[] = { "Unknown", "TrendingUp", "TrendingDown", "Ranging", "HighVol", "LowVol", "Spike" };

            // Show mapping table
            if (ImGui::BeginTable("RegimeStrategyTable", 2, ImGuiTableFlags_BordersInner | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Regime", ImGuiTableColumnFlags_WidthFixed, 100);
                ImGui::TableSetupColumn("Strategy", ImGuiTableColumnFlags_WidthFixed, 120);
                ImGui::TableHeadersRow();

                for (int i = 0; i <= 6; ++i) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();

                    // Regime name with color
                    ImVec4 regime_color;
                    switch (i) {
                        case 1: regime_color = ImVec4(0.2f, 1.0f, 0.2f, 1.0f); break;  // TrendingUp - green
                        case 2: regime_color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f); break;  // TrendingDown - red
                        case 3: regime_color = ImVec4(0.6f, 0.8f, 1.0f, 1.0f); break;  // Ranging - light blue
                        case 4: regime_color = ImVec4(1.0f, 0.5f, 0.0f, 1.0f); break;  // HighVol - orange
                        case 5: regime_color = ImVec4(0.5f, 0.5f, 0.8f, 1.0f); break;  // LowVol - purple
                        case 6: regime_color = ImVec4(1.0f, 0.0f, 0.5f, 1.0f); break;  // Spike - pink
                        default: regime_color = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);        // Unknown - gray
                    }
                    ImGui::PushStyleColor(ImGuiCol_Text, regime_color);
                    ImGui::Text("%s", regime_names[i]);
                    ImGui::PopStyleColor();

                    // Strategy dropdown
                    ImGui::TableNextColumn();
                    int current_strategy = config->get_strategy_for_regime(i);
                    ImGui::PushID(i);
                    ImGui::SetNextItemWidth(100);
                    if (ImGui::Combo("##strategy", &current_strategy, strategy_names, 7)) {
                        config->set_strategy_for_regime(i, static_cast<uint8_t>(current_strategy));
                    }
                    ImGui::PopID();
                }

                ImGui::EndTable();
            }

            // Quick presets
            ImGui::Spacing();
            if (ImGui::Button("Conservative")) {
                config->set_strategy_for_regime(0, 0);  // Unknown → NONE
                config->set_strategy_for_regime(1, 5);  // TrendingUp → CAUTIOUS
                config->set_strategy_for_regime(2, 4);  // TrendingDown → DEFENSIVE
                config->set_strategy_for_regime(3, 3);  // Ranging → MKT_MAKER
                config->set_strategy_for_regime(4, 0);  // HighVol → NONE
                config->set_strategy_for_regime(5, 3);  // LowVol → MKT_MAKER
                config->set_strategy_for_regime(6, 0);  // Spike → NONE
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Safe preset: No trading in unknown/volatile/spike regimes");
            }
            ImGui::SameLine();
            if (ImGui::Button("Balanced")) {
                config->set_strategy_for_regime(0, 0);  // Unknown → NONE
                config->set_strategy_for_regime(1, 1);  // TrendingUp → MOMENTUM
                config->set_strategy_for_regime(2, 4);  // TrendingDown → DEFENSIVE
                config->set_strategy_for_regime(3, 3);  // Ranging → MKT_MAKER
                config->set_strategy_for_regime(4, 5);  // HighVol → CAUTIOUS
                config->set_strategy_for_regime(5, 3);  // LowVol → MKT_MAKER
                config->set_strategy_for_regime(6, 0);  // Spike → NONE
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Default preset: Momentum in trends, MM in ranging");
            }
            ImGui::SameLine();
            if (ImGui::Button("Aggressive")) {
                config->set_strategy_for_regime(0, 5);  // Unknown → CAUTIOUS
                config->set_strategy_for_regime(1, 1);  // TrendingUp → MOMENTUM
                config->set_strategy_for_regime(2, 2);  // TrendingDown → MEAN_REV
                config->set_strategy_for_regime(3, 3);  // Ranging → MKT_MAKER
                config->set_strategy_for_regime(4, 5);  // HighVol → CAUTIOUS
                config->set_strategy_for_regime(5, 1);  // LowVol → MOMENTUM
                config->set_strategy_for_regime(6, 4);  // Spike → DEFENSIVE
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Active preset: Trade in all regimes, mean-rev on downtrends");
            }

            ImGui::TreePop();
        }

        // ===== RISK MANAGEMENT (Global - No Override) =====
        if (ImGui::TreeNode("Risk Management")) {
            ImGui::TextDisabled("Portfolio protection (no symbol override)");

            // Spread multiplier
            float spread_mult = config->spread_multiplier();
            ImGui::SetNextItemWidth(100);
            if (ImGui::InputFloat("Spread Mult", &spread_mult, 0.1f, 0.5f, "%.1fx")) {
                spread_mult = std::clamp(spread_mult, 0.1f, 100.0f);
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

            // Drawdown threshold (stored as percentage, e.g., 2 = 2%)
            float drawdown = config->drawdown_threshold();
            ImGui::SetNextItemWidth(100);
            if (ImGui::InputFloat("Max Drawdown", &drawdown, 0.5f, 1.0f, "%.1f%%")) {
                drawdown = std::clamp(drawdown, 0.1f, 50.0f);
                config->set_drawdown_threshold(drawdown);
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
            ImGui::SetNextItemWidth(100);
            if (ImGui::InputInt("Loss Streak", &loss_streak, 1, 2)) {
                loss_streak = std::clamp(loss_streak, 1, 100);
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

        // ===== DEFAULT PARAMETERS (Symbols can override) =====
        if (ImGui::TreeNode("Default Parameters")) {
            ImGui::TextDisabled("Symbols can override these via symbol config");
            ImGui::Spacing();

            // --- Position Sizing ---
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Position Sizing");

            // Base position (stored as percentage, e.g., 2 = 2%)
            float base_pos = config->base_position_pct();
            ImGui::SetNextItemWidth(100);
            if (ImGui::InputFloat("Base Size", &base_pos, 0.5f, 1.0f, "%.1f%%")) {
                base_pos = std::clamp(base_pos, 0.1f, 50.0f);
                config->set_base_position_pct(base_pos);
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

            // Max position (stored as percentage, e.g., 5 = 5%)
            float max_pos = config->max_position_pct();
            ImGui::SetNextItemWidth(100);
            if (ImGui::InputFloat("Max Size", &max_pos, 0.5f, 1.0f, "%.1f%%")) {
                max_pos = std::clamp(max_pos, 0.1f, 100.0f);
                config->set_max_position_pct(max_pos);
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

            ImGui::Spacing();
            ImGui::Separator();

            // --- Target / Stop ---
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Target / Stop");

            // Target (stored as percentage, e.g., 1.5 = 1.5%)
            float target = config->target_pct();
            ImGui::SetNextItemWidth(100);
            if (ImGui::InputFloat("Target %%", &target, 0.1f, 0.5f, "%.2f%%")) {
                target = std::clamp(target, 0.1f, 50.0f);
                config->set_target_pct(target);
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

            // Stop (stored as percentage, e.g., 1.0 = 1%)
            float stop = config->stop_pct();
            ImGui::SetNextItemWidth(100);
            if (ImGui::InputFloat("Stop %%", &stop, 0.1f, 0.5f, "%.2f%%")) {
                stop = std::clamp(stop, 0.1f, 50.0f);
                config->set_stop_pct(stop);
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

            // Pullback (Trend Exit) - stored as percentage, e.g., 0.5 = 0.5%
            float pullback = config->pullback_pct();
            ImGui::SetNextItemWidth(100);
            if (ImGui::InputFloat("Pullback %%", &pullback, 0.1f, 0.25f, "%.2f%%")) {
                pullback = std::clamp(pullback, 0.1f, 10.0f);
                config->set_pullback_pct(pullback);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "TREND EXIT (Pullback from Peak)\n\n"
                    "When in profit, sell if price drops\n"
                    "this much from its highest point.\n\n"
                    "0.25%%: Very sensitive, quick exits\n"
                    "0.5%%: Normal (recommended)\n"
                    "1-2%%: Lets winners run longer\n\n"
                    "Example: Entry=$100, Peak=$105\n"
                    "0.5%% pullback = sell at $104.48");
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

            ImGui::Spacing();
            ImGui::Separator();

            // --- Trade Filtering ---
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Trade Filtering");

            // Tuner Paused toggle
            bool tuner_paused = config->is_tuner_paused();
            if (ImGui::Checkbox("Pause Tuner", &tuner_paused)) {
                config->set_tuner_state(tuner_paused ? TunerState::PAUSED : TunerState::ON);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "TUNER PAUSE\n\n"
                    "PAUSED: ConfigStrategy runs with frozen config\n"
                    "        Tuner does NOT make changes\n"
                    "ON:     AI tuning active, parameters updated");
            }

            // Cooldown
            int cooldown = config->get_cooldown_ms();
            ImGui::SetNextItemWidth(100);
            if (ImGui::InputInt("Cooldown (ms)", &cooldown, 100, 500)) {
                cooldown = std::clamp(cooldown, 0, 60000);
                config->set_cooldown_ms(cooldown);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "TRADE COOLDOWN\n\n"
                    "Minimum time between trades.\n"
                    "Prevents overtrading.\n\n"
                    "500ms: Aggressive scalping\n"
                    "2000ms: Normal (recommended)\n"
                    "5000ms: Conservative\n\n"
                    "Higher = fewer trades, lower costs");
            }

            // Min Trade Value
            float min_trade = static_cast<float>(config->min_trade_value());
            ImGui::SetNextItemWidth(100);
            if (ImGui::InputFloat("Min Trade $", &min_trade, 10.0f, 50.0f, "%.0f")) {
                min_trade = std::clamp(min_trade, 1.0f, 10000.0f);
                config->set_min_trade_value(static_cast<double>(min_trade));
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "MINIMUM TRADE SIZE\n\n"
                    "Trades smaller than this are skipped.\n"
                    "Prevents tiny trades that cost more\n"
                    "in fees than they could profit.\n\n"
                    "$50: Aggressive\n"
                    "$100: Normal (recommended)\n"
                    "$200: Conservative");
            }

            // Signal Strength
            int sig_strength = config->get_signal_strength();
            ImGui::SetNextItemWidth(100);
            if (ImGui::InputInt("Signal Level", &sig_strength, 1, 1)) {
                sig_strength = std::clamp(sig_strength, 1, 3);
                config->set_signal_strength(sig_strength);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "SIGNAL STRENGTH FILTER\n\n"
                    "1 = Medium signals (more trades)\n"
                    "2 = Strong signals only (recommended)\n"
                    "3 = Very strong only (few trades)\n\n"
                    "Higher = fewer but higher quality trades");
            }

            ImGui::Spacing();
            ImGui::Separator();

            // --- EMA Thresholds ---
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "EMA Thresholds");

            // EMA deviation for trending regime
            float ema_trending = static_cast<float>(config->ema_dev_trending());
            ImGui::SetNextItemWidth(100);
            if (ImGui::InputFloat("Trending %%", &ema_trending, 0.1f, 0.5f, "%.2f%%")) {
                ema_trending = std::clamp(ema_trending, 0.1f, 10.0f);
                config->set_ema_dev_trending(static_cast<double>(ema_trending));
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "EMA DEVIATION - TRENDING REGIME\n\n"
                    "Max %% price can be above EMA\n"
                    "and still buy in uptrend.\n\n"
                    "0.5%%: Very strict, near EMA only\n"
                    "1.0%%: Normal (recommended)\n"
                    "2.0%%: Loose, chase trends more\n\n"
                    "Lower = safer entries, fewer trades");
            }

            // EMA deviation for ranging regime
            float ema_ranging = static_cast<float>(config->ema_dev_ranging());
            ImGui::SetNextItemWidth(100);
            if (ImGui::InputFloat("Ranging %%", &ema_ranging, 0.1f, 0.5f, "%.2f%%")) {
                ema_ranging = std::clamp(ema_ranging, 0.1f, 5.0f);
                config->set_ema_dev_ranging(static_cast<double>(ema_ranging));
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "EMA DEVIATION - RANGING REGIME\n\n"
                    "Max %% price can be above EMA\n"
                    "and still buy in sideways market.\n\n"
                    "0.25%%: Very tight, scalping\n"
                    "0.5%%: Normal (recommended)\n"
                    "1.0%%: Loose, more entries\n\n"
                    "Lower = trade closer to mean");
            }

            // EMA deviation for high volatility regime
            float ema_highvol = static_cast<float>(config->ema_dev_highvol());
            ImGui::SetNextItemWidth(100);
            if (ImGui::InputFloat("High Vol %%", &ema_highvol, 0.05f, 0.1f, "%.2f%%")) {
                ema_highvol = std::clamp(ema_highvol, 0.05f, 2.0f);
                config->set_ema_dev_highvol(static_cast<double>(ema_highvol));
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "EMA DEVIATION - HIGH VOLATILITY\n\n"
                    "Max %% price can be above EMA\n"
                    "in volatile/uncertain markets.\n\n"
                    "0.1%%: Very strict, near EMA only\n"
                    "0.2%%: Normal (recommended)\n"
                    "0.5%%: Loose, riskier entries\n\n"
                    "Lower = more conservative in chaos");
            }

            ImGui::TextDisabled("Lower values = stricter entries");

            ImGui::TreePop();
        }

        // ===== PAPER TRADING COSTS (Default Open) - Only shown in paper mode =====
        if (config->is_paper_trading() &&
            ImGui::TreeNodeEx("Paper Trading Costs", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("Slippage and commission simulation");

            // Commission Rate
            float commission = static_cast<float>(config->commission_rate() * 100);  // Convert to %
            ImGui::SetNextItemWidth(100);
            if (ImGui::InputFloat("Commission %", &commission, 0.01f, 0.05f, "%.3f")) {
                commission = std::clamp(commission, 0.0f, 1.0f);
                config->set_commission_rate(commission / 100.0);  // Convert back to decimal
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "COMMISSION RATE (per trade)\n\n"
                    "Applied to both entry and exit.\n"
                    "Round-trip cost = 2x this value.\n\n"
                    "0.05%% = 5 bps (low-fee exchange)\n"
                    "0.10%% = 10 bps (typical crypto)\n"
                    "0.25%% = 25 bps (high-fee exchange)\n\n"
                    "Lower = more realistic paper profits");
            }

            // Slippage (uses SharedPaperConfig if available, fallback to SharedConfig)
            float slippage = paper_config ? static_cast<float>(paper_config->slippage_bps())
                                          : static_cast<float>(config->slippage_bps());
            ImGui::SetNextItemWidth(100);
            if (ImGui::InputFloat("Slippage (bps)", &slippage, 1.0f, 5.0f, "%.1f")) {
                slippage = std::clamp(slippage, 0.0f, 100.0f);
                if (paper_config) {
                    paper_config->set_slippage_bps(slippage);
                } else {
                    config->set_slippage_bps(slippage);  // Fallback (deprecated)
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "SLIPPAGE SIMULATION (basis points)\n\n"
                    "Simulates market impact and execution delay.\n"
                    "Applied to both entry and exit prices.\n\n"
                    "0 bps = Perfect fills (unrealistic)\n"
                    "5 bps = Light slippage\n"
                    "10 bps = Normal market conditions\n"
                    "25+ bps = High volatility/low liquidity\n\n"
                    "1 bps = 0.01%%\n"
                    "Higher = more conservative P&L estimate");
            }

            // Round-trip cost calculation
            float rt_commission = commission * 2;
            float rt_slippage = slippage * 2 / 100.0f;  // Convert bps to %
            float rt_total = rt_commission + rt_slippage;
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                "Round-trip: %.3f%%", rt_total);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "TOTAL ROUND-TRIP COST\n\n"
                    "Commission: 2 x %.3f%% = %.3f%%\n"
                    "Slippage:   2 x %.1f bps = %.3f%%\n"
                    "─────────────────────────\n"
                    "TOTAL: %.3f%%\n\n"
                    "Your target profit must exceed this\n"
                    "to be profitable!",
                    commission, rt_commission,
                    slippage, rt_slippage,
                    rt_total);
            }

            ImGui::TreePop();
        }

        ImGui::Unindent(10);
        ImGui::Spacing();
    }

    // ===== TUNER CONTROL PANEL =====
    if (config && symbol_configs && ImGui::CollapsingHeader("TUNER CONTROL", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent(10);

        // Update tuner state from shared memory
        data.tuner_connected = symbol_configs->tuner_connected.load() != 0;
        data.tune_count = symbol_configs->tune_count.load();
        data.last_tune_ns = symbol_configs->last_tune_ns.load();

        // Status line
        if (data.tuner_connected) {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "[*] TUNER CONNECTED");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "[ ] TUNER NOT CONNECTED");
        }

        // Last tune time
        if (data.last_tune_ns > 0) {
            uint64_t now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
            uint64_t age_sec = (now_ns - data.last_tune_ns) / 1'000'000'000ULL;
            ImGui::SameLine();
            ImGui::TextDisabled("Last tune: %llus ago", (unsigned long long)age_sec);
        }

        ImGui::TextDisabled("Total tunes: %u", data.tune_count);

        ImGui::Separator();

        // AI Tuner Mode toggle (enable/disable)
        bool tuner_on = config->is_tuner_on() || config->is_tuner_paused();
        if (ImGui::Checkbox("AI Tuner Enabled", &tuner_on)) {
            config->set_tuner_state(tuner_on ? TunerState::ON : TunerState::OFF);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "AI TUNER MODE\n\n"
                "OFF: Traditional strategies\n"
                "     (TechIndicators, MarketMaker, etc.)\n"
                "ON:  AI-controlled unified strategy\n"
                "     Parameters tuned by Claude\n\n"
                "Requires: trader_tuner running");
        }

        // Pause Tuner checkbox (only visible when tuner is enabled)
        if (tuner_on) {
            ImGui::SameLine();
            bool tuner_paused = config->is_tuner_paused();
            if (tuner_paused) {
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.6f, 0.3f, 0.0f, 0.8f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.8f, 0.4f, 0.0f, 0.8f));
                ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1.0f, 0.6f, 0.0f, 1.0f));
            }
            if (ImGui::Checkbox("Paused", &tuner_paused)) {
                config->set_tuner_state(tuner_paused ? TunerState::PAUSED : TunerState::ON);
            }
            if (tuner_paused) {
                ImGui::PopStyleColor(3);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "PAUSE/RESUME TUNER\n\n"
                    "When paused:\n"
                    "- Scheduled tuning is skipped\n"
                    "- Manual trigger still works\n"
                    "- Useful for testing manual configs\n\n"
                    "Resume to let AI optimize again");
            }
        }

        // Force Tune button (only when tuner is enabled)
        if (tuner_on) {
            ImGui::SameLine();
        }
        if (ImGui::Button("Force Tune")) {
            config->request_manual_tune();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "FORCE IMMEDIATE TUNING\n\n"
                "Triggers the AI tuner to run now,\n"
                "regardless of scheduled interval.\n\n"
                "Use when:\n"
                "- Market conditions changed suddenly\n"
                "- You want to test new base config\n"
                "- After manual parameter changes");
        }

        // Manual tune request indicator
        if (config->should_tune_now()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "(pending...)");
        }

        ImGui::Unindent(10);
        ImGui::Spacing();
    }

    // Calculate remaining height for Events + Status
    float right_remaining = ImGui::GetContentRegionAvail().y;
    float events_height = right_remaining * data.right_events_ratio;
    float status_height = right_remaining - events_height - 8.0f;  // 8px for splitter

    // Clamp to valid ranges
    events_height = std::max(events_height, 80.0f);
    status_height = std::max(status_height, 60.0f);

    // Events Panel
    ImGui::BeginChild("Events", ImVec2(0, events_height), true);
    ImGui::Text("LIVE EVENTS");
    ImGui::Separator();

    for (const auto& ev : data.events) {
        ImGui::PushStyleColor(ImGuiCol_Text, ev.color);
        ImGui::Text("%s", ev.text.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::EndChild();

    // Horizontal splitter bar (resizes vertically)
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.4f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.5f, 0.5f, 0.7f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.6f, 0.8f, 1.0f));
    ImGui::Button("##RightSplitter", ImVec2(-1, 8.0f));
    if (ImGui::IsItemActive()) {
        float delta = ImGui::GetIO().MouseDelta.y;
        float new_ratio = data.right_events_ratio + delta / right_remaining;
        data.right_events_ratio = std::clamp(new_ratio, 0.2f, 0.8f);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    ImGui::PopStyleColor(3);

    // ===== STATUS MESSAGES (Trader debug/info) =====
    ImGui::BeginChild("StatusMessages", ImVec2(0, status_height), true);
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.8f, 1.0f), "TRADER STATUS");
    ImGui::SameLine();
    ImGui::TextDisabled("(%llu msgs)", (unsigned long long)data.status_events);
    ImGui::Separator();

    // Show most recent status messages
    for (const auto& msg : data.status_messages) {
        ImGui::PushStyleColor(ImGuiCol_Text, msg.color);
        ImGui::Text("%s", msg.text.c_str());
        ImGui::PopStyleColor();
    }

    if (data.status_messages.empty()) {
        ImGui::TextDisabled("Waiting for Trader status...");
    }

    ImGui::EndChild();

    ImGui::EndChild();  // RightPanel

    ImGui::End();

    // ===== SYMBOL CONFIG POPUP WINDOW =====
    if (data.show_symbol_config && symbol_configs && !data.selected_symbol.empty()) {
        ImGui::SetNextWindowSize(ImVec2(350, 450), ImGuiCond_FirstUseEver);
        char window_title[64];
        snprintf(window_title, sizeof(window_title), "Config: %s###SymbolConfig", data.selected_symbol.c_str());

        if (ImGui::Begin(window_title, &data.show_symbol_config, ImGuiWindowFlags_NoCollapse)) {
            // Get or create symbol config
            SymbolTuningConfig* cfg = symbol_configs->get_or_create(data.selected_symbol.c_str());

            if (cfg) {
                // Enabled checkbox
                bool enabled = cfg->is_enabled();
                if (ImGui::Checkbox("Trading Enabled", &enabled)) {
                    cfg->enabled = enabled ? 1 : 0;
                    symbol_configs->sequence.fetch_add(1);
                }

                ImGui::Separator();

                // === EMA Thresholds ===
                if (ImGui::TreeNodeEx("EMA Deviation Thresholds", ImGuiTreeNodeFlags_DefaultOpen)) {
                    // Use Global checkbox
                    bool use_global_ema = cfg->use_global_ema();
                    if (ImGui::Checkbox("Use Global##ema", &use_global_ema)) {
                        cfg->set_use_global_ema(use_global_ema);
                        symbol_configs->sequence.fetch_add(1);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Use global EMA thresholds from Default Parameters");
                    }

                    // Get values (global or symbol-specific)
                    float ema_trend = use_global_ema ?
                        static_cast<float>(config->ema_dev_trending()) :
                        cfg->ema_dev_trending_x100 / 100.0f;
                    float ema_range = use_global_ema ?
                        static_cast<float>(config->ema_dev_ranging()) :
                        cfg->ema_dev_ranging_x100 / 100.0f;
                    float ema_hvol = use_global_ema ?
                        static_cast<float>(config->ema_dev_highvol()) :
                        cfg->ema_dev_highvol_x100 / 100.0f;

                    if (use_global_ema) ImGui::BeginDisabled();

                    ImGui::SetNextItemWidth(100);
                    if (ImGui::InputFloat("Trending %", &ema_trend, 0.1f, 0.5f, "%.2f")) {
                        if (!use_global_ema) {
                            ema_trend = std::clamp(ema_trend, 0.1f, 10.0f);
                            cfg->ema_dev_trending_x100 = static_cast<int16_t>(ema_trend * 100);
                            symbol_configs->sequence.fetch_add(1);
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", use_global_ema ? "(global)" : "(custom)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Max %% above EMA to allow buy in trending market");
                    }

                    ImGui::SetNextItemWidth(100);
                    if (ImGui::InputFloat("Ranging %", &ema_range, 0.1f, 0.5f, "%.2f")) {
                        if (!use_global_ema) {
                            ema_range = std::clamp(ema_range, 0.1f, 10.0f);
                            cfg->ema_dev_ranging_x100 = static_cast<int16_t>(ema_range * 100);
                            symbol_configs->sequence.fetch_add(1);
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", use_global_ema ? "(global)" : "(custom)");

                    ImGui::SetNextItemWidth(100);
                    if (ImGui::InputFloat("High Vol %", &ema_hvol, 0.1f, 0.5f, "%.2f")) {
                        if (!use_global_ema) {
                            ema_hvol = std::clamp(ema_hvol, 0.0f, 10.0f);
                            cfg->ema_dev_highvol_x100 = static_cast<int16_t>(ema_hvol * 100);
                            symbol_configs->sequence.fetch_add(1);
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", use_global_ema ? "(global)" : "(custom)");

                    if (use_global_ema) ImGui::EndDisabled();

                    ImGui::TreePop();
                }

                // === Position Sizing ===
                if (ImGui::TreeNodeEx("Position Sizing", ImGuiTreeNodeFlags_DefaultOpen)) {
                    // Use Global checkbox
                    bool use_global_pos = cfg->use_global_position();
                    if (ImGui::Checkbox("Use Global##pos", &use_global_pos)) {
                        cfg->set_use_global_position(use_global_pos);
                        symbol_configs->sequence.fetch_add(1);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Use global position sizing from Default Parameters");
                    }

                    // Get values (global or symbol-specific)
                    float base_pos = use_global_pos ?
                        static_cast<float>(config->base_position_pct()) :
                        cfg->base_position_x100 / 100.0f;
                    float max_pos = use_global_pos ?
                        static_cast<float>(config->max_position_pct()) :
                        cfg->max_position_x100 / 100.0f;

                    if (use_global_pos) ImGui::BeginDisabled();

                    ImGui::SetNextItemWidth(100);
                    if (ImGui::InputFloat("Base %", &base_pos, 0.5f, 1.0f, "%.1f")) {
                        if (!use_global_pos) {
                            base_pos = std::clamp(base_pos, 0.1f, 20.0f);
                            cfg->base_position_x100 = static_cast<int16_t>(base_pos * 100);
                            symbol_configs->sequence.fetch_add(1);
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", use_global_pos ? "(global)" : "(custom)");

                    ImGui::SetNextItemWidth(100);
                    if (ImGui::InputFloat("Max %", &max_pos, 0.5f, 1.0f, "%.1f")) {
                        if (!use_global_pos) {
                            max_pos = std::clamp(max_pos, 0.1f, 50.0f);
                            cfg->max_position_x100 = static_cast<int16_t>(max_pos * 100);
                            symbol_configs->sequence.fetch_add(1);
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", use_global_pos ? "(global)" : "(custom)");

                    if (use_global_pos) ImGui::EndDisabled();

                    ImGui::TreePop();
                }

                // === Target/Stop ===
                if (ImGui::TreeNodeEx("Target/Stop", ImGuiTreeNodeFlags_DefaultOpen)) {
                    // Use Global checkbox
                    bool use_global_target = cfg->use_global_target();
                    if (ImGui::Checkbox("Use Global##target", &use_global_target)) {
                        cfg->set_use_global_target(use_global_target);
                        symbol_configs->sequence.fetch_add(1);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Use global target/stop from Default Parameters");
                    }

                    // Get values (global or symbol-specific)
                    float target = use_global_target ?
                        static_cast<float>(config->target_pct()) :
                        cfg->target_pct_x100 / 100.0f;
                    float stop = use_global_target ?
                        static_cast<float>(config->stop_pct()) :
                        cfg->stop_pct_x100 / 100.0f;
                    float pullback = use_global_target ?
                        static_cast<float>(config->pullback_pct()) :
                        cfg->pullback_pct_x100 / 100.0f;

                    if (use_global_target) ImGui::BeginDisabled();

                    ImGui::SetNextItemWidth(100);
                    if (ImGui::InputFloat("Target %", &target, 0.5f, 1.0f, "%.2f")) {
                        if (!use_global_target) {
                            target = std::clamp(target, 0.1f, 20.0f);
                            cfg->target_pct_x100 = static_cast<int16_t>(target * 100);
                            symbol_configs->sequence.fetch_add(1);
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", use_global_target ? "(global)" : "(custom)");

                    ImGui::SetNextItemWidth(100);
                    if (ImGui::InputFloat("Stop %", &stop, 0.1f, 0.5f, "%.2f")) {
                        if (!use_global_target) {
                            stop = std::clamp(stop, 0.1f, 20.0f);
                            cfg->stop_pct_x100 = static_cast<int16_t>(stop * 100);
                            symbol_configs->sequence.fetch_add(1);
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", use_global_target ? "(global)" : "(custom)");

                    ImGui::SetNextItemWidth(100);
                    if (ImGui::InputFloat("Pullback %", &pullback, 0.1f, 0.5f, "%.2f")) {
                        if (!use_global_target) {
                            pullback = std::clamp(pullback, 0.0f, 10.0f);
                            cfg->pullback_pct_x100 = static_cast<int16_t>(pullback * 100);
                            symbol_configs->sequence.fetch_add(1);
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", use_global_target ? "(global)" : "(custom)");

                    if (use_global_target) ImGui::EndDisabled();

                    ImGui::TreePop();
                }

                // === Trade Filtering ===
                if (ImGui::TreeNodeEx("Trade Filtering", ImGuiTreeNodeFlags_DefaultOpen)) {
                    // Use Global checkbox
                    bool use_global_filter = cfg->use_global_filtering();
                    if (ImGui::Checkbox("Use Global##filter", &use_global_filter)) {
                        cfg->set_use_global_filtering(use_global_filter);
                        symbol_configs->sequence.fetch_add(1);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Use global trade filtering from Default Parameters");
                    }

                    // Get values (global or symbol-specific)
                    int cooldown = use_global_filter ?
                        config->get_cooldown_ms() :
                        static_cast<int>(cfg->cooldown_ms);
                    int sig_str = use_global_filter ?
                        config->get_signal_strength() :
                        static_cast<int>(cfg->signal_strength);

                    if (use_global_filter) ImGui::BeginDisabled();

                    ImGui::SetNextItemWidth(100);
                    if (ImGui::InputInt("Cooldown ms", &cooldown, 100, 500)) {
                        if (!use_global_filter) {
                            cooldown = std::clamp(cooldown, 0, 60000);
                            cfg->cooldown_ms = static_cast<int16_t>(cooldown);
                            symbol_configs->sequence.fetch_add(1);
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", use_global_filter ? "(global)" : "(custom)");

                    ImGui::SetNextItemWidth(100);
                    if (ImGui::InputInt("Signal Lvl", &sig_str, 1, 1)) {
                        if (!use_global_filter) {
                            sig_str = std::clamp(sig_str, 1, 3);
                            cfg->signal_strength = static_cast<int8_t>(sig_str);
                            symbol_configs->sequence.fetch_add(1);
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", use_global_filter ? "(global)" : "(custom)");

                    if (use_global_filter) ImGui::EndDisabled();

                    ImGui::TreePop();
                }

                ImGui::Separator();

                // === Performance Stats (Read-only) ===
                ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Performance");
                ImGui::Text("Trades: %d", cfg->total_trades);
                ImGui::Text("Wins: %d (%.1f%%)", cfg->winning_trades, cfg->win_rate());
                ImGui::Text("Total P&L: $%.2f", cfg->total_pnl_x100 / 100.0);
                ImGui::Text("Avg P&L: $%.2f/trade", cfg->avg_pnl());

                ImGui::Separator();

                // Reset button
                if (ImGui::Button("Reset to Defaults")) {
                    const char* sym = cfg->symbol;
                    cfg->init(sym);
                    symbol_configs->sequence.fetch_add(1);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Reset this symbol to default config values");
                }
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Failed to get symbol config");
            }
        }
        ImGui::End();
    }
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
    GLFWwindow* window = glfwCreateWindow(1400, 900, "Trader Dashboard", nullptr, nullptr);
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
    std::cout << "Connecting to Trader engine...\n";

    // Try to open shared portfolio state (read-only)
    SharedPortfolioState* portfolio_state = SharedPortfolioState::open("/trader_portfolio");
    if (portfolio_state) {
        std::cout << "Found portfolio state (cash=$" << portfolio_state->cash() << ")\n";
    }

    // Try to open shared config (read-write for dashboard control)
    SharedConfig* shared_config = SharedConfig::open_rw("/trader_config");
    if (!shared_config) {
        // Create if doesn't exist
        shared_config = SharedConfig::create("/trader_config");
    }
    if (shared_config) {
        std::cout << "Config connected (spread_mult=" << shared_config->spread_multiplier() << "x)\n";
    }

    // Try to open paper config (for paper-specific settings like slippage)
    SharedPaperConfig* paper_config = SharedPaperConfig::open_rw("/trader_paper_config");
    if (!paper_config) {
        paper_config = SharedPaperConfig::create("/trader_paper_config");
    }
    if (paper_config) {
        std::cout << "Paper config connected (slippage=" << paper_config->slippage_bps() << " bps)\n";
    }

    // Try to open symbol configs (for per-symbol config editing)
    SharedSymbolConfigs* symbol_configs = SharedSymbolConfigs::open_rw("/trader_symbol_configs");
    if (!symbol_configs) {
        // Create if doesn't exist
        symbol_configs = SharedSymbolConfigs::create("/trader_symbol_configs");
    }
    if (symbol_configs) {
        std::cout << "Symbol configs connected (" << symbol_configs->symbol_count.load() << " symbols)\n";
    }

    SharedRingBuffer<TradeEvent>* buffer = nullptr;
    int retries = 0;

    while (!buffer && retries < 30 && g_running) {
        try {
            buffer = new SharedRingBuffer<TradeEvent>("/trader_events", false);
            std::cout << "Connected to event stream!\n";
        } catch (...) {
            retries++;
            std::cout << "Waiting for Trader engine... (" << retries << "/30)\n";
            glfwPollEvents();
            if (glfwWindowShouldClose(window)) {
                g_running = false;
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    if (!buffer) {
        std::cerr << "Could not connect to Trader engine\n";
        if (portfolio_state) munmap(portfolio_state, sizeof(SharedPortfolioState));
        if (shared_config) munmap(shared_config, sizeof(SharedConfig));
        if (paper_config) munmap(paper_config, sizeof(SharedPaperConfig));
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

            // Update global stats from shared memory (source of truth)
            data.fills = portfolio_state->total_fills.load();
            data.targets = portfolio_state->total_targets.load();
            data.stops = portfolio_state->total_stops.load();
            data.realized_pnl = portfolio_state->total_realized_pnl();
            data.winning_trades = portfolio_state->winning_trades.load();
            data.losing_trades = portfolio_state->losing_trades.load();

            // Update cash for correct equity calculation
            data.current_cash = portfolio_state->cash();
            data.initial_cash = portfolio_state->initial_cash();

            // Update profit/loss from realized
            if (data.realized_pnl >= 0) {
                data.total_profit = data.realized_pnl;
                data.total_loss = 0;
            } else {
                data.total_profit = 0;
                data.total_loss = std::abs(data.realized_pnl);
            }

            // Update trading costs from shared memory
            data.total_commissions = portfolio_state->total_commissions();
            data.total_spread_cost = portfolio_state->total_spread_cost();
            data.total_slippage = portfolio_state->total_slippage();
            data.total_volume = portfolio_state->total_volume();

            for (size_t i = 0; i < MAX_PORTFOLIO_SYMBOLS; ++i) {
                const auto& slot = portfolio_state->positions[i];
                if (!slot.active.load()) continue;

                std::string sym(slot.symbol);
                if (sym.empty()) continue;

                double price = slot.last_price();
                if (price > 0) {
                    // Update position from shared memory (source of truth)
                    auto& pos = data.positions[sym];
                    pos.symbol = sym;
                    pos.quantity = slot.quantity();
                    pos.avg_entry_price = slot.avg_price();
                    pos.last_price = price;
                    pos.realized_pnl = slot.realized_pnl();
                    pos.regime = static_cast<MarketRegime>(slot.regime.load());
                    pos.total_cost = pos.quantity * pos.avg_entry_price;
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
        render_dashboard(data, portfolio_state, shared_config, paper_config, symbol_configs);

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
    if (paper_config) {
        munmap(paper_config, sizeof(SharedPaperConfig));
    }
    if (symbol_configs) {
        munmap(symbol_configs, sizeof(SharedSymbolConfigs));
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
