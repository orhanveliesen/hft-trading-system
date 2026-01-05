/**
 * HFT C API Implementation
 *
 * Provides C-compatible wrappers around the C++ HFT library.
 */

#include "hft_c_api.h"
#include "orderbook.hpp"
#include "matching_engine.hpp"
#include "strategy/market_maker.hpp"

#include <memory>
#include <cstdlib>

// Version info
static const char* HFT_VERSION = "0.1.0";

// ============================================
// Internal wrapper structures
// ============================================

struct HftOrderBook {
    std::unique_ptr<hft::OrderBook> impl;
};

struct HftMatchingEngine {
    std::unique_ptr<hft::MatchingEngine> impl;
    HftTradeCallback callback;
    void* user_data;
};

struct HftMarketMaker {
    std::unique_ptr<hft::strategy::MarketMaker> impl;
};

// ============================================
// OrderBook API Implementation
// ============================================

extern "C" {

HftOrderBookHandle hft_orderbook_create(HftPrice base_price, size_t price_range) {
    try {
        auto handle = new HftOrderBook();
        handle->impl = std::make_unique<hft::OrderBook>(base_price, price_range);
        return handle;
    } catch (...) {
        return nullptr;
    }
}

void hft_orderbook_destroy(HftOrderBookHandle book) {
    delete book;
}

bool hft_orderbook_add_order(
    HftOrderBookHandle book,
    HftOrderId order_id,
    HftSide side,
    HftPrice price,
    HftQuantity quantity
) {
    if (!book || !book->impl) return false;

    hft::Side cpp_side = (side == HFT_SIDE_BUY) ? hft::Side::Buy : hft::Side::Sell;
    book->impl->add_order(order_id, cpp_side, price, quantity);
    return true;
}

bool hft_orderbook_cancel_order(HftOrderBookHandle book, HftOrderId order_id) {
    if (!book || !book->impl) return false;
    return book->impl->cancel_order(order_id);
}

bool hft_orderbook_execute_order(
    HftOrderBookHandle book,
    HftOrderId order_id,
    HftQuantity quantity
) {
    if (!book || !book->impl) return false;
    book->impl->execute_order(order_id, quantity);
    return true;
}

HftPrice hft_orderbook_best_bid(HftOrderBookHandle book) {
    if (!book || !book->impl) return INT64_MIN;
    return book->impl->best_bid();
}

HftPrice hft_orderbook_best_ask(HftOrderBookHandle book) {
    if (!book || !book->impl) return INT64_MAX;
    return book->impl->best_ask();
}

HftQuantity hft_orderbook_bid_quantity_at(HftOrderBookHandle book, HftPrice price) {
    if (!book || !book->impl) return 0;
    return book->impl->bid_quantity_at(price);
}

HftQuantity hft_orderbook_ask_quantity_at(HftOrderBookHandle book, HftPrice price) {
    if (!book || !book->impl) return 0;
    return book->impl->ask_quantity_at(price);
}

// ============================================
// Matching Engine API Implementation
// ============================================

HftMatchingEngineHandle hft_matching_engine_create(HftPrice base_price, size_t price_range) {
    try {
        auto handle = new HftMatchingEngine();
        handle->impl = std::make_unique<hft::MatchingEngine>(base_price, price_range);
        handle->callback = nullptr;
        handle->user_data = nullptr;
        return handle;
    } catch (...) {
        return nullptr;
    }
}

void hft_matching_engine_destroy(HftMatchingEngineHandle engine) {
    delete engine;
}

void hft_matching_engine_set_callback(
    HftMatchingEngineHandle engine,
    HftTradeCallback callback,
    void* user_data
) {
    if (!engine || !engine->impl) return;

    engine->callback = callback;
    engine->user_data = user_data;

    if (callback) {
        engine->impl->set_trade_callback([engine](const hft::Trade& trade) {
            HftTrade c_trade;
            c_trade.aggressive_order_id = trade.aggressive_order_id;
            c_trade.passive_order_id = trade.passive_order_id;
            c_trade.price = trade.price;
            c_trade.quantity = trade.quantity;
            c_trade.aggressor_side = (trade.aggressor_side == hft::Side::Buy) ?
                                      HFT_SIDE_BUY : HFT_SIDE_SELL;
            c_trade.timestamp = trade.timestamp;

            engine->callback(&c_trade, engine->user_data);
        });
    }
}

size_t hft_matching_engine_add_order(
    HftMatchingEngineHandle engine,
    HftOrderId order_id,
    HftSide side,
    HftPrice price,
    HftQuantity quantity
) {
    if (!engine || !engine->impl) return 0;

    hft::Side cpp_side = (side == HFT_SIDE_BUY) ? hft::Side::Buy : hft::Side::Sell;
    engine->impl->add_order(order_id, cpp_side, price, quantity);

    // Return number of trades (simplified - actual implementation would track this)
    return 0;
}

bool hft_matching_engine_cancel_order(HftMatchingEngineHandle engine, HftOrderId order_id) {
    if (!engine || !engine->impl) return false;
    return engine->impl->cancel_order(order_id);
}

HftOrderBookHandle hft_matching_engine_get_orderbook(HftMatchingEngineHandle engine) {
    // Note: This returns a non-owning view. The caller should not destroy it.
    // For simplicity, we return nullptr here. A full implementation would
    // provide a way to access the internal order book without ownership issues.
    return nullptr;
}

// ============================================
// Market Maker Strategy API Implementation
// ============================================

HftMarketMakerHandle hft_market_maker_create(
    int spread_bps,
    HftQuantity quote_size,
    HftQuantity max_position,
    double skew_factor
) {
    try {
        hft::strategy::MarketMakerConfig config;
        config.spread_bps = spread_bps;
        config.quote_size = quote_size;
        config.max_position = max_position;
        config.skew_factor = skew_factor;

        auto handle = new HftMarketMaker();
        handle->impl = std::make_unique<hft::strategy::MarketMaker>(config);
        return handle;
    } catch (...) {
        return nullptr;
    }
}

void hft_market_maker_destroy(HftMarketMakerHandle mm) {
    delete mm;
}

void hft_market_maker_calculate_quotes(
    HftMarketMakerHandle mm,
    HftPrice mid_price,
    int32_t position,
    HftQuote* out_quote
) {
    if (!mm || !mm->impl || !out_quote) return;

    auto quote = mm->impl->calculate_quote(mid_price, position);

    out_quote->bid_price = quote.bid_price;
    out_quote->ask_price = quote.ask_price;
    out_quote->bid_size = quote.bid_size;
    out_quote->ask_size = quote.ask_size;
}

// ============================================
// Utility Functions Implementation
// ============================================

HftPrice hft_price_from_double(double price) {
    return static_cast<HftPrice>(price * 10000.0 + 0.5);
}

double hft_price_to_double(HftPrice price) {
    return static_cast<double>(price) / 10000.0;
}

const char* hft_version(void) {
    return HFT_VERSION;
}

} // extern "C"
