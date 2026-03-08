#pragma once

#include "../metrics/combined_metrics.hpp"
#include "../metrics/futures_metrics.hpp"
#include "../metrics/order_book_metrics.hpp"
#include "../metrics/order_flow_metrics.hpp"
#include "../metrics/trade_stream_metrics.hpp"
#include "regime_detector.hpp"

namespace hft {
namespace strategy {

/**
 * Read-only metrics context passed to strategies.
 *
 * Contains const pointers to per-symbol metrics instances.
 * nullptr means that metric source is unavailable.
 * Strategy must null-check before use.
 */
struct MetricsContext {
    const TradeStreamMetrics* trade = nullptr;
    const OrderBookMetrics* book = nullptr;
    const OrderFlowMetrics<20>* flow = nullptr;
    const CombinedMetrics* combined = nullptr;
    const FuturesMetrics* futures = nullptr;

    // Regime detection (Phase 5.0)
    MarketRegime regime = MarketRegime::Unknown;
    double regime_confidence = 0.0;

    /// Check if any metrics are available
    bool has_any() const { return trade || book || flow || combined || futures; }

    /// Check if all spot metrics are available
    bool has_spot() const { return trade && book && flow && combined; }

    /// Check if futures metrics are available
    bool has_futures() const { return futures != nullptr; }

    /// Check if regime information is available
    bool has_regime() const { return regime != MarketRegime::Unknown; }
};

} // namespace strategy
} // namespace hft
