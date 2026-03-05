#pragma once
#include "execution_scorer.hpp"
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
/// Phase 4.2: Execution score gate added
/// - Only fires when exec_score ≤ 0 (Market preferred)
/// - If exec_score > 0, SpotLimitStage handles it
class SpotMarketStage : public IExecutionStage {
public:
    std::vector<OrderRequest> process(const strategy::Signal& signal, const ExecutionContext& ctx) override {
        if (!signal.is_actionable()) {
            return {};
        }

        // Phase 4.2: Check execution score — only fire for Market preference
        if (ctx.metrics) {
            Side side = signal.is_buy() ? Side::Buy : Side::Sell;
            auto exec = ExecutionScorer::compute(signal, ctx.metrics, side);
            if (exec.prefer_limit()) {
                return {}; // SpotLimitStage will handle this
            }
        }
        // No metrics → default to Market (safe fallback)

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
