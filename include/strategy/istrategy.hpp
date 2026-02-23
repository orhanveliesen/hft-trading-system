#pragma once

#include "../types.hpp"
#include "regime_detector.hpp"

#include <cstdint>
#include <string_view>

namespace hft {
namespace strategy {

// =============================================================================
// Signal Types
// =============================================================================

enum class SignalType : uint8_t {
    None = 0,
    Buy,
    Sell,
    Exit // Close position regardless of direction
};

enum class SignalStrength : uint8_t { None = 0, Weak, Medium, Strong };

enum class OrderPreference : uint8_t {
    Market, // Execute immediately, accept slippage
    Limit,  // Passive order, no slippage
    Either  // Let ExecutionEngine decide based on conditions
};

// =============================================================================
// Market Data Snapshot
// =============================================================================

struct MarketSnapshot {
    Price bid = 0;
    Price ask = 0;
    Quantity bid_size = 0;
    Quantity ask_size = 0;
    Price last_trade = 0;
    uint64_t timestamp_ns = 0;

    // Helpers
    Price mid() const { return (bid + ask) / 2; }

    Price spread() const { return ask - bid; }

    double spread_bps() const {
        if (mid() == 0)
            return 0;
        return static_cast<double>(spread()) * 10000.0 / static_cast<double>(mid());
    }

    double mid_usd(double price_scale) const { return static_cast<double>(mid()) / price_scale; }

    double bid_usd(double price_scale) const { return static_cast<double>(bid) / price_scale; }

    double ask_usd(double price_scale) const { return static_cast<double>(ask) / price_scale; }

    bool valid() const { return bid > 0 && ask > 0 && ask > bid; }
};

// =============================================================================
// Position Information for Strategy Signal Generation
// Note: Named StrategyPosition to avoid conflict with halt_manager's PositionInfo
// =============================================================================

struct StrategyPosition {
    double quantity = 0;        // Current holding (can be fractional for crypto)
    double avg_entry_price = 0; // Average entry price
    double unrealized_pnl = 0;  // Current unrealized P&L
    double realized_pnl = 0;    // Total realized P&L
    double cash_available = 0;  // Cash available for new trades
    double max_position = 0;    // Maximum allowed position

    bool has_position() const { return quantity > 1e-9; }
    bool can_buy() const { return cash_available > 0; }
    bool can_sell() const { return quantity > 1e-9; }
    double position_pct() const { return max_position > 0 ? quantity / max_position : 0; }
};

// =============================================================================
// Strategy Signal Output
// =============================================================================

struct Signal {
    SignalType type = SignalType::None;
    SignalStrength strength = SignalStrength::None;
    OrderPreference order_pref = OrderPreference::Either;

    double suggested_qty = 0; // Suggested order quantity
    Price limit_price = 0;    // For limit orders (0 = use mid)

    const char* reason = ""; // Human-readable reason for logging

    // Helper to check if signal is actionable
    bool is_actionable() const {
        return type != SignalType::None && strength != SignalStrength::None && suggested_qty > 0;
    }

    bool is_buy() const { return type == SignalType::Buy; }
    bool is_sell() const { return type == SignalType::Sell || type == SignalType::Exit; }

    // Factory methods
    static Signal none() { return Signal{}; }

    static Signal buy(SignalStrength str, double qty, const char* reason = "") {
        return Signal{SignalType::Buy, str, OrderPreference::Either, qty, 0, reason};
    }

    static Signal sell(SignalStrength str, double qty, const char* reason = "") {
        return Signal{SignalType::Sell, str, OrderPreference::Either, qty, 0, reason};
    }

    static Signal exit(double qty, const char* reason = "") {
        return Signal{SignalType::Exit, SignalStrength::Strong, OrderPreference::Market, qty, 0, reason};
    }

    static Signal limit_buy(SignalStrength str, double qty, Price price, const char* reason = "") {
        return Signal{SignalType::Buy, str, OrderPreference::Limit, qty, price, reason};
    }

    static Signal limit_sell(SignalStrength str, double qty, Price price, const char* reason = "") {
        return Signal{SignalType::Sell, str, OrderPreference::Limit, qty, price, reason};
    }
};

// =============================================================================
// Strategy Interface
// =============================================================================

class IStrategy {
public:
    virtual ~IStrategy() = default;

    // =========================================================================
    // Core Methods
    // =========================================================================

    /**
     * Generate trading signal based on current market state
     *
     * @param symbol Symbol ID
     * @param market Current market data (bid/ask/sizes)
     * @param position Current position information
     * @param regime Current market regime
     * @return Signal with type, strength, quantity, and order preference
     */
    virtual Signal generate(Symbol symbol, const MarketSnapshot& market, const StrategyPosition& position,
                            MarketRegime regime) = 0;

    // =========================================================================
    // Metadata
    // =========================================================================

    /// Strategy name for logging and selection
    virtual std::string_view name() const = 0;

    /// Default order type preference (can be overridden in Signal)
    virtual OrderPreference default_order_preference() const = 0;

    // =========================================================================
    // Regime Suitability
    // =========================================================================

    /// Check if this strategy is suitable for the given market regime
    virtual bool suitable_for_regime(MarketRegime regime) const = 0;

    // =========================================================================
    // State Management
    // =========================================================================

    /// Called on every tick to update internal state (indicators, etc.)
    virtual void on_tick(const MarketSnapshot& market) = 0;

    /// Reset internal state (e.g., when switching symbols)
    virtual void reset() = 0;

    /// Check if strategy has enough data to generate signals
    virtual bool ready() const = 0;
};

// =============================================================================
// Helper: Convert between signal strength types
// =============================================================================

inline SignalStrength to_signal_strength(int value) {
    if (value >= 3)
        return SignalStrength::Strong;
    if (value >= 2)
        return SignalStrength::Medium;
    if (value >= 1)
        return SignalStrength::Weak;
    return SignalStrength::None;
}

inline const char* signal_type_str(SignalType type) {
    switch (type) {
    case SignalType::Buy:
        return "BUY";
    case SignalType::Sell:
        return "SELL";
    case SignalType::Exit:
        return "EXIT";
    default:
        return "NONE";
    }
}

inline const char* signal_strength_str(SignalStrength str) {
    switch (str) {
    case SignalStrength::Strong:
        return "STRONG";
    case SignalStrength::Medium:
        return "MEDIUM";
    case SignalStrength::Weak:
        return "WEAK";
    default:
        return "NONE";
    }
}

inline const char* order_pref_str(OrderPreference pref) {
    switch (pref) {
    case OrderPreference::Market:
        return "MARKET";
    case OrderPreference::Limit:
        return "LIMIT";
    default:
        return "EITHER";
    }
}

} // namespace strategy
} // namespace hft
