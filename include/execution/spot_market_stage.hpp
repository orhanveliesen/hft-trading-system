#pragma once
#include "execution_stage.hpp"

namespace hft {
namespace execution {

/// Phase 4.1: Converts actionable signals to spot Market orders
///
/// Logic:
/// - Actionable signal → Market order on Spot venue
/// - Sell qty capped to current position (prevent overselling)
/// - OrderPreference from signal is IGNORED — this stage always produces Market
///
/// Future: SpotLimitStage (Phase 4.2) will handle limit orders separately
class SpotMarketStage : public IExecutionStage {
public:
    std::vector<OrderRequest> process(const strategy::Signal& signal, const ExecutionContext& ctx) override {
        if (!signal.is_actionable()) {
            return {};
        }

        OrderRequest req;
        req.symbol = ctx.symbol;
        req.side = signal.is_buy() ? Side::Buy : Side::Sell;
        req.type = OrderType::Market;
        req.qty = signal.suggested_qty;
        req.limit_price = 0;
        req.venue = Venue::Spot;
        req.reason = signal.reason;
        req.source_stage = "SpotMarket";

        // Cap sell qty to current position
        if (req.side == Side::Sell) {
            if (!ctx.position.has_position()) {
                return {}; // Nothing to sell
            }
            if (req.qty > ctx.position.quantity) {
                req.qty = ctx.position.quantity;
            }
        }

        return {req};
    }

    std::string_view name() const override { return "SpotMarket"; }
};

} // namespace execution
} // namespace hft
