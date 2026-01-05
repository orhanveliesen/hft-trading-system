#pragma once

#include "../market_events.hpp"
#include "../types.hpp"
#include <string_view>
#include <cstdint>
#include <charconv>

namespace hft {
namespace feed {

/**
 * Binance WebSocket Feed Handler
 *
 * Parses Binance WebSocket messages and emits generic market events.
 * Supports:
 *   - @trade: Individual trades
 *   - @depth: Order book depth updates
 *   - @bookTicker: Best bid/ask updates
 *
 * Note: Binance doesn't provide order-level data like ITCH.
 * We emit BookLevelUpdate and Trade events instead.
 *
 * Callback must implement:
 *   void on_trade(const Trade&)
 *   void on_quote(const QuoteUpdate&)
 *   void on_book_level(const BookLevelUpdate&)
 */
template<typename Callback>
class BinanceFeedHandler {
public:
    explicit BinanceFeedHandler(Callback& callback, SymbolId symbol_id = 0)
        : callback_(callback)
        , symbol_id_(symbol_id)
    {}

    void set_symbol_id(SymbolId id) { symbol_id_ = id; }

    /**
     * Process a WebSocket message (JSON string)
     * Returns true if parsed successfully
     */
    bool process_message(std::string_view json) {
        // Find event type
        auto event_pos = json.find("\"e\":");
        if (event_pos == std::string_view::npos) {
            // Might be a depth snapshot or error
            return parse_depth_snapshot(json);
        }

        // Extract event type (simple parsing, no external JSON lib)
        auto type_start = json.find('"', event_pos + 4);
        if (type_start == std::string_view::npos) return false;
        auto type_end = json.find('"', type_start + 1);
        if (type_end == std::string_view::npos) return false;

        std::string_view event_type = json.substr(type_start + 1, type_end - type_start - 1);

        if (event_type == "trade") {
            return parse_trade(json);
        } else if (event_type == "depthUpdate") {
            return parse_depth_update(json);
        } else if (event_type == "bookTicker") {
            return parse_book_ticker(json);
        }

        return true;  // Unknown event type, skip
    }

private:
    Callback& callback_;
    SymbolId symbol_id_;

    // Parse @trade stream
    // {"e":"trade","E":123456789,"s":"BTCUSDT","t":12345,"p":"0.001","q":"100","T":123456785,"m":true}
    bool parse_trade(std::string_view json) {
        Trade event;
        event.symbol_id = symbol_id_;

        // Parse price "p":"..."
        if (!parse_decimal_field(json, "\"p\":\"", event.price)) return false;

        // Parse quantity "q":"..."
        if (!parse_decimal_field(json, "\"q\":\"", event.quantity)) return false;

        // Parse timestamp "T":...
        if (!parse_int_field(json, "\"T\":", event.timestamp)) return false;

        // Parse maker side "m":true/false (true = buyer is maker, so seller aggressed)
        auto m_pos = json.find("\"m\":");
        if (m_pos != std::string_view::npos) {
            event.aggressor_side = (json[m_pos + 4] == 't') ? Side::Sell : Side::Buy;
        }

        callback_.on_trade(event);
        return true;
    }

    // Parse @bookTicker stream
    // {"u":123456,"s":"BTCUSDT","b":"0.0024","B":"10","a":"0.0025","A":"100"}
    bool parse_book_ticker(std::string_view json) {
        QuoteUpdate event;
        event.symbol_id = symbol_id_;
        event.timestamp = 0;  // bookTicker doesn't have timestamp

        // Parse best bid "b":"..."
        if (!parse_decimal_field(json, "\"b\":\"", event.bid_price)) return false;

        // Parse bid size "B":"..."
        if (!parse_decimal_field(json, "\"B\":\"", event.bid_size)) return false;

        // Parse best ask "a":"..."
        if (!parse_decimal_field(json, "\"a\":\"", event.ask_price)) return false;

        // Parse ask size "A":"..."
        if (!parse_decimal_field(json, "\"A\":\"", event.ask_size)) return false;

        callback_.on_quote(event);
        return true;
    }

    // Parse @depth stream update
    // {"e":"depthUpdate","E":123456789,"s":"BTCUSDT","U":157,"u":160,
    //  "b":[["0.0024","10"]],
    //  "a":[["0.0025","100"]]}
    bool parse_depth_update(std::string_view json) {
        Timestamp ts = 0;
        parse_int_field(json, "\"E\":", ts);

        // Parse bids
        parse_book_side(json, "\"b\":", Side::Buy, ts);

        // Parse asks
        parse_book_side(json, "\"a\":", Side::Sell, ts);

        return true;
    }

    // Parse depth snapshot (full order book)
    bool parse_depth_snapshot(std::string_view json) {
        // Check if this looks like a depth snapshot
        if (json.find("\"bids\":") == std::string_view::npos) return false;

        Timestamp ts = 0;
        parse_int_field(json, "\"lastUpdateId\":", ts);

        parse_book_side(json, "\"bids\":", Side::Buy, ts);
        parse_book_side(json, "\"asks\":", Side::Sell, ts);

        return true;
    }

    void parse_book_side(std::string_view json, const char* field, Side side, Timestamp ts) {
        auto pos = json.find(field);
        if (pos == std::string_view::npos) return;

        // Find the array start
        auto arr_start = json.find('[', pos);
        if (arr_start == std::string_view::npos) return;

        // Parse each [price, qty] pair
        size_t i = arr_start + 1;
        while (i < json.size()) {
            // Find next level array
            auto level_start = json.find('[', i);
            if (level_start == std::string_view::npos) break;

            auto level_end = json.find(']', level_start);
            if (level_end == std::string_view::npos) break;

            std::string_view level = json.substr(level_start + 1, level_end - level_start - 1);

            BookLevelUpdate event;
            event.symbol_id = symbol_id_;
            event.side = side;
            event.timestamp = ts;

            // Parse price (first quoted value)
            auto q1 = level.find('"');
            auto q2 = level.find('"', q1 + 1);
            if (q1 != std::string_view::npos && q2 != std::string_view::npos) {
                event.price = parse_price(level.substr(q1 + 1, q2 - q1 - 1));
            }

            // Parse quantity (second quoted value)
            auto q3 = level.find('"', q2 + 1);
            auto q4 = level.find('"', q3 + 1);
            if (q3 != std::string_view::npos && q4 != std::string_view::npos) {
                event.quantity = parse_quantity(level.substr(q3 + 1, q4 - q3 - 1));
            }

            callback_.on_book_level(event);

            i = level_end + 1;
            if (i < json.size() && json[i] == ']') break;  // End of array
        }
    }

    // Helper: parse a decimal string field to Price (multiply by 10000)
    bool parse_decimal_field(std::string_view json, const char* field, Price& out) {
        auto pos = json.find(field);
        if (pos == std::string_view::npos) return false;

        pos += strlen(field);
        auto end = json.find('"', pos);
        if (end == std::string_view::npos) return false;

        out = parse_price(json.substr(pos, end - pos));
        return true;
    }

    bool parse_decimal_field(std::string_view json, const char* field, Quantity& out) {
        auto pos = json.find(field);
        if (pos == std::string_view::npos) return false;

        pos += strlen(field);
        auto end = json.find('"', pos);
        if (end == std::string_view::npos) return false;

        out = parse_quantity(json.substr(pos, end - pos));
        return true;
    }

    // Helper: parse an integer field
    bool parse_int_field(std::string_view json, const char* field, uint64_t& out) {
        auto pos = json.find(field);
        if (pos == std::string_view::npos) return false;

        pos += strlen(field);
        auto result = std::from_chars(json.data() + pos, json.data() + json.size(), out);
        return result.ec == std::errc{};
    }

    // Parse price string "12345.6789" to Price (integer, 4 decimal places)
    Price parse_price(std::string_view str) {
        // Find decimal point
        auto dot = str.find('.');
        if (dot == std::string_view::npos) {
            // No decimal, multiply by 10000
            uint64_t val = 0;
            std::from_chars(str.data(), str.data() + str.size(), val);
            return static_cast<Price>(val * 10000);
        }

        // Parse integer part
        uint64_t int_part = 0;
        std::from_chars(str.data(), str.data() + dot, int_part);

        // Parse fractional part (up to 4 digits)
        std::string_view frac = str.substr(dot + 1);
        uint64_t frac_part = 0;
        size_t frac_len = std::min(frac.size(), size_t(4));
        std::from_chars(frac.data(), frac.data() + frac_len, frac_part);

        // Scale fractional part to 4 decimal places
        for (size_t i = frac_len; i < 4; ++i) {
            frac_part *= 10;
        }

        return static_cast<Price>(int_part * 10000 + frac_part);
    }

    // Parse quantity string to Quantity
    Quantity parse_quantity(std::string_view str) {
        // For simplicity, parse as integer (ignoring decimals)
        // In production, would need proper decimal handling
        auto dot = str.find('.');
        uint64_t val = 0;
        if (dot != std::string_view::npos) {
            std::from_chars(str.data(), str.data() + dot, val);
        } else {
            std::from_chars(str.data(), str.data() + str.size(), val);
        }
        return static_cast<Quantity>(val);
    }
};

}  // namespace feed
}  // namespace hft
