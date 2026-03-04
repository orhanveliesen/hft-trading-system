#pragma once
#include "../types.hpp" // OrderType, Side, Price, Symbol

namespace hft {
namespace execution {

/// Venue target for order routing
enum class Venue : uint8_t {
    Spot,   // Binance spot
    Futures // Binance USDT-M futures (Phase 4.3)
};

/// Produced by pipeline stages, consumed by ExecutionEngine
struct OrderRequest {
    Symbol symbol = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Market;
    double qty = 0.0;
    Price limit_price = 0;     // 0 for market orders
    Venue venue = Venue::Spot; // Phase 4.3: Futures support
    const char* reason = "";
    const char* source_stage = ""; // Which stage produced this

    bool is_valid() const { return qty > 0.0; }
};

} // namespace execution
} // namespace hft
