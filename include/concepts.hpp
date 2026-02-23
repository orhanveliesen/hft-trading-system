#pragma once

/**
 * HFT Type Concepts - Compile-time Contract Enforcement
 *
 * C++20/23 concepts for strict template type checking.
 * These concepts ensure that template parameters satisfy
 * required interfaces at compile-time with clear error messages.
 *
 * Benefits:
 *   1. Better error messages (vs. template substitution failures)
 *   2. Self-documenting code (concepts as interface specs)
 *   3. Overload resolution based on type capabilities
 *   4. Zero runtime overhead (compile-time only)
 *
 * Usage:
 *   template<OrderSender Sender>
 *   class TradingEngine { ... };
 *
 *   // Constraint with requires clause
 *   template<typename T>
 *       requires TradingStrategy<T>
 *   void run_backtest(T& strategy);
 */

#include "types.hpp"

#include <concepts>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>

namespace hft {
namespace concepts {

// =============================================================================
// Core Type Concepts
// =============================================================================

/**
 * Arithmetic - Numeric types for prices, quantities, etc.
 */
template <typename T>
concept Arithmetic = std::is_arithmetic_v<T>;

/**
 * PriceType - Can be used as a price value
 */
template <typename T>
concept PriceType = Arithmetic<T> && requires(T a, T b) {
    { a + b } -> std::convertible_to<T>;
    { a - b } -> std::convertible_to<T>;
    { a* b } -> std::convertible_to<T>;
    { a / b } -> std::convertible_to<T>;
    { a < b } -> std::convertible_to<bool>;
    { a > b } -> std::convertible_to<bool>;
    { a == b } -> std::convertible_to<bool>;
};

/**
 * QuantityType - Can be used as a quantity value
 */
template <typename T>
concept QuantityType = std::integral<T> || std::floating_point<T>;

// =============================================================================
// Order Sender Concept
// =============================================================================

/**
 * OrderSender - Can send and cancel orders
 *
 * Required methods:
 *   bool send_order(Symbol, Side, Quantity, bool is_market)
 *   bool cancel_order(Symbol, OrderId)
 *
 * Implementations:
 *   - NullOrderSender (no-op)
 *   - MockOrderSender (testing)
 *   - BinanceOrderSender (paper trading)
 *   - OuchOrderSender (NASDAQ direct)
 */
template <typename T>
concept OrderSender = requires(T& sender, Symbol s, Side side, Quantity q, OrderId id, bool is_market) {
    { sender.send_order(s, side, q, is_market) } -> std::convertible_to<bool>;
    { sender.cancel_order(s, id) } -> std::convertible_to<bool>;
};

/**
 * ExtendedOrderSender - OrderSender with additional capabilities
 *
 * Adds:
 *   - Limit order support with price
 *   - Order status query
 */
template <typename T>
concept ExtendedOrderSender =
    OrderSender<T> && requires(T& sender, Symbol s, Side side, Quantity q, Price p, OrderId id) {
        { sender.send_limit_order(s, side, q, p) } -> std::convertible_to<OrderId>;
        { sender.is_order_active(id) } -> std::convertible_to<bool>;
    };

// =============================================================================
// Feed Handler Callback Concept
// =============================================================================

/**
 * FeedCallback - Receives parsed market data events
 *
 * Required methods:
 *   void on_add_order(OrderId, Side, Price, Quantity)
 *   void on_order_executed(OrderId, Quantity)
 *   void on_order_cancelled(OrderId, Quantity)
 *   void on_order_deleted(OrderId)
 *
 * Used by: FeedHandler, BinanceFeedHandler, etc.
 */
template <typename T>
concept FeedCallback = requires(T& cb, OrderId id, Side side, Price price, Quantity qty) {
    { cb.on_add_order(id, side, price, qty) } -> std::same_as<void>;
    { cb.on_order_executed(id, qty) } -> std::same_as<void>;
    { cb.on_order_cancelled(id, qty) } -> std::same_as<void>;
    { cb.on_order_deleted(id) } -> std::same_as<void>;
};

/**
 * QuoteCallback - Receives quote (BBO) updates
 *
 * Required methods:
 *   void on_quote(Symbol, Price bid, Price ask, Quantity bid_size, Quantity ask_size)
 */
template <typename T>
concept QuoteCallback = requires(T& cb, Symbol sym, Price bid, Price ask, Quantity bid_sz, Quantity ask_sz) {
    { cb.on_quote(sym, bid, ask, bid_sz, ask_sz) } -> std::same_as<void>;
};

/**
 * TradeCallback - Receives trade (last sale) updates
 *
 * Required methods:
 *   void on_trade(Symbol, Price, Quantity, Side aggressor)
 */
template <typename T>
concept TradeCallback = requires(T& cb, Symbol sym, Price price, Quantity qty, Side side) {
    { cb.on_trade(sym, price, qty, side) } -> std::same_as<void>;
};

/**
 * FullMarketDataCallback - Combined quote and trade callbacks
 */
template <typename T>
concept FullMarketDataCallback = QuoteCallback<T> && TradeCallback<T>;

// =============================================================================
// Trading Strategy Concepts
// =============================================================================

/**
 * SignalLike - A type that can represent trading signals
 *
 * Any type that has a finite set of values representing trading decisions.
 * Examples: enum Signal, enum class Signal, int codes, etc.
 */
template <typename T>
concept SignalLike = std::is_enum_v<T> || std::is_integral_v<T>;

/**
 * BasicStrategy - Minimal strategy interface
 *
 * Required:
 *   SignalLike operator()(Price bid, Price ask)
 *
 * Returns a signal indicating trading decision (Buy/Sell/None/Close)
 */
template <typename T>
concept BasicStrategy = requires(T& strategy, Price bid, Price ask) {
    { strategy(bid, ask) }; // Just require it's callable, Signal type is project-specific
};

/**
 * PositionAwareStrategy - Strategy that considers current position
 *
 * Required:
 *   SignalLike operator()(Price bid, Price ask, Position current_position)
 */
template <typename T>
concept PositionAwareStrategy = requires(T& strategy, Price bid, Price ask, Position pos) {
    { strategy(bid, ask, pos) }; // Callable with position
};

/**
 * TradingStrategy - Full strategy interface (either basic or position-aware)
 */
template <typename T>
concept TradingStrategy = BasicStrategy<T> || PositionAwareStrategy<T>;

/**
 * StatefulStrategy - Strategy with state management
 *
 * Adds:
 *   void reset()
 *   bool is_ready() const
 */
template <typename T>
concept StatefulStrategy = TradingStrategy<T> && requires(T& strategy, const T& const_strategy) {
    { strategy.reset() } -> std::same_as<void>;
    { const_strategy.is_ready() } -> std::convertible_to<bool>;
};

/**
 * ConfigurableStrategy - Strategy with runtime configuration
 *
 * Adds:
 *   typename config_type
 *   const Config& config() const
 */
template <typename T>
concept ConfigurableStrategy = TradingStrategy<T> && requires(const T& strategy) {
    typename T::config_type;
    { strategy.config() } -> std::convertible_to<const typename T::config_type&>;
};

// =============================================================================
// Risk Manager Concepts
// =============================================================================

/**
 * RiskChecker - Can check if an order is allowed
 *
 * Required:
 *   bool can_place_order(Symbol, Side, Quantity, Price) const
 *   bool can_trade() const
 */
template <typename T>
concept RiskChecker = requires(const T& rm, Symbol sym, Side side, Quantity qty, Price price) {
    { rm.can_place_order(sym, side, qty, price) } -> std::convertible_to<bool>;
    { rm.can_trade() } -> std::convertible_to<bool>;
};

/**
 * RiskManager - Full risk management interface
 *
 * Adds fill registration and state updates:
 *   void register_fill(Symbol, Side, Quantity, Price)
 *   void update_pnl(PnL)
 *   void reset_daily()
 */
template <typename T>
concept RiskManager = RiskChecker<T> && requires(T& rm, Symbol sym, Side side, Quantity qty, Price price, PnL pnl) {
    { rm.register_fill(sym, side, qty, price) } -> std::same_as<void>;
    { rm.update_pnl(pnl) } -> std::same_as<void>;
    { rm.reset_daily() } -> std::same_as<void>;
};

/**
 * SymbolRiskManager - Risk manager with per-symbol tracking
 *
 * Adds symbol registration:
 *   SymbolIndex register_symbol(const std::string&, Position max, Notional max)
 *   Position get_position(SymbolIndex) const
 */
template <typename T>
concept SymbolRiskManager = RiskManager<T> && requires(T& rm, const T& const_rm, const std::string& sym,
                                                       Position max_pos, uint64_t max_notional, uint32_t idx) {
    { rm.register_symbol(sym, max_pos, max_notional) } -> std::convertible_to<uint32_t>;
    { const_rm.get_position(idx) } -> std::convertible_to<Position>;
};

// =============================================================================
// Order Book Concepts
// =============================================================================

/**
 * ReadableOrderBook - Can query order book state
 *
 * Required:
 *   Price best_bid() const
 *   Price best_ask() const
 *
 * Note: bid_size/ask_size not required as OrderBook uses bid_quantity_at(price)
 */
template <typename T>
concept ReadableOrderBook = requires(const T& book) {
    { book.best_bid() } -> std::convertible_to<Price>;
    { book.best_ask() } -> std::convertible_to<Price>;
};

/**
 * DetailedOrderBook - ReadableOrderBook with quantity queries
 *
 * Additional:
 *   Quantity bid_quantity_at(Price) const
 *   Quantity ask_quantity_at(Price) const
 */
template <typename T>
concept DetailedOrderBook = ReadableOrderBook<T> && requires(const T& book, Price price) {
    { book.bid_quantity_at(price) } -> std::convertible_to<Quantity>;
    { book.ask_quantity_at(price) } -> std::convertible_to<Quantity>;
};

/**
 * MutableOrderBook - Can modify order book
 *
 * Adds:
 *   void add_order(OrderId, Side, Price, Quantity)
 *   bool cancel_order(OrderId)
 *   Quantity execute_order(OrderId, Quantity)
 */
template <typename T>
concept MutableOrderBook = ReadableOrderBook<T> && requires(T& book, OrderId id, Side side, Price price, Quantity qty) {
    { book.add_order(id, side, price, qty) } -> std::same_as<void>;
    { book.cancel_order(id) } -> std::convertible_to<bool>;
    { book.execute_order(id, qty) } -> std::convertible_to<Quantity>;
};

/**
 * FullOrderBook - Complete order book interface
 */
template <typename T>
concept FullOrderBook = MutableOrderBook<T> && requires(const T& book) {
    { book.order_count() } -> std::convertible_to<size_t>;
    { book.is_empty() } -> std::convertible_to<bool>;
};

// =============================================================================
// Serialization Concepts
// =============================================================================

/**
 * Serializable - Can be serialized to bytes
 *
 * Required:
 *   size_t serialize(std::span<uint8_t> buffer) const
 *   static size_t serialized_size()
 */
template <typename T>
concept Serializable = requires(const T& obj, std::span<uint8_t> buf) {
    { obj.serialize(buf) } -> std::convertible_to<size_t>;
    { T::serialized_size() } -> std::convertible_to<size_t>;
};

/**
 * Deserializable - Can be deserialized from bytes
 *
 * Required:
 *   static std::optional<T> deserialize(std::span<const uint8_t> buffer)
 */
template <typename T>
concept Deserializable = requires(std::span<const uint8_t> buf) {
    { T::deserialize(buf) }; // Returns optional<T> or similar
};

/**
 * FullySerializable - Both serializable and deserializable
 */
template <typename T>
concept FullySerializable = Serializable<T> && Deserializable<T>;

// =============================================================================
// Time-Related Concepts
// =============================================================================

/**
 * Timestamped - Has a timestamp
 *
 * Required:
 *   uint64_t timestamp() const  (nanoseconds since epoch)
 */
template <typename T>
concept Timestamped = requires(const T& obj) {
    { obj.timestamp() } -> std::convertible_to<uint64_t>;
};

/**
 * TimestampedMutable - Timestamp can be set
 */
template <typename T>
concept TimestampedMutable = Timestamped<T> && requires(T& obj, uint64_t ts) {
    { obj.set_timestamp(ts) } -> std::same_as<void>;
};

// =============================================================================
// Callback/Handler Concepts
// =============================================================================

/**
 * Callable with specific signature
 */
template <typename F, typename R, typename... Args>
concept CallableWith = std::invocable<F, Args...> && std::convertible_to<std::invoke_result_t<F, Args...>, R>;

/**
 * FillHandler - Handles order fills
 */
template <typename T>
concept FillHandler = requires(T& handler, Symbol sym, OrderId id, Side side, Quantity qty, Price price) {
    { handler.on_fill(sym, id, side, qty, price) } -> std::same_as<void>;
};

/**
 * ErrorHandler - Handles errors
 */
template <typename T>
concept ErrorHandler = requires(T& handler, int error_code, const char* message) {
    { handler.on_error(error_code, message) } -> std::same_as<void>;
};

// =============================================================================
// Container Concepts (HFT-specific)
// =============================================================================

/**
 * LockFreeQueue - Lock-free SPSC queue interface
 */
template <typename T>
concept LockFreeQueue = requires(T& q, const T& cq, typename T::value_type& val) {
    typename T::value_type;
    { q.push(val) } -> std::convertible_to<bool>;
    { q.pop(val) } -> std::convertible_to<bool>;
    { cq.empty() } -> std::convertible_to<bool>;
    { cq.size() } -> std::convertible_to<size_t>;
};

/**
 * ObjectPool - Pre-allocated object pool interface
 */
template <typename T>
concept ObjectPool = requires(T& pool) {
    typename T::value_type;
    { pool.allocate() } -> std::convertible_to<typename T::value_type*>;
    { pool.deallocate(std::declval<typename T::value_type*>()) } -> std::same_as<void>;
    { pool.available() } -> std::convertible_to<size_t>;
};

// =============================================================================
// Concept Aliases for Common Patterns
// =============================================================================

/**
 * TradingComponent - Any component that can be started/stopped
 */
template <typename T>
concept TradingComponent = requires(T& comp, const T& const_comp) {
    { comp.start() } -> std::same_as<void>;
    { comp.stop() } -> std::same_as<void>;
    { const_comp.is_running() } -> std::convertible_to<bool>;
};

/**
 * Resettable - Can be reset to initial state
 */
template <typename T>
concept Resettable = requires(T& obj) {
    { obj.reset() } -> std::same_as<void>;
};

/**
 * Named - Has a name/identifier
 */
template <typename T>
concept Named = requires(const T& obj) {
    { obj.name() } -> std::convertible_to<std::string_view>;
};

// =============================================================================
// Helper Type Traits (for backward compatibility)
// =============================================================================

template <typename T>
inline constexpr bool is_order_sender_v = OrderSender<T>;

template <typename T>
inline constexpr bool is_feed_callback_v = FeedCallback<T>;

template <typename T>
inline constexpr bool is_trading_strategy_v = TradingStrategy<T>;

template <typename T>
inline constexpr bool is_risk_manager_v = RiskManager<T>;

template <typename T>
inline constexpr bool is_readable_order_book_v = ReadableOrderBook<T>;

} // namespace concepts

// Bring commonly used concepts into hft namespace for convenience
// Note: RiskManager not brought in to avoid clash with strategy::RiskManager class
using concepts::FeedCallback;
using concepts::OrderSender;
using concepts::ReadableOrderBook;
using concepts::TradingStrategy;

// Keep backward compatibility alias
using concepts::is_order_sender_v;

} // namespace hft
