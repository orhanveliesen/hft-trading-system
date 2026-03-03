#pragma once

#include "../types.hpp"

#include <string>

namespace hft {
namespace exchange {

/**
 * Funding Rate data (futures-specific)
 *
 * Binance futures funding rate format from /fapi/v1/premiumIndex
 */
struct FundingRate {
    std::string symbol;
    double funding_rate = 0.0;
    double mark_price = 0.0;
    Timestamp funding_time = 0; // Next funding timestamp
    Timestamp event_time = 0;
};

/**
 * Mark Price Update (futures-specific)
 *
 * Binance futures @markPrice@1s stream
 * Note: Uses double for prices to avoid overflow (BTC at 430K+ exceeds uint32_t * 10000)
 * estimated_settle field omitted (only for delivery futures, not USD-M perpetuals)
 */
struct MarkPriceUpdate {
    std::string symbol;
    double mark_price = 0.0;
    double index_price = 0.0;
    double funding_rate = 0.0;
    Timestamp next_funding_time = 0;
    Timestamp event_time = 0;
};

/**
 * Open Interest data (futures-specific)
 *
 * Binance futures /fapi/v1/openInterest
 */
struct OpenInterest {
    std::string symbol;
    double open_interest = 0.0;       // Total open interest in contracts
    double open_interest_value = 0.0; // Notional value in quote asset
    Timestamp time = 0;
};

/**
 * Liquidation Order (futures-specific)
 *
 * Binance futures @forceOrder stream
 * Uses double for price to prevent overflow (BTC 430K+)
 */
struct LiquidationOrder {
    std::string symbol;
    Side side = Side::Buy;
    double price = 0.0;
    double quantity = 0.0;
    double avg_price = 0.0;
    std::string order_status;
    Timestamp trade_time = 0;
    Timestamp event_time = 0;
};

/**
 * Futures Book Ticker (best bid/ask)
 *
 * Binance futures @bookTicker stream
 * Difference from spot BookTicker: futures has transaction_time + event_time fields
 * Uses double for prices to prevent overflow (BTC 430K+)
 */
struct FuturesBookTicker {
    std::string symbol;
    double bid_price = 0.0;
    double bid_qty = 0.0;
    double ask_price = 0.0;
    double ask_qty = 0.0;
    Timestamp transaction_time = 0;
    Timestamp event_time = 0;
};

} // namespace exchange
} // namespace hft
