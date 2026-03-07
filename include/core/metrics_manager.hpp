#pragma once

#include "../ipc/shared_metrics_snapshot.hpp"
#include "../metrics/combined_metrics.hpp"
#include "../metrics/futures_metrics.hpp"
#include "../metrics/order_book_metrics.hpp"
#include "../metrics/order_flow_metrics.hpp"
#include "../metrics/trade_stream_metrics.hpp"
#include "../orderbook.hpp"
#include "../strategy/metrics_context.hpp"
#include "../strategy/regime_detector.hpp"
#include "../types.hpp"

#include <array>
#include <functional>
#include <memory>

namespace hft::core {

using namespace hft;
using namespace hft::ipc;
using namespace hft::strategy;

/**
 * @brief Callback invoked when metrics change exceeds thresholds
 * @param symbol Symbol ID that changed
 */
using ChangeCallback = std::function<void(Symbol symbol)>;

/**
 * @brief Metrics thresholds for change detection
 *
 * Near-zero defaults trigger on almost every real change.
 * Future: ML-optimized per-symbol thresholds (Issue #2).
 */
struct MetricsThresholds {
    double spread_bps = 0.1;       // 0.1 bps change triggers
    double buy_ratio = 0.01;       // 1% change in buy ratio
    double basis_bps = 0.5;        // 0.5 bps basis change
    double funding_rate = 0.00001; // 0.001% funding rate change
    double volatility = 0.001;     // 0.1% volatility change
    double top_imbalance = 0.02;   // 2% top imbalance change
};

/**
 * @brief Metrics snapshot for threshold comparison
 */
struct MetricsSnapshot {
    double spread_bps = 0.0;
    double buy_ratio = 0.0;
    double basis_bps = 0.0;
    double funding_rate = 0.0;
    double volatility = 0.0;
    double top_imbalance = 0.0;
    MarketRegime regime = MarketRegime::Unknown;
};

/**
 * @brief Central coordinator for all metrics
 *
 * Owns metrics arrays (MAX_SYMBOLS = 64):
 * - TradeStreamMetrics
 * - OrderBookMetrics
 * - OrderFlowMetrics
 * - CombinedMetrics
 * - FuturesMetrics
 * - RegimeDetector
 *
 * Responsibilities:
 * 1. Update metrics from WS feeds (direct calls, no events)
 * 2. Check thresholds → fire change callback
 * 3. Write SharedMetricsSnapshot for tuner/ML
 * 4. Provide MetricsContext for strategy evaluation
 *
 * Thread safety: Single writer (trader main thread)
 */
class MetricsManager {
public:
    static constexpr size_t MAX_SYMBOLS = 64;

    MetricsManager() {
        // Initialize CombinedMetrics (needs references to other metrics)
        for (size_t i = 0; i < MAX_SYMBOLS; i++) {
            combined_[i] = std::make_unique<CombinedMetrics>(trade_[i], book_[i]);
        }
    }

    /**
     * @brief Set change callback (invoked when thresholds crossed)
     */
    void set_change_callback(ChangeCallback callback) { on_change_ = std::move(callback); }

    /**
     * @brief Set custom thresholds
     */
    void set_thresholds(const MetricsThresholds& thresholds) { thresholds_ = thresholds; }

    /**
     * @brief Set shared memory snapshot for tuner/ML
     */
    void set_shared_snapshot(SharedMetricsSnapshot* snap) { shared_snap_ = snap; }

    /**
     * @brief Set ticker string for a symbol (for shared memory)
     */
    void set_ticker(Symbol id, const char* ticker) {
        if (id >= MAX_SYMBOLS || !shared_snap_)
            return;
        std::strncpy(shared_snap_->symbols[id].ticker, ticker, 15);
        shared_snap_->symbols[id].ticker[15] = '\0';
    }

    /**
     * @brief Update from trade stream (direct call from WS)
     */
    void on_trade(Symbol id, double price, int qty, bool is_buy, uint64_t timestamp_us) {
        if (id >= MAX_SYMBOLS)
            return;

        Price price_scaled = static_cast<Price>(price);
        Quantity qty_scaled = static_cast<Quantity>(qty);

        trade_[id].on_trade(price_scaled, qty_scaled, is_buy, timestamp_us);
        flow_[id].on_trade(price_scaled, qty_scaled, timestamp_us);

        // Update combined metrics
        combined_[id]->update(timestamp_us);

        // Update regime detector
        regimes_[id].update(price); // Simplified - would use bid/ask in production

        // Write to shared memory (only if snapshot configured)
        if (shared_snap_) {
            write_snapshot(id);
        }

        // Check thresholds
        check_thresholds(id);
    }

    /**
     * @brief Update from depth snapshot (direct call from WS)
     */
    void on_depth(Symbol id, const BookSnapshot& snapshot, uint64_t timestamp_us) {
        if (id >= MAX_SYMBOLS)
            return;

        book_[id].on_depth_snapshot(snapshot, timestamp_us);
        flow_[id].on_depth_snapshot(snapshot, timestamp_us); // Flow metrics from depth

        combined_[id]->update(timestamp_us);

        // Update regime from mid price
        double mid = (snapshot.best_bid + snapshot.best_ask) / 2.0;
        regimes_[id].update(mid);

        write_snapshot(id);
        check_thresholds(id);
    }

    /**
     * @brief Update from mark price stream (futures)
     */
    void on_mark_price(Symbol id, double mark, double index, double funding, uint64_t next_funding_time_ms,
                       uint64_t timestamp_us) {
        if (id >= MAX_SYMBOLS)
            return;

        futures_[id].on_mark_price(mark, index, funding, next_funding_time_ms, timestamp_us);

        write_snapshot(id);
        check_thresholds(id);
    }

    /**
     * @brief Update from liquidation stream (futures)
     */
    void on_liquidation(Symbol id, Side side, double price, double qty, uint64_t timestamp_us) {
        if (id >= MAX_SYMBOLS)
            return;

        futures_[id].on_liquidation(side, price, qty, timestamp_us);

        write_snapshot(id);
        check_thresholds(id);
    }

    /**
     * @brief Get filled MetricsContext for strategy evaluation
     */
    MetricsContext context_for(Symbol id) const {
        if (id >= MAX_SYMBOLS)
            return MetricsContext{};

        MetricsContext ctx;
        ctx.trade = &trade_[id];
        ctx.book = &book_[id];
        ctx.flow = &flow_[id];
        ctx.combined = combined_[id].get();
        ctx.futures = &futures_[id];
        ctx.regime = regimes_[id].current_regime();
        ctx.regime_confidence = regimes_[id].confidence();

        return ctx;
    }

    /**
     * @brief Get raw metrics pointers (for advanced use cases)
     */
    const TradeStreamMetrics* trade(Symbol id) const { return (id < MAX_SYMBOLS) ? &trade_[id] : nullptr; }
    const OrderBookMetrics* book(Symbol id) const { return (id < MAX_SYMBOLS) ? &book_[id] : nullptr; }
    const OrderFlowMetrics<20>* flow(Symbol id) const { return (id < MAX_SYMBOLS) ? &flow_[id] : nullptr; }
    const CombinedMetrics* combined(Symbol id) const { return (id < MAX_SYMBOLS) ? combined_[id].get() : nullptr; }
    const FuturesMetrics* futures(Symbol id) const { return (id < MAX_SYMBOLS) ? &futures_[id] : nullptr; }

private:
    // Metrics arrays (one per symbol)
    std::array<TradeStreamMetrics, MAX_SYMBOLS> trade_;
    std::array<OrderBookMetrics, MAX_SYMBOLS> book_;
    std::array<OrderFlowMetrics<20>, MAX_SYMBOLS> flow_;
    std::array<std::unique_ptr<CombinedMetrics>, MAX_SYMBOLS> combined_;
    std::array<FuturesMetrics, MAX_SYMBOLS> futures_;
    std::array<RegimeDetector, MAX_SYMBOLS> regimes_;

    // Previous snapshots for threshold checking
    std::array<MetricsSnapshot, MAX_SYMBOLS> prev_snapshots_;

    // Callbacks and configuration
    ChangeCallback on_change_;
    MetricsThresholds thresholds_;
    SharedMetricsSnapshot* shared_snap_ = nullptr;

    /**
     * @brief Check if metrics change exceeds thresholds
     */
    void check_thresholds(Symbol id) {
        auto& prev = prev_snapshots_[id];

        // Get current snapshot
        auto book_m = book_[id].get_metrics();
        auto trade_m = trade_[id].get_metrics(TradeWindow::W1s);
        auto futures_m = futures_[id].get_metrics(FuturesWindow::W1s);
        auto regime = regimes_[id].current_regime();

        MetricsSnapshot curr;
        curr.spread_bps = book_m.spread_bps;
        curr.buy_ratio = trade_m.buy_ratio;
        curr.basis_bps = futures_m.basis_bps;
        curr.funding_rate = futures_m.funding_rate;
        curr.volatility = trade_m.realized_volatility;
        curr.top_imbalance = book_m.top_imbalance;
        curr.regime = regime;

        // Check if any threshold crossed
        bool changed = (std::abs(curr.spread_bps - prev.spread_bps) > thresholds_.spread_bps) ||
                       (std::abs(curr.buy_ratio - prev.buy_ratio) > thresholds_.buy_ratio) ||
                       (std::abs(curr.basis_bps - prev.basis_bps) > thresholds_.basis_bps) ||
                       (std::abs(curr.funding_rate - prev.funding_rate) > thresholds_.funding_rate) ||
                       (std::abs(curr.volatility - prev.volatility) > thresholds_.volatility) ||
                       (std::abs(curr.top_imbalance - prev.top_imbalance) > thresholds_.top_imbalance) ||
                       (curr.regime != prev.regime);

        if (changed) {
            prev = curr; // Update snapshot
            if (on_change_) {
                on_change_(id); // Fire callback
            }
        }
    }

    /**
     * @brief Write core metrics to shared memory snapshot
     *
     * Simplified implementation: writes only core fields that definitely exist.
     * TODO: Expand to full 326 fields after aligning with actual struct definitions.
     */
    void write_snapshot(Symbol id) {
        if (!shared_snap_ || id >= MAX_SYMBOLS)
            return;

        auto& sym = shared_snap_->symbols[id];

        // Increment update count
        sym.update_count.fetch_add(1, std::memory_order_relaxed);

        // Regime
        sym.regime.store(static_cast<uint8_t>(regimes_[id].current_regime()), std::memory_order_relaxed);
    }
};

} // namespace hft::core
