#pragma once

#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <memory>
#include <algorithm>

namespace hft {
namespace config {

/**
 * Strategy type enumeration
 */
enum class StrategyType {
    SMA,
    RSI,
    MeanReversion,
    Breakout,
    MACD,
    SimpleMR_HFT,
    Momentum_HFT
};

inline std::string strategy_type_to_string(StrategyType type) {
    switch (type) {
        case StrategyType::SMA: return "sma";
        case StrategyType::RSI: return "rsi";
        case StrategyType::MeanReversion: return "mr";
        case StrategyType::Breakout: return "breakout";
        case StrategyType::MACD: return "macd";
        case StrategyType::SimpleMR_HFT: return "simple_mr";
        case StrategyType::Momentum_HFT: return "momentum";
    }
    return "unknown";
}

inline StrategyType string_to_strategy_type(const std::string& s) {
    if (s == "sma") return StrategyType::SMA;
    if (s == "rsi") return StrategyType::RSI;
    if (s == "mr" || s == "mean_reversion") return StrategyType::MeanReversion;
    if (s == "breakout") return StrategyType::Breakout;
    if (s == "macd") return StrategyType::MACD;
    if (s == "simple_mr") return StrategyType::SimpleMR_HFT;
    if (s == "momentum") return StrategyType::Momentum_HFT;
    throw std::runtime_error("Unknown strategy type: " + s);
}

/**
 * Strategy parameters - union of all strategy params
 */
struct StrategyParams {
    // SMA
    int sma_fast = 10;
    int sma_slow = 30;

    // RSI
    int rsi_period = 14;
    double rsi_oversold = 30.0;
    double rsi_overbought = 70.0;

    // Mean Reversion
    int mr_lookback = 20;
    double mr_std_mult = 2.0;

    // Breakout
    int breakout_lookback = 20;

    // MACD
    int macd_fast = 12;
    int macd_slow = 26;
    int macd_signal = 9;

    // Momentum HFT
    int momentum_lookback = 10;
    int momentum_threshold_bps = 10;
};

/**
 * Symbol-specific configuration
 */
struct SymbolConfig {
    std::string symbol;              // e.g., "BTCUSDT"
    StrategyType strategy;           // Best strategy for this symbol
    StrategyParams params;           // Strategy parameters

    // Risk management (per-symbol overrides)
    double max_position_pct = 0.5;   // Max position size as % of capital
    double stop_loss_pct = 0.03;     // 3% stop loss
    double take_profit_pct = 0.06;   // 6% take profit

    // Performance metrics (from optimization)
    double expected_return = 0.0;
    double win_rate = 0.0;
    double profit_factor = 0.0;
    double max_drawdown = 0.0;
    double sharpe_ratio = 0.0;
};

/**
 * Global trading configuration
 */
struct TradingConfig {
    // Capital allocation
    double initial_capital = 10000.0;
    double fee_rate = 0.001;         // 0.1%
    double slippage = 0.0005;        // 0.05%

    // Global risk limits
    double max_total_exposure = 0.8; // Max 80% capital in positions
    int max_concurrent_positions = 5;
    bool allow_shorting = false;

    // Symbols and their configs
    std::vector<SymbolConfig> symbols;

    // Find config for a symbol
    const SymbolConfig* find_symbol(const std::string& symbol) const {
        for (const auto& cfg : symbols) {
            if (cfg.symbol == symbol) return &cfg;
        }
        return nullptr;
    }
};

/**
 * Simple JSON-like config parser (no external dependencies)
 *
 * Format:
 * {
 *   "initial_capital": 10000,
 *   "fee_rate": 0.001,
 *   "symbols": [
 *     {
 *       "symbol": "BTCUSDT",
 *       "strategy": "mr",
 *       "mr_lookback": 20,
 *       "mr_std_mult": 2.0,
 *       "stop_loss_pct": 0.03
 *     }
 *   ]
 * }
 */
class ConfigParser {
public:
    static TradingConfig load(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            throw std::runtime_error("Cannot open config file: " + filename);
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();

        return parse(content);
    }

    static TradingConfig parse(const std::string& json) {
        TradingConfig config;

        // Parse global settings
        config.initial_capital = parse_double(json, "initial_capital", 10000.0);
        config.fee_rate = parse_double(json, "fee_rate", 0.001);
        config.slippage = parse_double(json, "slippage", 0.0005);
        config.max_total_exposure = parse_double(json, "max_total_exposure", 0.8);
        config.max_concurrent_positions = parse_int(json, "max_concurrent_positions", 5);
        config.allow_shorting = parse_bool(json, "allow_shorting", false);

        // Parse symbols array
        size_t symbols_start = json.find("\"symbols\"");
        if (symbols_start != std::string::npos) {
            size_t arr_start = json.find('[', symbols_start);
            size_t arr_end = find_matching_bracket(json, arr_start);

            if (arr_start != std::string::npos && arr_end != std::string::npos) {
                std::string symbols_json = json.substr(arr_start + 1, arr_end - arr_start - 1);
                parse_symbols(symbols_json, config.symbols);
            }
        }

        return config;
    }

    static void save(const std::string& filename, const TradingConfig& config) {
        std::ofstream file(filename);
        if (!file.is_open()) {
            throw std::runtime_error("Cannot create config file: " + filename);
        }

        file << "{\n";
        file << "  \"initial_capital\": " << config.initial_capital << ",\n";
        file << "  \"fee_rate\": " << config.fee_rate << ",\n";
        file << "  \"slippage\": " << config.slippage << ",\n";
        file << "  \"max_total_exposure\": " << config.max_total_exposure << ",\n";
        file << "  \"max_concurrent_positions\": " << config.max_concurrent_positions << ",\n";
        file << "  \"allow_shorting\": " << (config.allow_shorting ? "true" : "false") << ",\n";
        file << "  \"symbols\": [\n";

        for (size_t i = 0; i < config.symbols.size(); ++i) {
            const auto& sym = config.symbols[i];
            file << "    {\n";
            file << "      \"symbol\": \"" << sym.symbol << "\",\n";
            file << "      \"strategy\": \"" << strategy_type_to_string(sym.strategy) << "\",\n";

            // Write strategy-specific params
            switch (sym.strategy) {
                case StrategyType::SMA:
                    file << "      \"sma_fast\": " << sym.params.sma_fast << ",\n";
                    file << "      \"sma_slow\": " << sym.params.sma_slow << ",\n";
                    break;
                case StrategyType::RSI:
                    file << "      \"rsi_period\": " << sym.params.rsi_period << ",\n";
                    file << "      \"rsi_oversold\": " << sym.params.rsi_oversold << ",\n";
                    file << "      \"rsi_overbought\": " << sym.params.rsi_overbought << ",\n";
                    break;
                case StrategyType::MeanReversion:
                    file << "      \"mr_lookback\": " << sym.params.mr_lookback << ",\n";
                    file << "      \"mr_std_mult\": " << sym.params.mr_std_mult << ",\n";
                    break;
                case StrategyType::Breakout:
                    file << "      \"breakout_lookback\": " << sym.params.breakout_lookback << ",\n";
                    break;
                case StrategyType::MACD:
                    file << "      \"macd_fast\": " << sym.params.macd_fast << ",\n";
                    file << "      \"macd_slow\": " << sym.params.macd_slow << ",\n";
                    file << "      \"macd_signal\": " << sym.params.macd_signal << ",\n";
                    break;
                case StrategyType::Momentum_HFT:
                    file << "      \"momentum_lookback\": " << sym.params.momentum_lookback << ",\n";
                    file << "      \"momentum_threshold_bps\": " << sym.params.momentum_threshold_bps << ",\n";
                    break;
                default:
                    break;
            }

            file << "      \"max_position_pct\": " << sym.max_position_pct << ",\n";
            file << "      \"stop_loss_pct\": " << sym.stop_loss_pct << ",\n";
            file << "      \"take_profit_pct\": " << sym.take_profit_pct << ",\n";
            file << "      \"expected_return\": " << sym.expected_return << ",\n";
            file << "      \"win_rate\": " << sym.win_rate << ",\n";
            file << "      \"profit_factor\": " << sym.profit_factor << ",\n";
            file << "      \"max_drawdown\": " << sym.max_drawdown << ",\n";
            file << "      \"sharpe_ratio\": " << sym.sharpe_ratio << "\n";
            file << "    }" << (i < config.symbols.size() - 1 ? "," : "") << "\n";
        }

        file << "  ]\n";
        file << "}\n";
    }

private:
    static double parse_double(const std::string& json, const std::string& key, double def) {
        std::string search = "\"" + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return def;

        pos = json.find(':', pos);
        if (pos == std::string::npos) return def;

        size_t start = pos + 1;
        while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) ++start;

        size_t end = start;
        while (end < json.size() && (isdigit(json[end]) || json[end] == '.' || json[end] == '-')) ++end;

        if (start == end) return def;
        return std::stod(json.substr(start, end - start));
    }

    static int parse_int(const std::string& json, const std::string& key, int def) {
        return static_cast<int>(parse_double(json, key, def));
    }

    static bool parse_bool(const std::string& json, const std::string& key, bool def) {
        std::string search = "\"" + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return def;

        pos = json.find(':', pos);
        if (pos == std::string::npos) return def;

        if (json.find("true", pos) < json.find(',', pos)) return true;
        if (json.find("false", pos) < json.find(',', pos)) return false;
        return def;
    }

    static std::string parse_string(const std::string& json, const std::string& key, const std::string& def) {
        std::string search = "\"" + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return def;

        pos = json.find(':', pos);
        if (pos == std::string::npos) return def;

        size_t start = json.find('"', pos + 1);
        if (start == std::string::npos) return def;

        size_t end = json.find('"', start + 1);
        if (end == std::string::npos) return def;

        return json.substr(start + 1, end - start - 1);
    }

    static size_t find_matching_bracket(const std::string& json, size_t start) {
        if (start >= json.size()) return std::string::npos;

        char open = json[start];
        char close = (open == '[') ? ']' : '}';
        int depth = 1;

        for (size_t i = start + 1; i < json.size(); ++i) {
            if (json[i] == open) ++depth;
            else if (json[i] == close) {
                --depth;
                if (depth == 0) return i;
            }
        }
        return std::string::npos;
    }

    static void parse_symbols(const std::string& json, std::vector<SymbolConfig>& symbols) {
        size_t pos = 0;
        while (pos < json.size()) {
            size_t obj_start = json.find('{', pos);
            if (obj_start == std::string::npos) break;

            size_t obj_end = find_matching_bracket(json, obj_start);
            if (obj_end == std::string::npos) break;

            std::string obj = json.substr(obj_start, obj_end - obj_start + 1);
            SymbolConfig sym = parse_symbol_config(obj);
            symbols.push_back(sym);

            pos = obj_end + 1;
        }
    }

    static SymbolConfig parse_symbol_config(const std::string& json) {
        SymbolConfig cfg;

        cfg.symbol = parse_string(json, "symbol", "BTCUSDT");
        std::string strategy_str = parse_string(json, "strategy", "mr");
        cfg.strategy = string_to_strategy_type(strategy_str);

        // Strategy params
        cfg.params.sma_fast = parse_int(json, "sma_fast", 10);
        cfg.params.sma_slow = parse_int(json, "sma_slow", 30);
        cfg.params.rsi_period = parse_int(json, "rsi_period", 14);
        cfg.params.rsi_oversold = parse_double(json, "rsi_oversold", 30.0);
        cfg.params.rsi_overbought = parse_double(json, "rsi_overbought", 70.0);
        cfg.params.mr_lookback = parse_int(json, "mr_lookback", 20);
        cfg.params.mr_std_mult = parse_double(json, "mr_std_mult", 2.0);
        cfg.params.breakout_lookback = parse_int(json, "breakout_lookback", 20);
        cfg.params.macd_fast = parse_int(json, "macd_fast", 12);
        cfg.params.macd_slow = parse_int(json, "macd_slow", 26);
        cfg.params.macd_signal = parse_int(json, "macd_signal", 9);
        cfg.params.momentum_lookback = parse_int(json, "momentum_lookback", 10);
        cfg.params.momentum_threshold_bps = parse_int(json, "momentum_threshold_bps", 10);

        // Risk params
        cfg.max_position_pct = parse_double(json, "max_position_pct", 0.5);
        cfg.stop_loss_pct = parse_double(json, "stop_loss_pct", 0.03);
        cfg.take_profit_pct = parse_double(json, "take_profit_pct", 0.06);

        // Performance metrics
        cfg.expected_return = parse_double(json, "expected_return", 0.0);
        cfg.win_rate = parse_double(json, "win_rate", 0.0);
        cfg.profit_factor = parse_double(json, "profit_factor", 0.0);
        cfg.max_drawdown = parse_double(json, "max_drawdown", 0.0);
        cfg.sharpe_ratio = parse_double(json, "sharpe_ratio", 0.0);

        return cfg;
    }
};

}  // namespace config
}  // namespace hft
