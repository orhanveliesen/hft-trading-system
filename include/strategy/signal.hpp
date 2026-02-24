#pragma once

namespace hft {
namespace strategy {

/**
 * Trading Signal
 *
 * Generic signal type used by all strategies.
 * Independent of backtest/paper/live trading context.
 */
enum class Signal {
    None, // No action
    Buy,  // Open long / add to long
    Sell, // Open short / add to short
    Close // Close current position
};

inline const char* signal_to_string(Signal sig) {
    switch (sig) {
    case Signal::Buy:
        return "BUY";
    case Signal::Sell:
        return "SELL";
    case Signal::Close:
        return "CLOSE";
    default:
        return "NONE";
    }
}

} // namespace strategy
} // namespace hft
