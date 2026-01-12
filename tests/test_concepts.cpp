/**
 * Concept Validation Tests
 *
 * Compile-time verification that implementations satisfy their concepts.
 * These tests ensure type contracts are enforced at compile time.
 */

#include <iostream>
#include <cassert>

#include "../include/concepts.hpp"
#include "../include/order_sender.hpp"
#include "../include/mock_order_sender.hpp"
#include "../include/feed_handler.hpp"
#include "../include/orderbook.hpp"
#include "../include/market_data_handler.hpp"
#include "../include/strategy/simple_mean_reversion.hpp"

using namespace hft;
using namespace hft::concepts;

// =============================================================================
// Compile-Time Concept Checks (static_assert)
// =============================================================================

// OrderSender implementations
static_assert(OrderSender<NullOrderSender>, "NullOrderSender must satisfy OrderSender");
static_assert(OrderSender<MockOrderSender>, "MockOrderSender must satisfy OrderSender");

// Verify non-OrderSender types fail
struct NotAnOrderSender {};
static_assert(!OrderSender<NotAnOrderSender>, "NotAnOrderSender should NOT satisfy OrderSender");

struct PartialOrderSender {
    bool send_order(Symbol, Side, Quantity, bool) { return true; }
    // Missing cancel_order
};
static_assert(!OrderSender<PartialOrderSender>, "PartialOrderSender should NOT satisfy OrderSender");

// FeedCallback - MarketDataHandler is a FeedCallback (adapter pattern)
static_assert(FeedCallback<MarketDataHandler>, "MarketDataHandler must satisfy FeedCallback");
static_assert(!FeedCallback<OrderBook>, "OrderBook should NOT satisfy FeedCallback (use MarketDataHandler)");

// ReadableOrderBook - OrderBook has best_bid/best_ask
static_assert(ReadableOrderBook<OrderBook>, "OrderBook must satisfy ReadableOrderBook");

// DetailedOrderBook - OrderBook has bid_quantity_at/ask_quantity_at
static_assert(DetailedOrderBook<OrderBook>, "OrderBook must satisfy DetailedOrderBook");

// TradingStrategy
static_assert(BasicStrategy<strategy::SimpleMeanReversion>,
              "SimpleMeanReversion must satisfy BasicStrategy");

// Type traits compatibility
static_assert(is_order_sender_v<NullOrderSender>, "is_order_sender_v must work");
static_assert(is_order_sender_v<MockOrderSender>, "is_order_sender_v must work");
static_assert(!is_order_sender_v<NotAnOrderSender>, "is_order_sender_v must reject non-senders");

// =============================================================================
// Runtime Tests
// =============================================================================

void test_order_sender_concept() {
    std::cout << "  test_order_sender_concept... ";

    // Create instances and verify they work
    NullOrderSender null_sender;
    assert(null_sender.send_order(1, Side::Buy, 100, true) == true);
    assert(null_sender.cancel_order(1, 12345) == true);

    MockOrderSender mock_sender;
    assert(mock_sender.send_order(1, Side::Buy, 100, false) == true);
    assert(mock_sender.send_count() == 1);

    std::cout << "PASSED\n";
}

void test_feed_callback_concept() {
    std::cout << "  test_feed_callback_concept... ";

    // MarketDataHandler satisfies FeedCallback - used by FeedHandler
    OrderBook book;
    MarketDataHandler handler(book);

    // These methods exist because MarketDataHandler satisfies FeedCallback
    handler.on_add_order(1001, Side::Buy, 10000, 100);
    handler.on_add_order(1002, Side::Sell, 10100, 50);

    assert(book.best_bid() == 10000);
    assert(book.best_ask() == 10100);

    handler.on_order_executed(1001, 50);
    handler.on_order_deleted(1002);

    std::cout << "PASSED\n";
}

void test_readable_order_book_concept() {
    std::cout << "  test_readable_order_book_concept... ";

    OrderBook book;
    book.add_order(1, Side::Buy, 100, 10);
    book.add_order(2, Side::Sell, 110, 20);

    // ReadableOrderBook methods
    Price bid = book.best_bid();
    Price ask = book.best_ask();

    assert(bid == 100);
    assert(ask == 110);

    // DetailedOrderBook methods (bid_quantity_at, ask_quantity_at)
    Quantity bid_qty = book.bid_quantity_at(100);
    Quantity ask_qty = book.ask_quantity_at(110);

    assert(bid_qty == 10);
    assert(ask_qty == 20);

    std::cout << "PASSED\n";
}

void test_trading_strategy_concept() {
    std::cout << "  test_trading_strategy_concept... ";

    strategy::SimpleMeanReversion strategy;

    // BasicStrategy requires operator()(Price, Price)
    auto signal1 = strategy(100, 101);
    auto signal2 = strategy(99, 100);  // Price dropped -> should signal

    // PositionAwareStrategy requires operator()(Price, Price, Position)
    auto signal3 = strategy(100, 101, 0);

    std::cout << "PASSED\n";
}

void test_feed_handler_with_concept() {
    std::cout << "  test_feed_handler_with_concept... ";

    // FeedHandler requires FeedCallback - use MarketDataHandler adapter
    OrderBook book;
    MarketDataHandler md_handler(book);
    FeedHandler<MarketDataHandler> feed_handler(md_handler);

    // Verify handler can process messages
    // (actual message parsing tested in test_feed_handler.cpp)

    std::cout << "PASSED\n";
}

// =============================================================================
// Concept Constraint Examples
// =============================================================================

// Function constrained by OrderSender concept
template<OrderSender T>
bool send_test_order(T& sender) {
    return sender.send_order(1, Side::Buy, 100, true);
}

// Function constrained by ReadableOrderBook concept
template<ReadableOrderBook T>
Price get_spread(const T& book) {
    return book.best_ask() - book.best_bid();
}

void test_constrained_functions() {
    std::cout << "  test_constrained_functions... ";

    NullOrderSender sender;
    assert(send_test_order(sender) == true);

    OrderBook book;
    book.add_order(1, Side::Buy, 100, 10);
    book.add_order(2, Side::Sell, 110, 20);
    assert(get_spread(book) == 10);

    std::cout << "PASSED\n";
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "\n=== Concept Validation Tests ===\n\n";

    test_order_sender_concept();
    test_feed_callback_concept();
    test_readable_order_book_concept();
    test_trading_strategy_concept();
    test_feed_handler_with_concept();
    test_constrained_functions();

    std::cout << "\n=== All concept tests passed! ===\n\n";

    std::cout << "Concept usage summary:\n";
    std::cout << "  - OrderSender: NullOrderSender, MockOrderSender, BinanceOrderSender\n";
    std::cout << "  - FeedCallback: MarketDataHandler (adapter for FeedHandler)\n";
    std::cout << "  - ReadableOrderBook: OrderBook (best_bid/best_ask)\n";
    std::cout << "  - DetailedOrderBook: OrderBook (with quantity queries)\n";
    std::cout << "  - TradingStrategy: SimpleMeanReversion, SmartStrategy\n";
    std::cout << "\nConcepts enable:\n";
    std::cout << "  - Clear error messages at compile time\n";
    std::cout << "  - Self-documenting template requirements\n";
    std::cout << "  - Overload resolution based on capabilities\n";
    std::cout << "  - Zero runtime overhead\n\n";

    return 0;
}
