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
     * @brief Update spot BBO for futures basis calculation
     */
    void on_spot_bbo(Symbol id, double bid, double ask, uint64_t timestamp_us) {
        if (id >= MAX_SYMBOLS)
            return;

        futures_[id].on_spot_bbo(bid, ask, timestamp_us);

        write_snapshot(id);
        check_thresholds(id);
    }

    /**
     * @brief Update futures BBO
     */
    void on_futures_bbo(Symbol id, double bid, double ask, uint64_t timestamp_us) {
        if (id >= MAX_SYMBOLS)
            return;

        futures_[id].on_futures_bbo(bid, ask, timestamp_us);

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
     * @brief Write ALL metrics to shared memory snapshot (326 fields)
     *
     * Writes all computed metrics for tuner/ML consumption:
     * - TradeStreamMetrics: 26 fields × 5 windows = 130
     * - OrderBookMetrics: 17 fields = 17
     * - OrderFlowMetrics: 21 fields × 4 windows = 84
     * - CombinedMetrics: 7 fields × 5 windows = 35
     * - FuturesMetrics: 11 fields × 5 windows + 1 = 56
     * - Regime: 4 fields
     *
     * Uses relaxed memory order (single writer, multiple readers).
     */
    void write_snapshot(Symbol id) {
        if (!shared_snap_ || id >= MAX_SYMBOLS)
            return;

        auto& sym = shared_snap_->symbols[id];

        // Update count and timestamp
        sym.update_count.fetch_add(1, std::memory_order_relaxed);
        sym.last_update_ns.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                                 std::memory_order_relaxed);

// Helper macros for writing fields (scaling doubles to fixed-point int64_t)
#define WRITE_METRIC(field, value) sym.field.store(static_cast<int64_t>((value)*1e8), std::memory_order_relaxed)
#define WRITE_COUNT(field, value) sym.field.store(static_cast<int32_t>(value), std::memory_order_relaxed)
#define WRITE_BOOL(field, value) sym.field.store((value) ? 1 : 0, std::memory_order_relaxed)

        // ================================================================
        // TradeStreamMetrics (26 fields × 5 windows = 130)
        // ================================================================
        {
            auto m = trade_[id].get_metrics(TradeWindow::W1s);
            WRITE_METRIC(trade_w1s_buy_volume_x8, m.buy_volume);
            WRITE_METRIC(trade_w1s_sell_volume_x8, m.sell_volume);
            WRITE_METRIC(trade_w1s_total_volume_x8, m.total_volume);
            WRITE_METRIC(trade_w1s_delta_x8, m.delta);
            WRITE_METRIC(trade_w1s_cumulative_delta_x8, m.cumulative_delta);
            WRITE_METRIC(trade_w1s_buy_ratio_x8, m.buy_ratio);
            WRITE_COUNT(trade_w1s_total_trades, m.total_trades);
            WRITE_COUNT(trade_w1s_buy_trades, m.buy_trades);
            WRITE_COUNT(trade_w1s_sell_trades, m.sell_trades);
            WRITE_COUNT(trade_w1s_large_trades, m.large_trades);
            WRITE_METRIC(trade_w1s_vwap_x8, m.vwap);
            WRITE_COUNT(trade_w1s_high, static_cast<int32_t>(m.high));
            WRITE_COUNT(trade_w1s_low, static_cast<int32_t>(m.low));
            WRITE_METRIC(trade_w1s_price_velocity_x8, m.price_velocity);
            WRITE_METRIC(trade_w1s_realized_volatility_x8, m.realized_volatility);
        }
        {
            auto m = trade_[id].get_metrics(TradeWindow::W5s);
            WRITE_METRIC(trade_w5s_buy_volume_x8, m.buy_volume);
            WRITE_METRIC(trade_w5s_sell_volume_x8, m.sell_volume);
            WRITE_METRIC(trade_w5s_total_volume_x8, m.total_volume);
            WRITE_METRIC(trade_w5s_delta_x8, m.delta);
            WRITE_METRIC(trade_w5s_cumulative_delta_x8, m.cumulative_delta);
            WRITE_METRIC(trade_w5s_buy_ratio_x8, m.buy_ratio);
            WRITE_COUNT(trade_w5s_total_trades, m.total_trades);
            WRITE_COUNT(trade_w5s_buy_trades, m.buy_trades);
            WRITE_COUNT(trade_w5s_sell_trades, m.sell_trades);
            WRITE_COUNT(trade_w5s_large_trades, m.large_trades);
            WRITE_METRIC(trade_w5s_vwap_x8, m.vwap);
            WRITE_COUNT(trade_w5s_high, static_cast<int32_t>(m.high));
            WRITE_COUNT(trade_w5s_low, static_cast<int32_t>(m.low));
            WRITE_METRIC(trade_w5s_price_velocity_x8, m.price_velocity);
            WRITE_METRIC(trade_w5s_realized_volatility_x8, m.realized_volatility);
        }
        {
            auto m = trade_[id].get_metrics(TradeWindow::W10s);
            WRITE_METRIC(trade_w10s_buy_volume_x8, m.buy_volume);
            WRITE_METRIC(trade_w10s_sell_volume_x8, m.sell_volume);
            WRITE_METRIC(trade_w10s_total_volume_x8, m.total_volume);
            WRITE_METRIC(trade_w10s_delta_x8, m.delta);
            WRITE_METRIC(trade_w10s_cumulative_delta_x8, m.cumulative_delta);
            WRITE_METRIC(trade_w10s_buy_ratio_x8, m.buy_ratio);
            WRITE_COUNT(trade_w10s_total_trades, m.total_trades);
            WRITE_COUNT(trade_w10s_buy_trades, m.buy_trades);
            WRITE_COUNT(trade_w10s_sell_trades, m.sell_trades);
            WRITE_COUNT(trade_w10s_large_trades, m.large_trades);
            WRITE_METRIC(trade_w10s_vwap_x8, m.vwap);
            WRITE_COUNT(trade_w10s_high, static_cast<int32_t>(m.high));
            WRITE_COUNT(trade_w10s_low, static_cast<int32_t>(m.low));
            WRITE_METRIC(trade_w10s_price_velocity_x8, m.price_velocity);
            WRITE_METRIC(trade_w10s_realized_volatility_x8, m.realized_volatility);
        }
        {
            auto m = trade_[id].get_metrics(TradeWindow::W30s);
            WRITE_METRIC(trade_w30s_buy_volume_x8, m.buy_volume);
            WRITE_METRIC(trade_w30s_sell_volume_x8, m.sell_volume);
            WRITE_METRIC(trade_w30s_total_volume_x8, m.total_volume);
            WRITE_METRIC(trade_w30s_delta_x8, m.delta);
            WRITE_METRIC(trade_w30s_cumulative_delta_x8, m.cumulative_delta);
            WRITE_METRIC(trade_w30s_buy_ratio_x8, m.buy_ratio);
            WRITE_COUNT(trade_w30s_total_trades, m.total_trades);
            WRITE_COUNT(trade_w30s_buy_trades, m.buy_trades);
            WRITE_COUNT(trade_w30s_sell_trades, m.sell_trades);
            WRITE_COUNT(trade_w30s_large_trades, m.large_trades);
            WRITE_METRIC(trade_w30s_vwap_x8, m.vwap);
            WRITE_COUNT(trade_w30s_high, static_cast<int32_t>(m.high));
            WRITE_COUNT(trade_w30s_low, static_cast<int32_t>(m.low));
            WRITE_METRIC(trade_w30s_price_velocity_x8, m.price_velocity);
            WRITE_METRIC(trade_w30s_realized_volatility_x8, m.realized_volatility);
        }
        {
            auto m = trade_[id].get_metrics(TradeWindow::W1min);
            WRITE_METRIC(trade_w1min_buy_volume_x8, m.buy_volume);
            WRITE_METRIC(trade_w1min_sell_volume_x8, m.sell_volume);
            WRITE_METRIC(trade_w1min_total_volume_x8, m.total_volume);
            WRITE_METRIC(trade_w1min_delta_x8, m.delta);
            WRITE_METRIC(trade_w1min_cumulative_delta_x8, m.cumulative_delta);
            WRITE_METRIC(trade_w1min_buy_ratio_x8, m.buy_ratio);
            WRITE_COUNT(trade_w1min_total_trades, m.total_trades);
            WRITE_COUNT(trade_w1min_buy_trades, m.buy_trades);
            WRITE_COUNT(trade_w1min_sell_trades, m.sell_trades);
            WRITE_COUNT(trade_w1min_large_trades, m.large_trades);
            WRITE_METRIC(trade_w1min_vwap_x8, m.vwap);
            WRITE_COUNT(trade_w1min_high, static_cast<int32_t>(m.high));
            WRITE_COUNT(trade_w1min_low, static_cast<int32_t>(m.low));
            WRITE_METRIC(trade_w1min_price_velocity_x8, m.price_velocity);
            WRITE_METRIC(trade_w1min_realized_volatility_x8, m.realized_volatility);
        }

        // ================================================================
        // OrderBookMetrics (17 fields × 1 = 17)
        // ================================================================
        {
            auto m = book_[id].get_metrics();
            WRITE_METRIC(book_current_spread_x8, m.spread);
            WRITE_METRIC(book_current_spread_bps_x8, m.spread_bps);
            WRITE_METRIC(book_current_mid_price_x8, m.mid_price);
            WRITE_METRIC(book_current_bid_depth_5_x8, m.bid_depth_5);
            WRITE_METRIC(book_current_bid_depth_10_x8, m.bid_depth_10);
            WRITE_METRIC(book_current_bid_depth_20_x8, m.bid_depth_20);
            WRITE_METRIC(book_current_ask_depth_5_x8, m.ask_depth_5);
            WRITE_METRIC(book_current_ask_depth_10_x8, m.ask_depth_10);
            WRITE_METRIC(book_current_ask_depth_20_x8, m.ask_depth_20);
            WRITE_METRIC(book_current_imbalance_5_x8, m.imbalance_5);
            WRITE_METRIC(book_current_imbalance_10_x8, m.imbalance_10);
            WRITE_METRIC(book_current_imbalance_20_x8, m.imbalance_20);
            WRITE_METRIC(book_current_top_imbalance_x8, m.top_imbalance);
            sym.book_current_best_bid.store(m.best_bid, std::memory_order_relaxed);
            sym.book_current_best_ask.store(m.best_ask, std::memory_order_relaxed);
            sym.book_current_best_bid_qty.store(m.best_bid_qty, std::memory_order_relaxed);
            sym.book_current_best_ask_qty.store(m.best_ask_qty, std::memory_order_relaxed);
        }

        // ================================================================
        // OrderFlowMetrics (21 fields × 4 windows = 84)
        // ================================================================
        {
            auto m = flow_[id].get_metrics(Window::SEC_1);
            WRITE_METRIC(flow_sec1_bid_volume_added_x8, m.bid_volume_added);
            WRITE_METRIC(flow_sec1_ask_volume_added_x8, m.ask_volume_added);
            WRITE_METRIC(flow_sec1_bid_volume_removed_x8, m.bid_volume_removed);
            WRITE_METRIC(flow_sec1_ask_volume_removed_x8, m.ask_volume_removed);
            WRITE_METRIC(flow_sec1_estimated_bid_cancel_volume_x8, m.estimated_bid_cancel_volume);
            WRITE_METRIC(flow_sec1_estimated_ask_cancel_volume_x8, m.estimated_ask_cancel_volume);
            WRITE_METRIC(flow_sec1_cancel_ratio_bid_x8, m.cancel_ratio_bid);
            WRITE_METRIC(flow_sec1_cancel_ratio_ask_x8, m.cancel_ratio_ask);
            WRITE_METRIC(flow_sec1_bid_depth_velocity_x8, m.bid_depth_velocity);
            WRITE_METRIC(flow_sec1_ask_depth_velocity_x8, m.ask_depth_velocity);
            WRITE_METRIC(flow_sec1_bid_additions_per_sec_x8, m.bid_additions_per_sec);
            WRITE_METRIC(flow_sec1_ask_additions_per_sec_x8, m.ask_additions_per_sec);
            WRITE_METRIC(flow_sec1_bid_removals_per_sec_x8, m.bid_removals_per_sec);
            WRITE_METRIC(flow_sec1_ask_removals_per_sec_x8, m.ask_removals_per_sec);
            WRITE_METRIC(flow_sec1_avg_bid_level_lifetime_us_x8, m.avg_bid_level_lifetime_us);
            WRITE_METRIC(flow_sec1_avg_ask_level_lifetime_us_x8, m.avg_ask_level_lifetime_us);
            WRITE_METRIC(flow_sec1_short_lived_bid_ratio_x8, m.short_lived_bid_ratio);
            WRITE_METRIC(flow_sec1_short_lived_ask_ratio_x8, m.short_lived_ask_ratio);
            WRITE_COUNT(flow_sec1_book_update_count, m.book_update_count);
            WRITE_COUNT(flow_sec1_bid_level_changes, m.bid_level_changes);
            WRITE_COUNT(flow_sec1_ask_level_changes, m.ask_level_changes);
        }
        {
            auto m = flow_[id].get_metrics(Window::SEC_5);
            WRITE_METRIC(flow_sec5_bid_volume_added_x8, m.bid_volume_added);
            WRITE_METRIC(flow_sec5_ask_volume_added_x8, m.ask_volume_added);
            WRITE_METRIC(flow_sec5_bid_volume_removed_x8, m.bid_volume_removed);
            WRITE_METRIC(flow_sec5_ask_volume_removed_x8, m.ask_volume_removed);
            WRITE_METRIC(flow_sec5_estimated_bid_cancel_volume_x8, m.estimated_bid_cancel_volume);
            WRITE_METRIC(flow_sec5_estimated_ask_cancel_volume_x8, m.estimated_ask_cancel_volume);
            WRITE_METRIC(flow_sec5_cancel_ratio_bid_x8, m.cancel_ratio_bid);
            WRITE_METRIC(flow_sec5_cancel_ratio_ask_x8, m.cancel_ratio_ask);
            WRITE_METRIC(flow_sec5_bid_depth_velocity_x8, m.bid_depth_velocity);
            WRITE_METRIC(flow_sec5_ask_depth_velocity_x8, m.ask_depth_velocity);
            WRITE_METRIC(flow_sec5_bid_additions_per_sec_x8, m.bid_additions_per_sec);
            WRITE_METRIC(flow_sec5_ask_additions_per_sec_x8, m.ask_additions_per_sec);
            WRITE_METRIC(flow_sec5_bid_removals_per_sec_x8, m.bid_removals_per_sec);
            WRITE_METRIC(flow_sec5_ask_removals_per_sec_x8, m.ask_removals_per_sec);
            WRITE_METRIC(flow_sec5_avg_bid_level_lifetime_us_x8, m.avg_bid_level_lifetime_us);
            WRITE_METRIC(flow_sec5_avg_ask_level_lifetime_us_x8, m.avg_ask_level_lifetime_us);
            WRITE_METRIC(flow_sec5_short_lived_bid_ratio_x8, m.short_lived_bid_ratio);
            WRITE_METRIC(flow_sec5_short_lived_ask_ratio_x8, m.short_lived_ask_ratio);
            WRITE_COUNT(flow_sec5_book_update_count, m.book_update_count);
            WRITE_COUNT(flow_sec5_bid_level_changes, m.bid_level_changes);
            WRITE_COUNT(flow_sec5_ask_level_changes, m.ask_level_changes);
        }
        {
            auto m = flow_[id].get_metrics(Window::SEC_10);
            WRITE_METRIC(flow_sec10_bid_volume_added_x8, m.bid_volume_added);
            WRITE_METRIC(flow_sec10_ask_volume_added_x8, m.ask_volume_added);
            WRITE_METRIC(flow_sec10_bid_volume_removed_x8, m.bid_volume_removed);
            WRITE_METRIC(flow_sec10_ask_volume_removed_x8, m.ask_volume_removed);
            WRITE_METRIC(flow_sec10_estimated_bid_cancel_volume_x8, m.estimated_bid_cancel_volume);
            WRITE_METRIC(flow_sec10_estimated_ask_cancel_volume_x8, m.estimated_ask_cancel_volume);
            WRITE_METRIC(flow_sec10_cancel_ratio_bid_x8, m.cancel_ratio_bid);
            WRITE_METRIC(flow_sec10_cancel_ratio_ask_x8, m.cancel_ratio_ask);
            WRITE_METRIC(flow_sec10_bid_depth_velocity_x8, m.bid_depth_velocity);
            WRITE_METRIC(flow_sec10_ask_depth_velocity_x8, m.ask_depth_velocity);
            WRITE_METRIC(flow_sec10_bid_additions_per_sec_x8, m.bid_additions_per_sec);
            WRITE_METRIC(flow_sec10_ask_additions_per_sec_x8, m.ask_additions_per_sec);
            WRITE_METRIC(flow_sec10_bid_removals_per_sec_x8, m.bid_removals_per_sec);
            WRITE_METRIC(flow_sec10_ask_removals_per_sec_x8, m.ask_removals_per_sec);
            WRITE_METRIC(flow_sec10_avg_bid_level_lifetime_us_x8, m.avg_bid_level_lifetime_us);
            WRITE_METRIC(flow_sec10_avg_ask_level_lifetime_us_x8, m.avg_ask_level_lifetime_us);
            WRITE_METRIC(flow_sec10_short_lived_bid_ratio_x8, m.short_lived_bid_ratio);
            WRITE_METRIC(flow_sec10_short_lived_ask_ratio_x8, m.short_lived_ask_ratio);
            WRITE_COUNT(flow_sec10_book_update_count, m.book_update_count);
            WRITE_COUNT(flow_sec10_bid_level_changes, m.bid_level_changes);
            WRITE_COUNT(flow_sec10_ask_level_changes, m.ask_level_changes);
        }
        {
            auto m = flow_[id].get_metrics(Window::SEC_30);
            WRITE_METRIC(flow_sec30_bid_volume_added_x8, m.bid_volume_added);
            WRITE_METRIC(flow_sec30_ask_volume_added_x8, m.ask_volume_added);
            WRITE_METRIC(flow_sec30_bid_volume_removed_x8, m.bid_volume_removed);
            WRITE_METRIC(flow_sec30_ask_volume_removed_x8, m.ask_volume_removed);
            WRITE_METRIC(flow_sec30_estimated_bid_cancel_volume_x8, m.estimated_bid_cancel_volume);
            WRITE_METRIC(flow_sec30_estimated_ask_cancel_volume_x8, m.estimated_ask_cancel_volume);
            WRITE_METRIC(flow_sec30_cancel_ratio_bid_x8, m.cancel_ratio_bid);
            WRITE_METRIC(flow_sec30_cancel_ratio_ask_x8, m.cancel_ratio_ask);
            WRITE_METRIC(flow_sec30_bid_depth_velocity_x8, m.bid_depth_velocity);
            WRITE_METRIC(flow_sec30_ask_depth_velocity_x8, m.ask_depth_velocity);
            WRITE_METRIC(flow_sec30_bid_additions_per_sec_x8, m.bid_additions_per_sec);
            WRITE_METRIC(flow_sec30_ask_additions_per_sec_x8, m.ask_additions_per_sec);
            WRITE_METRIC(flow_sec30_bid_removals_per_sec_x8, m.bid_removals_per_sec);
            WRITE_METRIC(flow_sec30_ask_removals_per_sec_x8, m.ask_removals_per_sec);
            WRITE_METRIC(flow_sec30_avg_bid_level_lifetime_us_x8, m.avg_bid_level_lifetime_us);
            WRITE_METRIC(flow_sec30_avg_ask_level_lifetime_us_x8, m.avg_ask_level_lifetime_us);
            WRITE_METRIC(flow_sec30_short_lived_bid_ratio_x8, m.short_lived_bid_ratio);
            WRITE_METRIC(flow_sec30_short_lived_ask_ratio_x8, m.short_lived_ask_ratio);
            WRITE_COUNT(flow_sec30_book_update_count, m.book_update_count);
            WRITE_COUNT(flow_sec30_bid_level_changes, m.bid_level_changes);
            WRITE_COUNT(flow_sec30_ask_level_changes, m.ask_level_changes);
        }

        // ================================================================
        // CombinedMetrics (7 fields × 5 windows = 35)
        // ================================================================
        if (combined_[id]) {
            {
                auto m = combined_[id]->get_metrics(CombinedMetrics::Window::SEC_1);
                WRITE_METRIC(combined_sec1_trade_to_depth_ratio_x8, m.trade_to_depth_ratio);
                WRITE_METRIC(combined_sec1_absorption_ratio_bid_x8, m.absorption_ratio_bid);
                WRITE_METRIC(combined_sec1_absorption_ratio_ask_x8, m.absorption_ratio_ask);
                WRITE_METRIC(combined_sec1_spread_mean_x8, m.spread_mean);
                WRITE_METRIC(combined_sec1_spread_max_x8, m.spread_max);
                WRITE_METRIC(combined_sec1_spread_min_x8, m.spread_min);
                WRITE_METRIC(combined_sec1_spread_volatility_x8, m.spread_volatility);
            }
            {
                auto m = combined_[id]->get_metrics(CombinedMetrics::Window::SEC_5);
                WRITE_METRIC(combined_sec5_trade_to_depth_ratio_x8, m.trade_to_depth_ratio);
                WRITE_METRIC(combined_sec5_absorption_ratio_bid_x8, m.absorption_ratio_bid);
                WRITE_METRIC(combined_sec5_absorption_ratio_ask_x8, m.absorption_ratio_ask);
                WRITE_METRIC(combined_sec5_spread_mean_x8, m.spread_mean);
                WRITE_METRIC(combined_sec5_spread_max_x8, m.spread_max);
                WRITE_METRIC(combined_sec5_spread_min_x8, m.spread_min);
                WRITE_METRIC(combined_sec5_spread_volatility_x8, m.spread_volatility);
            }
            {
                auto m = combined_[id]->get_metrics(CombinedMetrics::Window::SEC_10);
                WRITE_METRIC(combined_sec10_trade_to_depth_ratio_x8, m.trade_to_depth_ratio);
                WRITE_METRIC(combined_sec10_absorption_ratio_bid_x8, m.absorption_ratio_bid);
                WRITE_METRIC(combined_sec10_absorption_ratio_ask_x8, m.absorption_ratio_ask);
                WRITE_METRIC(combined_sec10_spread_mean_x8, m.spread_mean);
                WRITE_METRIC(combined_sec10_spread_max_x8, m.spread_max);
                WRITE_METRIC(combined_sec10_spread_min_x8, m.spread_min);
                WRITE_METRIC(combined_sec10_spread_volatility_x8, m.spread_volatility);
            }
            {
                auto m = combined_[id]->get_metrics(CombinedMetrics::Window::SEC_30);
                WRITE_METRIC(combined_sec30_trade_to_depth_ratio_x8, m.trade_to_depth_ratio);
                WRITE_METRIC(combined_sec30_absorption_ratio_bid_x8, m.absorption_ratio_bid);
                WRITE_METRIC(combined_sec30_absorption_ratio_ask_x8, m.absorption_ratio_ask);
                WRITE_METRIC(combined_sec30_spread_mean_x8, m.spread_mean);
                WRITE_METRIC(combined_sec30_spread_max_x8, m.spread_max);
                WRITE_METRIC(combined_sec30_spread_min_x8, m.spread_min);
                WRITE_METRIC(combined_sec30_spread_volatility_x8, m.spread_volatility);
            }
            {
                auto m = combined_[id]->get_metrics(CombinedMetrics::Window::MIN_1);
                WRITE_METRIC(combined_min1_trade_to_depth_ratio_x8, m.trade_to_depth_ratio);
                WRITE_METRIC(combined_min1_absorption_ratio_bid_x8, m.absorption_ratio_bid);
                WRITE_METRIC(combined_min1_absorption_ratio_ask_x8, m.absorption_ratio_ask);
                WRITE_METRIC(combined_min1_spread_mean_x8, m.spread_mean);
                WRITE_METRIC(combined_min1_spread_max_x8, m.spread_max);
                WRITE_METRIC(combined_min1_spread_min_x8, m.spread_min);
                WRITE_METRIC(combined_min1_spread_volatility_x8, m.spread_volatility);
            }
        }

        // ================================================================
        // FuturesMetrics (11 fields × 5 windows + 1 = 56)
        // ================================================================
        {
            auto m = futures_[id].get_metrics(FuturesWindow::W1s);
            WRITE_METRIC(futures_w1s_funding_rate_x8, m.funding_rate);
            WRITE_METRIC(futures_w1s_funding_rate_ema_x8, m.funding_rate_ema);
            WRITE_BOOL(futures_w1s_funding_rate_extreme, m.funding_rate_extreme);
            WRITE_METRIC(futures_w1s_basis_x8, m.basis);
            WRITE_METRIC(futures_w1s_basis_bps_x8, m.basis_bps);
            WRITE_METRIC(futures_w1s_basis_ema_x8, m.basis_ema);
            WRITE_METRIC(futures_w1s_liquidation_volume_x8, m.liquidation_volume);
            WRITE_COUNT(futures_w1s_liquidation_count, m.liquidation_count);
            WRITE_METRIC(futures_w1s_long_liquidation_volume_x8, m.long_liquidation_volume);
            WRITE_METRIC(futures_w1s_short_liquidation_volume_x8, m.short_liquidation_volume);
            WRITE_METRIC(futures_w1s_liquidation_imbalance_x8, m.liquidation_imbalance);
        }
        {
            auto m = futures_[id].get_metrics(FuturesWindow::W5s);
            WRITE_METRIC(futures_w5s_funding_rate_x8, m.funding_rate);
            WRITE_METRIC(futures_w5s_funding_rate_ema_x8, m.funding_rate_ema);
            WRITE_BOOL(futures_w5s_funding_rate_extreme, m.funding_rate_extreme);
            WRITE_METRIC(futures_w5s_basis_x8, m.basis);
            WRITE_METRIC(futures_w5s_basis_bps_x8, m.basis_bps);
            WRITE_METRIC(futures_w5s_basis_ema_x8, m.basis_ema);
            WRITE_METRIC(futures_w5s_liquidation_volume_x8, m.liquidation_volume);
            WRITE_COUNT(futures_w5s_liquidation_count, m.liquidation_count);
            WRITE_METRIC(futures_w5s_long_liquidation_volume_x8, m.long_liquidation_volume);
            WRITE_METRIC(futures_w5s_short_liquidation_volume_x8, m.short_liquidation_volume);
            WRITE_METRIC(futures_w5s_liquidation_imbalance_x8, m.liquidation_imbalance);
        }
        {
            auto m = futures_[id].get_metrics(FuturesWindow::W10s);
            WRITE_METRIC(futures_w10s_funding_rate_x8, m.funding_rate);
            WRITE_METRIC(futures_w10s_funding_rate_ema_x8, m.funding_rate_ema);
            WRITE_BOOL(futures_w10s_funding_rate_extreme, m.funding_rate_extreme);
            WRITE_METRIC(futures_w10s_basis_x8, m.basis);
            WRITE_METRIC(futures_w10s_basis_bps_x8, m.basis_bps);
            WRITE_METRIC(futures_w10s_basis_ema_x8, m.basis_ema);
            WRITE_METRIC(futures_w10s_liquidation_volume_x8, m.liquidation_volume);
            WRITE_COUNT(futures_w10s_liquidation_count, m.liquidation_count);
            WRITE_METRIC(futures_w10s_long_liquidation_volume_x8, m.long_liquidation_volume);
            WRITE_METRIC(futures_w10s_short_liquidation_volume_x8, m.short_liquidation_volume);
            WRITE_METRIC(futures_w10s_liquidation_imbalance_x8, m.liquidation_imbalance);
        }
        {
            auto m = futures_[id].get_metrics(FuturesWindow::W30s);
            WRITE_METRIC(futures_w30s_funding_rate_x8, m.funding_rate);
            WRITE_METRIC(futures_w30s_funding_rate_ema_x8, m.funding_rate_ema);
            WRITE_BOOL(futures_w30s_funding_rate_extreme, m.funding_rate_extreme);
            WRITE_METRIC(futures_w30s_basis_x8, m.basis);
            WRITE_METRIC(futures_w30s_basis_bps_x8, m.basis_bps);
            WRITE_METRIC(futures_w30s_basis_ema_x8, m.basis_ema);
            WRITE_METRIC(futures_w30s_liquidation_volume_x8, m.liquidation_volume);
            WRITE_COUNT(futures_w30s_liquidation_count, m.liquidation_count);
            WRITE_METRIC(futures_w30s_long_liquidation_volume_x8, m.long_liquidation_volume);
            WRITE_METRIC(futures_w30s_short_liquidation_volume_x8, m.short_liquidation_volume);
            WRITE_METRIC(futures_w30s_liquidation_imbalance_x8, m.liquidation_imbalance);
        }
        {
            auto m = futures_[id].get_metrics(FuturesWindow::W1min);
            WRITE_METRIC(futures_w1min_funding_rate_x8, m.funding_rate);
            WRITE_METRIC(futures_w1min_funding_rate_ema_x8, m.funding_rate_ema);
            WRITE_BOOL(futures_w1min_funding_rate_extreme, m.funding_rate_extreme);
            WRITE_METRIC(futures_w1min_basis_x8, m.basis);
            WRITE_METRIC(futures_w1min_basis_bps_x8, m.basis_bps);
            WRITE_METRIC(futures_w1min_basis_ema_x8, m.basis_ema);
            WRITE_METRIC(futures_w1min_liquidation_volume_x8, m.liquidation_volume);
            WRITE_COUNT(futures_w1min_liquidation_count, m.liquidation_count);
            WRITE_METRIC(futures_w1min_long_liquidation_volume_x8, m.long_liquidation_volume);
            WRITE_METRIC(futures_w1min_short_liquidation_volume_x8, m.short_liquidation_volume);
            WRITE_METRIC(futures_w1min_liquidation_imbalance_x8, m.liquidation_imbalance);
        }
        // next_funding_time_ms is stored outside windowed metrics (same for all windows, read from W1s)
        sym.futures_next_funding_time_ms.store(futures_[id].get_metrics(FuturesWindow::W1s).next_funding_time_ms,
                                               std::memory_order_relaxed);

        // ================================================================
        // Regime (4 fields)
        // ================================================================
        sym.regime.store(static_cast<uint8_t>(regimes_[id].current_regime()), std::memory_order_relaxed);
        WRITE_METRIC(regime_current_confidence_x8, regimes_[id].confidence());
        // TODO: RegimeDetector doesn't track last_change_ns and change_count yet - add in future
        sym.regime_last_change_ns.store(0, std::memory_order_relaxed);
        sym.regime_change_count.store(0, std::memory_order_relaxed);

#undef WRITE_METRIC
#undef WRITE_COUNT
#undef WRITE_BOOL
    }
};

} // namespace hft::core
