#pragma once

#include "types.hpp"

namespace hft {

/**
 * Exchange-agnostic market events
 *
 * These are the common events that any feed handler must produce.
 * Each exchange-specific parser converts their native format to these.
 */

// New order added to the book
struct OrderAdd {
    OrderId order_id;
    SymbolId symbol_id;
    Side side;
    Price price;
    Quantity quantity;
    Timestamp timestamp;
};

// Order fully or partially executed (trade occurred)
struct OrderExecute {
    OrderId order_id;
    Quantity quantity;      // Quantity executed
    Price exec_price;       // Execution price (may differ from order price)
    Timestamp timestamp;
};

// Order quantity reduced (partial cancel)
struct OrderReduce {
    OrderId order_id;
    Quantity reduce_by;     // Amount to reduce
    Timestamp timestamp;
};

// Order completely removed from book
struct OrderDelete {
    OrderId order_id;
    Timestamp timestamp;
};

// Trade event (for strategies that only need trades, not full book)
struct Trade {
    SymbolId symbol_id;
    Price price;
    Quantity quantity;
    Side aggressor_side;    // Which side initiated the trade
    Timestamp timestamp;
};

// Top-of-book quote update (BBO)
struct QuoteUpdate {
    SymbolId symbol_id;
    Price bid_price;
    Price ask_price;
    Quantity bid_size;
    Quantity ask_size;
    Timestamp timestamp;
};

// Book level update (for L2 data)
struct BookLevelUpdate {
    SymbolId symbol_id;
    Side side;
    Price price;
    Quantity quantity;      // New total quantity at this level (0 = level removed)
    Timestamp timestamp;
};

/**
 * Callback concept (documentation)
 *
 * A valid market data callback must implement:
 *   void on_order_add(const OrderAdd&)
 *   void on_order_execute(const OrderExecute&)
 *   void on_order_reduce(const OrderReduce&)
 *   void on_order_delete(const OrderDelete&)
 *
 * Optional (for trade-only strategies):
 *   void on_trade(const Trade&)
 *   void on_quote(const QuoteUpdate&)
 *   void on_book_level(const BookLevelUpdate&)
 */

}  // namespace hft
