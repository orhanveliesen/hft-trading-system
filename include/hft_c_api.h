/**
 * HFT C API - Foreign Function Interface
 *
 * This header provides a C-compatible interface to the HFT library,
 * enabling integration with other languages like Rust, Python, etc.
 */

#ifndef HFT_C_API_H
#define HFT_C_API_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * Type Definitions
 * ============================================ */

typedef uint64_t HftOrderId;
typedef uint32_t HftSymbol;
typedef int64_t  HftPrice;      // Fixed-point price (4 decimal places)
typedef uint32_t HftQuantity;

typedef enum {
    HFT_SIDE_BUY = 0,
    HFT_SIDE_SELL = 1
} HftSide;

/* Opaque handle types */
typedef struct HftOrderBook* HftOrderBookHandle;
typedef struct HftMatchingEngine* HftMatchingEngineHandle;
typedef struct HftMarketMaker* HftMarketMakerHandle;

/* Trade callback structure */
typedef struct {
    HftOrderId aggressive_order_id;
    HftOrderId passive_order_id;
    HftPrice price;
    HftQuantity quantity;
    HftSide aggressor_side;
    uint64_t timestamp;
} HftTrade;

/* Quote structure */
typedef struct {
    HftPrice bid_price;
    HftPrice ask_price;
    HftQuantity bid_size;
    HftQuantity ask_size;
} HftQuote;

/* Market data update structure */
typedef struct {
    HftSymbol symbol;
    HftPrice best_bid;
    HftPrice best_ask;
    HftQuantity bid_size;
    HftQuantity ask_size;
    uint64_t timestamp;
} HftMarketUpdate;

/* Callback function types */
typedef void (*HftTradeCallback)(const HftTrade* trade, void* user_data);
typedef void (*HftQuoteCallback)(const HftQuote* quote, void* user_data);

/* ============================================
 * OrderBook API
 * ============================================ */

/**
 * Create a new order book
 * @param base_price Minimum price in the book (fixed-point)
 * @param price_range Number of price levels from base_price
 * @return Handle to the order book, NULL on failure
 */
HftOrderBookHandle hft_orderbook_create(HftPrice base_price, size_t price_range);

/**
 * Destroy an order book and free memory
 */
void hft_orderbook_destroy(HftOrderBookHandle book);

/**
 * Add an order to the book
 * @return true if order was added successfully
 */
bool hft_orderbook_add_order(
    HftOrderBookHandle book,
    HftOrderId order_id,
    HftSide side,
    HftPrice price,
    HftQuantity quantity
);

/**
 * Cancel an order
 * @return true if order was found and cancelled
 */
bool hft_orderbook_cancel_order(HftOrderBookHandle book, HftOrderId order_id);

/**
 * Execute (partially fill) an order
 * @return true if order was found and executed
 */
bool hft_orderbook_execute_order(
    HftOrderBookHandle book,
    HftOrderId order_id,
    HftQuantity quantity
);

/**
 * Get the best bid price
 * @return Best bid price, or INT64_MIN if no bids
 */
HftPrice hft_orderbook_best_bid(HftOrderBookHandle book);

/**
 * Get the best ask price
 * @return Best ask price, or INT64_MAX if no asks
 */
HftPrice hft_orderbook_best_ask(HftOrderBookHandle book);

/**
 * Get quantity at a specific bid price level
 */
HftQuantity hft_orderbook_bid_quantity_at(HftOrderBookHandle book, HftPrice price);

/**
 * Get quantity at a specific ask price level
 */
HftQuantity hft_orderbook_ask_quantity_at(HftOrderBookHandle book, HftPrice price);

/* ============================================
 * Matching Engine API
 * ============================================ */

/**
 * Create a new matching engine
 * @param base_price Minimum price for the order book
 * @param price_range Number of price levels
 * @return Handle to the matching engine, NULL on failure
 */
HftMatchingEngineHandle hft_matching_engine_create(HftPrice base_price, size_t price_range);

/**
 * Destroy a matching engine
 */
void hft_matching_engine_destroy(HftMatchingEngineHandle engine);

/**
 * Set trade callback
 * @param callback Function to call when a trade occurs
 * @param user_data User pointer passed to callback
 */
void hft_matching_engine_set_callback(
    HftMatchingEngineHandle engine,
    HftTradeCallback callback,
    void* user_data
);

/**
 * Submit an order to the matching engine
 * @return Number of trades generated
 */
size_t hft_matching_engine_add_order(
    HftMatchingEngineHandle engine,
    HftOrderId order_id,
    HftSide side,
    HftPrice price,
    HftQuantity quantity
);

/**
 * Cancel an order
 * @return true if order was found and cancelled
 */
bool hft_matching_engine_cancel_order(HftMatchingEngineHandle engine, HftOrderId order_id);

/**
 * Get the underlying order book handle
 */
HftOrderBookHandle hft_matching_engine_get_orderbook(HftMatchingEngineHandle engine);

/* ============================================
 * Market Maker Strategy API
 * ============================================ */

/**
 * Create a new market maker strategy
 * @param spread_bps Spread in basis points (e.g., 10 = 0.1%)
 * @param quote_size Size of each quote
 * @param max_position Maximum allowed position
 * @param skew_factor Inventory skew factor (0.0 to 1.0)
 * @return Handle to the market maker, NULL on failure
 */
HftMarketMakerHandle hft_market_maker_create(
    int spread_bps,
    HftQuantity quote_size,
    HftQuantity max_position,
    double skew_factor
);

/**
 * Destroy a market maker strategy
 */
void hft_market_maker_destroy(HftMarketMakerHandle mm);

/**
 * Calculate quotes based on mid price and current position
 * @param mm Market maker handle
 * @param mid_price Current mid price
 * @param position Current position (positive = long, negative = short)
 * @param out_quote Output quote structure
 */
void hft_market_maker_calculate_quotes(
    HftMarketMakerHandle mm,
    HftPrice mid_price,
    int32_t position,
    HftQuote* out_quote
);

/* ============================================
 * Utility Functions
 * ============================================ */

/**
 * Convert a double price to fixed-point representation
 * @param price Price as double (e.g., 150.25)
 * @return Fixed-point price (e.g., 1502500)
 */
HftPrice hft_price_from_double(double price);

/**
 * Convert a fixed-point price to double
 * @param price Fixed-point price
 * @return Price as double
 */
double hft_price_to_double(HftPrice price);

/**
 * Get library version string
 */
const char* hft_version(void);

#ifdef __cplusplus
}
#endif

#endif /* HFT_C_API_H */
