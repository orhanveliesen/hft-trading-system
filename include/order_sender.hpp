#pragma once

#include "types.hpp"
#include <type_traits>
#if __cplusplus >= 202002L
#include <concepts>
#endif

namespace hft {

/**
 * OrderSender Concept
 *
 * Zero-cost abstraction for order sending/cancelling.
 * Template-based polymorphism eliminates std::function overhead.
 *
 * Implementations must provide:
 *   - bool send_order(Symbol, Side, Quantity, bool is_market)
 *   - bool cancel_order(Symbol, OrderId)
 *
 * Example implementations:
 *   - MockOrderSender (testing)
 *   - BinanceOrderSender (paper trading)
 *   - OuchOrderSender (NASDAQ direct)
 *   - FixOrderSender (FIX protocol)
 */

// C++20 concept (fallback to SFINAE for C++17)
#if __cplusplus >= 202002L

template<typename T>
concept OrderSenderConcept = requires(T& sender, Symbol s, Side side, Quantity q, OrderId id) {
    { sender.send_order(s, side, q, true) } -> std::convertible_to<bool>;
    { sender.cancel_order(s, id) } -> std::convertible_to<bool>;
};

// Provide is_order_sender_v for backwards compatibility
template<typename T>
inline constexpr bool is_order_sender_v = OrderSenderConcept<T>;

#else

// C++17 SFINAE fallback
namespace detail {

template<typename T, typename = void>
struct is_order_sender : std::false_type {};

template<typename T>
struct is_order_sender<T, std::void_t<
    decltype(std::declval<T&>().send_order(
        std::declval<Symbol>(),
        std::declval<Side>(),
        std::declval<Quantity>(),
        std::declval<bool>()
    )),
    decltype(std::declval<T&>().cancel_order(
        std::declval<Symbol>(),
        std::declval<OrderId>()
    ))
>> : std::true_type {};

}  // namespace detail

template<typename T>
inline constexpr bool is_order_sender_v = detail::is_order_sender<T>::value;

#endif

/**
 * NullOrderSender - No-op implementation
 *
 * Used when order sending is not needed (e.g., backtest, market data only).
 * All operations succeed but do nothing.
 */
struct NullOrderSender {
    bool send_order(Symbol /*symbol*/, Side /*side*/, Quantity /*qty*/, bool /*is_market*/) {
        return true;
    }

    bool cancel_order(Symbol /*symbol*/, OrderId /*order_id*/) {
        return true;
    }
};

}  // namespace hft
