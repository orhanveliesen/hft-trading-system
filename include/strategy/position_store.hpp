#pragma once

/**
 * PositionStore - Position persistence for crash recovery
 *
 * Saves portfolio state to JSON file:
 * - On every fill event (immediate persistence)
 * - Periodically (every 5 seconds as backup)
 *
 * On HFT restart, positions can be restored from the file.
 *
 * Usage:
 *   Writer (hft):
 *     PositionStore store("positions.json");
 *     store.save(portfolio_state);  // After each fill
 *
 *   On startup:
 *     if (store.restore(portfolio_state)) {
 *         std::cout << "Restored positions from previous session\n";
 *     }
 *
 *   On graceful shutdown after closing all positions:
 *     store.clear();
 */

#include "../ipc/shared_portfolio_state.hpp"
#include "../util/time_utils.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace hft {
namespace strategy {

class PositionStore {
public:
    static constexpr const char* DEFAULT_PATH = "positions.json";
    static constexpr uint64_t SAVE_INTERVAL_NS = 5'000'000'000ULL; // 5 seconds

    explicit PositionStore(const char* path = DEFAULT_PATH) : path_(path), last_save_ns_(0) {}

    /**
     * Save portfolio state to JSON file
     * Call this after every fill for immediate persistence
     */
    bool save(const ipc::SharedPortfolioState& portfolio) {
        auto now = util::now_ns();

        // Rate limit saves to avoid disk thrashing
        if (last_save_ns_ > 0 && (now - last_save_ns_) < SAVE_INTERVAL_NS) {
            return true; // Skip, too recent
        }

        bool success = write_json(portfolio);
        if (success) {
            last_save_ns_ = now;
        }
        return success;
    }

    /**
     * Force save immediately (for fill events)
     */
    bool save_immediate(const ipc::SharedPortfolioState& portfolio) {
        last_save_ns_ = 0; // Reset rate limit
        return save(portfolio);
    }

    /**
     * Restore portfolio state from JSON file
     * Returns true if restoration was successful
     */
    bool restore(ipc::SharedPortfolioState& portfolio) { return read_json(portfolio); }

    /**
     * Check if a position file exists
     */
    bool exists() const {
        std::ifstream f(path_);
        return f.good();
    }

    /**
     * Clear the position file (on graceful shutdown after closing positions)
     */
    void clear() { std::remove(path_); }

    /**
     * Get path to position file
     */
    const char* path() const { return path_; }

private:
    const char* path_;
    uint64_t last_save_ns_;

    bool write_json(const ipc::SharedPortfolioState& portfolio) {
        // Write to temp file first, then rename (atomic on POSIX)
        std::string temp_path = std::string(path_) + ".tmp";
        std::ofstream out(temp_path);
        if (!out.is_open())
            return false;

        // Write JSON manually (no external JSON library needed)
        out << "{\n";
        out << "  \"version\": 1,\n";
        out << "  \"timestamp_ns\": " << util::now_ns() << ",\n";
        out << "  \"initial_capital\": " << portfolio.initial_cash() << ",\n";
        out << "  \"cash\": " << portfolio.cash() << ",\n";
        out << "  \"total_realized_pnl\": " << portfolio.total_realized_pnl() << ",\n";
        out << "  \"winning_trades\": " << portfolio.winning_trades.load() << ",\n";
        out << "  \"losing_trades\": " << portfolio.losing_trades.load() << ",\n";
        out << "  \"total_fills\": " << portfolio.total_fills.load() << ",\n";
        out << "  \"total_targets\": " << portfolio.total_targets.load() << ",\n";
        out << "  \"total_stops\": " << portfolio.total_stops.load() << ",\n";
        out << "  \"total_commissions\": " << portfolio.total_commissions() << ",\n";
        out << "  \"total_spread_cost\": " << portfolio.total_spread_cost() << ",\n";
        out << "  \"total_slippage\": " << portfolio.total_slippage() << ",\n";
        out << "  \"total_volume\": " << portfolio.total_volume() << ",\n";
        out << "  \"positions\": [\n";

        bool first = true;
        for (size_t i = 0; i < ipc::MAX_PORTFOLIO_SYMBOLS; ++i) {
            const auto& pos = portfolio.positions[i];
            if (!pos.active.load())
                continue;

            double qty = pos.quantity();
            // Only save positions with non-zero quantity
            if (qty == 0)
                continue;

            if (!first)
                out << ",\n";
            first = false;

            out << "    {\n";
            out << "      \"symbol\": \"" << pos.symbol << "\",\n";
            out << "      \"symbol_id\": " << i << ",\n";
            out << "      \"quantity\": " << qty << ",\n";
            out << "      \"avg_price\": " << pos.avg_price() << ",\n";
            out << "      \"last_price\": " << pos.last_price() << ",\n";
            out << "      \"realized_pnl\": " << pos.realized_pnl() << ",\n";
            out << "      \"buy_count\": " << pos.buy_count.load() << ",\n";
            out << "      \"sell_count\": " << pos.sell_count.load() << "\n";
            out << "    }";
        }

        out << "\n  ]\n";
        out << "}\n";

        out.close();

        // Atomic rename
        if (std::rename(temp_path.c_str(), path_) != 0) {
            std::remove(temp_path.c_str());
            return false;
        }

        return true;
    }

    bool read_json(ipc::SharedPortfolioState& portfolio) {
        std::ifstream in(path_);
        if (!in.is_open())
            return false;

        // Read entire file into string
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();

        if (content.empty())
            return false;

        // Simple JSON parsing (minimal, no external library)
        // Parse global fields
        double initial_capital = parse_double(content, "initial_capital");
        double cash = parse_double(content, "cash");
        double total_realized_pnl = parse_double(content, "total_realized_pnl");
        uint32_t winning_trades = static_cast<uint32_t>(parse_double(content, "winning_trades"));
        uint32_t losing_trades = static_cast<uint32_t>(parse_double(content, "losing_trades"));
        uint32_t total_fills = static_cast<uint32_t>(parse_double(content, "total_fills"));
        uint32_t total_targets = static_cast<uint32_t>(parse_double(content, "total_targets"));
        uint32_t total_stops = static_cast<uint32_t>(parse_double(content, "total_stops"));
        double total_commissions = parse_double(content, "total_commissions");
        double total_spread_cost = parse_double(content, "total_spread_cost");
        double total_slippage = parse_double(content, "total_slippage");
        double total_volume = parse_double(content, "total_volume");

        // Set global state
        portfolio.initial_cash_x8.store(static_cast<int64_t>(initial_capital * 1e8));
        portfolio.cash_x8.store(static_cast<int64_t>(cash * 1e8));
        portfolio.total_realized_pnl_x8.store(static_cast<int64_t>(total_realized_pnl * 1e8));
        portfolio.winning_trades.store(winning_trades);
        portfolio.losing_trades.store(losing_trades);
        portfolio.total_fills.store(total_fills);
        portfolio.total_targets.store(total_targets);
        portfolio.total_stops.store(total_stops);
        portfolio.total_commissions_x8.store(static_cast<int64_t>(total_commissions * 1e8));
        portfolio.total_spread_cost_x8.store(static_cast<int64_t>(total_spread_cost * 1e8));
        portfolio.total_slippage_x8.store(static_cast<int64_t>(total_slippage * 1e8));
        portfolio.total_volume_x8.store(static_cast<int64_t>(total_volume * 1e8));

        // Parse positions array
        size_t positions_start = content.find("\"positions\"");
        if (positions_start == std::string::npos)
            return true; // No positions, OK

        size_t array_start = content.find('[', positions_start);
        size_t array_end = content.find(']', array_start);
        if (array_start == std::string::npos || array_end == std::string::npos) {
            return true; // Empty positions array, OK
        }

        std::string positions_str = content.substr(array_start, array_end - array_start + 1);

        // Parse each position object
        size_t pos = 0;
        while ((pos = positions_str.find('{', pos)) != std::string::npos) {
            size_t end = positions_str.find('}', pos);
            if (end == std::string::npos)
                break;

            std::string obj = positions_str.substr(pos, end - pos + 1);

            std::string symbol = parse_string(obj, "symbol");
            size_t symbol_id = static_cast<size_t>(parse_double(obj, "symbol_id"));
            double quantity = parse_double(obj, "quantity");
            double avg_price = parse_double(obj, "avg_price");
            double last_price = parse_double(obj, "last_price");
            double realized_pnl = parse_double(obj, "realized_pnl");
            uint32_t buy_count = static_cast<uint32_t>(parse_double(obj, "buy_count"));
            uint32_t sell_count = static_cast<uint32_t>(parse_double(obj, "sell_count"));

            if (!symbol.empty() && symbol_id < ipc::MAX_PORTFOLIO_SYMBOLS) {
                auto& slot = portfolio.positions[symbol_id];
                std::strncpy(slot.symbol, symbol.c_str(), 15);
                slot.symbol[15] = '\0';
                slot.quantity_x8.store(static_cast<int64_t>(quantity * 1e8));
                slot.avg_price_x8.store(static_cast<int64_t>(avg_price * 1e8));
                slot.last_price_x8.store(static_cast<int64_t>(last_price * 1e8));
                slot.realized_pnl_x8.store(static_cast<int64_t>(realized_pnl * 1e8));
                slot.buy_count.store(buy_count);
                slot.sell_count.store(sell_count);
                slot.active.store(1);
            }

            pos = end + 1;
        }

        // VALIDATION: Recalculate cash from positions to prevent corrupted data
        // Bug discovered: overselling bug could save inflated cash values
        // Cash = initial_capital - sum(position_costs) + realized_pnl - commissions - slippage
        double position_cost = 0.0;
        for (size_t i = 0; i < ipc::MAX_PORTFOLIO_SYMBOLS; ++i) {
            if (portfolio.positions[i].active.load()) {
                double qty = portfolio.positions[i].quantity();
                double price = portfolio.positions[i].avg_price();
                position_cost += qty * price;
            }
        }

        double calculated_cash =
            initial_capital - position_cost + total_realized_pnl - total_commissions - total_slippage;

        // Sanity check: if file cash differs significantly from calculated, use calculated
        double cash_diff = std::abs(cash - calculated_cash);
        if (cash_diff > initial_capital * 0.01) { // More than 1% discrepancy
            // Log warning would be nice, but avoid iostream in hot path compatible code
            // Just use the calculated (correct) value
            portfolio.cash_x8.store(static_cast<int64_t>(calculated_cash * 1e8));
        }

        return true;
    }

    // Simple JSON value parsers (no external library needed)
    static double parse_double(const std::string& json, const char* key) {
        std::string search = std::string("\"") + key + "\":";
        size_t pos = json.find(search);
        if (pos == std::string::npos) {
            search = std::string("\"") + key + "\": ";
            pos = json.find(search);
        }
        if (pos == std::string::npos)
            return 0.0;

        pos = json.find(':', pos) + 1;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
            ++pos;

        size_t end = pos;
        while (end < json.size() && (std::isdigit(json[end]) || json[end] == '.' || json[end] == '-' ||
                                     json[end] == '+' || json[end] == 'e' || json[end] == 'E')) {
            ++end;
        }

        if (end == pos)
            return 0.0;
        return std::stod(json.substr(pos, end - pos));
    }

    static std::string parse_string(const std::string& json, const char* key) {
        std::string search = std::string("\"") + key + "\":";
        size_t pos = json.find(search);
        if (pos == std::string::npos) {
            search = std::string("\"") + key + "\": ";
            pos = json.find(search);
        }
        if (pos == std::string::npos)
            return "";

        pos = json.find(':', pos) + 1;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
            ++pos;

        if (pos >= json.size() || json[pos] != '"')
            return "";
        ++pos; // Skip opening quote

        size_t end = json.find('"', pos);
        if (end == std::string::npos)
            return "";

        return json.substr(pos, end - pos);
    }
};

} // namespace strategy
} // namespace hft
