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

namespace core {

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

        // Write to shared memory
        write_snapshot(id);

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
        // flow_[id].on_order_book_update() requires OrderBook, not BookSnapshot
        // So we skip flow update here - would need book reconstruction in production

        combined_[id]->update(timestamp_us);

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
     * @brief Write metrics to shared memory snapshot
     */
    void write_snapshot(Symbol id) {
        if (!shared_snap_ || id >= MAX_SYMBOLS)
            return;

        auto& sym = shared_snap_->symbols[id];

        // Get metrics for all windows
        auto trade_w1s = trade_[id].get_metrics(TradeWindow::W1s);
        auto trade_w5s = trade_[id].get_metrics(TradeWindow::W5s);
        auto book_m = book_[id].get_metrics();
        auto flow_sec1 = flow_[id].get_metrics(Window::SEC_1);
        auto comb_sec1 = combined_[id]->get_metrics(CombinedMetrics::Window::SEC_1);
        auto futures_w1s = futures_[id].get_metrics(FuturesWindow::W1s);
        auto regime = regimes_[id].current_regime();
        auto regime_conf = regimes_[id].confidence();

        // Helper to store double as int64_t scaled
        auto store_double = [](std::atomic<int64_t>& dest, double val) {
            dest.store(static_cast<int64_t>(val * METRICS_FIXED_POINT_SCALE), std::memory_order_relaxed);
        };

        // TradeStreamMetrics - w1s window (subset shown, full implementation needed)
        store_double(sym.trade_w1s_buy_volume_x8, trade_w1s.buy_volume);
        store_double(sym.trade_w1s_sell_volume_x8, trade_w1s.sell_volume);
        store_double(sym.trade_w1s_total_volume_x8, trade_w1s.total_volume);
        store_double(sym.trade_w1s_delta_x8, trade_w1s.delta);
        store_double(sym.trade_w1s_buy_ratio_x8, trade_w1s.buy_ratio);
        sym.trade_w1s_total_trades.store(trade_w1s.total_trades, std::memory_order_relaxed);

        // OrderBookMetrics
        store_double(sym.book_current_spread_bps_x8, book_m.spread_bps);
        store_double(sym.book_current_top_imbalance_x8, book_m.top_imbalance);
        sym.book_current_best_bid.store(book_m.best_bid, std::memory_order_relaxed);
        sym.book_current_best_ask.store(book_m.best_ask, std::memory_order_relaxed);

        // Regime
        sym.regime.store(static_cast<uint8_t>(regime), std::memory_order_relaxed);
        store_double(sym.regime_current_confidence_x8, regime_conf);

        sym.update_count.fetch_add(1, std::memory_order_relaxed);
        sym.last_update_ns.store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count(),
            std::memory_order_relaxed);

        // Full implementation would write ALL fields for all windows
        // (omitted for brevity - pattern shown above)
    }
};

} // namespace core
