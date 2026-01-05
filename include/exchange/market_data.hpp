#pragma once

#include "../types.hpp"
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace hft {
namespace exchange {

/**
 * OHLCV Candlestick data
 *
 * Binance kline format:
 * [open_time, open, high, low, close, volume, close_time,
 *  quote_volume, trades, taker_buy_base, taker_buy_quote, ignore]
 */
struct Kline {
    Timestamp open_time;      // Candle open time (ms)
    Timestamp close_time;     // Candle close time (ms)
    Price open;
    Price high;
    Price low;
    Price close;
    double volume;            // Base asset volume
    double quote_volume;      // Quote asset volume
    uint32_t trades;          // Number of trades
    double taker_buy_volume;  // Taker buy base volume

    // Helpers
    Price mid() const { return (high + low) / 2; }
    Price range() const { return high - low; }
    bool is_bullish() const { return close > open; }
    bool is_bearish() const { return close < open; }
    double body_ratio() const {
        if (range() == 0) return 0;
        Price body = (close > open) ? (close - open) : (open - close);
        return static_cast<double>(body) / range();
    }
};

/**
 * Market trade tick data (from exchange feed)
 */
struct MarketTrade {
    Timestamp time;
    Price price;
    double quantity;
    bool is_buyer_maker;  // true = sell (taker sold), false = buy (taker bought)
};

/**
 * Order book snapshot (L2)
 */
struct BookSnapshot {
    Timestamp time;
    std::vector<std::pair<Price, double>> bids;  // price, qty
    std::vector<std::pair<Price, double>> asks;  // price, qty
};

/**
 * Load klines from CSV file
 *
 * Expected format:
 * open_time,open,high,low,close,volume,close_time,quote_volume,trades,taker_buy_volume,taker_buy_quote,ignore
 */
inline std::vector<Kline> load_klines_csv(const std::string& filename) {
    std::vector<Kline> klines;
    std::ifstream file(filename);

    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + filename);
    }

    std::string line;
    bool first_line = true;

    while (std::getline(file, line)) {
        // Skip header if present
        if (first_line && line.find("open_time") != std::string::npos) {
            first_line = false;
            continue;
        }
        first_line = false;

        if (line.empty()) continue;

        std::stringstream ss(line);
        std::string token;
        std::vector<std::string> tokens;

        while (std::getline(ss, token, ',')) {
            tokens.push_back(token);
        }

        if (tokens.size() < 10) continue;  // Need at least 10 columns

        Kline k;
        k.open_time = std::stoull(tokens[0]);
        k.open = static_cast<Price>(std::stod(tokens[1]) * 10000);  // 4 decimals
        k.high = static_cast<Price>(std::stod(tokens[2]) * 10000);
        k.low = static_cast<Price>(std::stod(tokens[3]) * 10000);
        k.close = static_cast<Price>(std::stod(tokens[4]) * 10000);
        k.volume = std::stod(tokens[5]);
        k.close_time = std::stoull(tokens[6]);
        k.quote_volume = std::stod(tokens[7]);
        k.trades = static_cast<uint32_t>(std::stoul(tokens[8]));
        k.taker_buy_volume = (tokens.size() > 9) ? std::stod(tokens[9]) : 0;

        klines.push_back(k);
    }

    return klines;
}

/**
 * Save klines to CSV file
 */
inline void save_klines_csv(const std::string& filename, const std::vector<Kline>& klines) {
    std::ofstream file(filename);

    if (!file.is_open()) {
        throw std::runtime_error("Cannot create file: " + filename);
    }

    // Header
    file << "open_time,open,high,low,close,volume,close_time,quote_volume,trades,taker_buy_volume\n";

    for (const auto& k : klines) {
        file << k.open_time << ","
             << (k.open / 10000.0) << ","
             << (k.high / 10000.0) << ","
             << (k.low / 10000.0) << ","
             << (k.close / 10000.0) << ","
             << k.volume << ","
             << k.close_time << ","
             << k.quote_volume << ","
             << k.trades << ","
             << k.taker_buy_volume << "\n";
    }
}

/**
 * Load trades from CSV
 *
 * Format: time,price,quantity,is_buyer_maker
 */
inline std::vector<MarketTrade> load_trades_csv(const std::string& filename) {
    std::vector<MarketTrade> trades;
    std::ifstream file(filename);

    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + filename);
    }

    std::string line;
    bool first_line = true;

    while (std::getline(file, line)) {
        if (first_line && line.find("time") != std::string::npos) {
            first_line = false;
            continue;
        }
        first_line = false;

        if (line.empty()) continue;

        std::stringstream ss(line);
        std::string token;
        std::vector<std::string> tokens;

        while (std::getline(ss, token, ',')) {
            tokens.push_back(token);
        }

        if (tokens.size() < 4) continue;

        MarketTrade t;
        t.time = std::stoull(tokens[0]);
        t.price = static_cast<Price>(std::stod(tokens[1]) * 10000);
        t.quantity = std::stod(tokens[2]);
        t.is_buyer_maker = (tokens[3] == "true" || tokens[3] == "1");

        trades.push_back(t);
    }

    return trades;
}

}  // namespace exchange
}  // namespace hft
