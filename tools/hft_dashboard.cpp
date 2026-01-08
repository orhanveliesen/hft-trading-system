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

struct Position {
    std::string symbol;
    double quantity = 0;
    double avg_entry_price = 0;
    double total_cost = 0;
    double last_price = 0;
    double realized_pnl = 0;
    int trades = 0;

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

                snprintf(buf, sizeof(buf), "%6.1fs  %s  %s  %.0f @ $%.4f",
                    rel_sec, e.side == 0 ? "BUY " : "SELL",
                    symbol.c_str(), e.quantity, e.price);
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

                snprintf(buf, sizeof(buf), "%6.1fs  TARGET %s  +$%.2f",
                    rel_sec, symbol.c_str(), pnl);
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

                snprintf(buf, sizeof(buf), "%6.1fs  STOP   %s  $%.2f",
                    rel_sec, symbol.c_str(), pnl);
                events.push_front({buf, ImVec4(1.0f, 0.2f, 0.2f, 1.0f), rel_sec});
                break;
            }
            case EventType::Signal: {
                snprintf(buf, sizeof(buf), "%6.1fs  SIGNAL %s  %s",
                    rel_sec, symbol.c_str(), e.side == 0 ? "BUY" : "SELL");
                events.push_front({buf, ImVec4(0.4f, 0.8f, 1.0f, 1.0f), rel_sec});
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

void render_dashboard(DashboardData& data) {
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
    ImGui::SameLine(ImGui::GetWindowWidth() - 250);
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

    if (ImGui::BeginTable("positions_table", 6,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {

        ImGui::TableSetupColumn("Symbol", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Qty", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Entry", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Last", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("P&L", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Chart", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (const auto& [sym, pos] : data.positions) {
            if (pos.quantity <= 0) continue;

            ImGui::TableNextRow();

            // Symbol
            ImGui::TableNextColumn();
            ImGui::Text("%s", sym.c_str());

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

    // Right Panel - Events
    ImGui::BeginChild("RightPanel", ImVec2(0, 0), false);
    ImGui::BeginChild("Events", ImVec2(0, 0), true);
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

    SharedRingBuffer<TradeEvent>* buffer = nullptr;
    int retries = 0;

    while (!buffer && retries < 30 && g_running) {
        try {
            buffer = new SharedRingBuffer<TradeEvent>("/hft_events", false);
            std::cout << "Connected!\n";
        } catch (...) {
            retries++;
            std::cout << "Waiting... (" << retries << "/30)\n";
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

    while (!glfwWindowShouldClose(window) && g_running) {
        glfwPollEvents();

        // Process events from shared memory
        while (buffer->pop(event)) {
            data.process_event(event);
        }

        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Render dashboard
        render_dashboard(data);

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
