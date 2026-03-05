#pragma once

#include "../util/time_utils.hpp"
#include "execution_engine.hpp"
#include "execution_scorer.hpp"
#include "execution_stage.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace hft {
namespace execution {

/// Pending limit order tracked by SpotLimitStage
struct PendingLimit {
    uint64_t order_id = 0;
    Symbol symbol = 0;
    Side side = Side::Buy;
    Price limit_price = 0;
    double qty = 0.0;
    uint64_t submit_time_ns = 0;
    bool active = false;

    void clear() {
        order_id = 0;
        symbol = 0;
        side = Side::Buy;
        limit_price = 0;
        qty = 0.0;
        submit_time_ns = 0;
        active = false;
    }
};

/// Configuration for SpotLimitStage
struct SpotLimitConfig {
    uint64_t timeout_ns = 5'000'000'000; // 5 seconds
    double replace_threshold_bps = 5.0;  // Reprice if > 5 bps drift
};

/// Execution stage: places intelligent limit orders based on execution score
///
/// Lifecycle:
///   1. New signal, no pending → place new limit
///   2. New signal, same direction → replace if price drifted > threshold
///   3. New signal, opposite direction → cancel pending, place new
///   4. Signal::none(), pending → check timeout (5s), cancel if stale
///   5. Trading halted → cancel all pending
class SpotLimitStage : public IExecutionStage {
public:
    using Config = SpotLimitConfig;

    explicit SpotLimitStage(ExecutionEngine* engine, const Config& config = Config())
        : engine_(engine), config_(config) {}

    /// Process signal through limit order logic
    ///
    /// Returns:
    ///   - Empty vector if Market preferred (exec_score ≤ 0)
    ///   - Empty vector if signal not actionable
    ///   - OrderRequest if limit order should be placed/replaced
    std::vector<OrderRequest> process(const strategy::Signal& signal, const ExecutionContext& ctx) override {
        Symbol symbol = ctx.symbol;

        // Step 1: Check timeout on existing pending
        cancel_stale_pending(symbol);

        // Step 2: Return empty if signal not actionable
        if (!signal.is_actionable()) {
            return {};
        }

        // Step 3: Compute execution score
        Side side = signal.is_buy() ? Side::Buy : Side::Sell;
        auto exec_result = ExecutionScorer::compute(signal, ctx.metrics, side);

        // Return empty if Market preferred
        if (!exec_result.prefer_limit()) {
            return {};
        }

        // Step 4: Calculate limit price
        Price limit_price = calculate_limit_price(ctx, side, exec_result.score);

        // Step 5: Check existing pending
        PendingLimit* existing = find_pending(symbol);

        if (existing) {
            // Direction changed → cancel and place new
            if (existing->side != side) {
                bool cancelled = cancel_pending(symbol);
                if (!cancelled) {
                    return {}; // Can't place new order — old one still stuck
                }
            }
            // Price drifted → cancel and replace
            else if (should_replace(symbol, limit_price)) {
                bool cancelled = cancel_pending(symbol);
                if (!cancelled) {
                    return {}; // Can't replace — old order still stuck
                }
            }
            // Otherwise keep existing
            else {
                return {}; // Keep current limit order
            }
        }

        // Step 6: Build OrderRequest for new limit
        double qty = signal.suggested_qty;

        // Position check for sells
        if (side == Side::Sell && engine_) {
            // Note: position check will be done by ExecutionEngine
            // We just pass the qty here
        }

        OrderRequest req;
        req.symbol = symbol;
        req.side = side;
        req.type = OrderType::Limit;
        req.qty = qty;
        req.limit_price = limit_price;
        req.venue = Venue::Spot;
        req.reason = "limit_exec";
        req.source_stage = "SpotLimit";

        return {req};
    }

    std::string_view name() const override { return "SpotLimit"; }

    /// Cancel all pending limit orders
    void cancel_all() {
        if (!engine_)
            return;

        for (auto& pending : pending_) {
            if (pending.active) {
                int resolved = engine_->cancel_pending_for_symbol(pending.symbol);
                if (resolved > 0) {
                    pending.clear();
                }
                // If failed, leave active — recover_stuck_orders will handle
            }
        }
    }

    /// Notify stage that an order was filled
    void on_fill(uint64_t order_id, Symbol symbol) {
        for (auto& pending : pending_) {
            if (pending.active && pending.order_id == order_id && pending.symbol == symbol) {
                pending.clear();
                return;
            }
        }
    }

    /// Track a pending limit order
    void track_pending(Symbol symbol, uint64_t order_id, Side side, Price limit_price, double qty,
                       uint64_t submit_time_ns) {
        // Find existing slot for this symbol
        PendingLimit* slot = find_pending(symbol);
        if (slot) {
            // Update existing slot
            slot->order_id = order_id;
            slot->side = side;
            slot->limit_price = limit_price;
            slot->qty = qty;
            slot->submit_time_ns = submit_time_ns;
            slot->active = true;
            return;
        }

        // Find free slot
        for (auto& pending : pending_) {
            if (!pending.active) {
                pending.symbol = symbol;
                pending.order_id = order_id;
                pending.side = side;
                pending.limit_price = limit_price;
                pending.qty = qty;
                pending.submit_time_ns = submit_time_ns;
                pending.active = true;
                return;
            }
        }

        // No free slot - should not happen with 64 slots
    }

private:
    ExecutionEngine* engine_;
    Config config_;
    std::array<PendingLimit, 64> pending_{};

    /// Find pending order for symbol
    PendingLimit* find_pending(Symbol symbol) {
        for (auto& pending : pending_) {
            if (pending.active && pending.symbol == symbol) {
                return &pending;
            }
        }
        return nullptr;
    }

    /// Cancel pending order for symbol
    /// Returns true if order confirmed gone (Success/NotFound)
    /// Returns false if cancel failed (order may still be active)
    bool cancel_pending(Symbol symbol) {
        if (!engine_)
            return false;

        PendingLimit* pending = find_pending(symbol);
        if (!pending)
            return true; // Nothing to cancel

        int resolved = engine_->cancel_pending_for_symbol(symbol);
        if (resolved > 0) {
            pending->clear();
            return true;
        } else {
            // Cancel failed — DON'T clear local state
            // Order may still be on exchange
            // recover_stuck_orders() will handle retry
            return false;
        }
    }

    /// Cancel stale pending orders (timeout check)
    void cancel_stale_pending(Symbol symbol) {
        PendingLimit* pending = find_pending(symbol);
        if (!pending)
            return;

        uint64_t now = util::now_ns();
        uint64_t age = now - pending->submit_time_ns;

        if (age > config_.timeout_ns) {
            cancel_pending(symbol);
        }
    }

    /// Calculate limit price based on execution score
    ///
    /// aggression = 1.0 - clamp(exec_score / 50.0, 0.0, 0.8)
    /// For Buy:  limit_price = bid + spread * aggression
    /// For Sell: limit_price = ask - spread * aggression
    Price calculate_limit_price(const ExecutionContext& ctx, Side side, double exec_score) {
        Price bid = ctx.market.bid;
        Price ask = ctx.market.ask;
        Price spread = ask - bid;

        // Calculate aggression level
        double aggression = 1.0 - std::clamp(exec_score / 50.0, 0.0, 0.8);

        // Calculate limit price
        Price limit_price;
        if (side == Side::Buy) {
            limit_price = bid + static_cast<Price>(spread * aggression);
        } else {
            limit_price = ask - static_cast<Price>(spread * aggression);
        }

        return limit_price;
    }

    /// Check if pending order should be replaced due to price drift
    bool should_replace(Symbol symbol, Price new_price) const {
        for (const auto& pending : pending_) {
            if (pending.active && pending.symbol == symbol) {
                // Calculate drift in bps
                Price old_price = pending.limit_price;
                if (old_price == 0)
                    return false;

                double drift_bps =
                    std::abs(static_cast<double>(new_price - old_price)) / static_cast<double>(old_price) * 10000.0;

                return drift_bps > config_.replace_threshold_bps;
            }
        }
        return false;
    }
};

} // namespace execution
} // namespace hft
