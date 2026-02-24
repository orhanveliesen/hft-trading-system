#pragma once

#include "../orderbook.hpp"
#include "../types.hpp"

#include <cmath>

namespace hft {

/**
 * OrderBookMetrics - Real-time order book depth and imbalance metrics
 *
 * Calculates:
 * - Spread (absolute and basis points)
 * - Depth within N bps (5, 10, 20 bps on each side)
 * - Imbalance ratios at different depths
 * - Top of book state
 *
 * Design:
 * - Header-only, zero allocation
 * - Uses OrderBook::get_snapshot() for efficient extraction
 * - Single pass through levels for all depth calculations
 *
 * Performance: < 5 μs per update
 */
class OrderBookMetrics {
public:
    OrderBookMetrics() = default;

    // Update metrics from order book snapshot
    void on_order_book_update(const OrderBook& book, uint64_t timestamp_us) {
        auto snapshot = book.get_snapshot(20);
        calculate_metrics(snapshot);
        last_update_us_ = timestamp_us;
    }

    struct Metrics {
        // Spread
        double spread = 0.0;
        double spread_bps = 0.0;
        double mid_price = 0.0;

        // Depth (volume within N bps)
        double bid_depth_5 = 0.0;
        double bid_depth_10 = 0.0;
        double bid_depth_20 = 0.0;
        double ask_depth_5 = 0.0;
        double ask_depth_10 = 0.0;
        double ask_depth_20 = 0.0;

        // Imbalance ratios
        double imbalance_5 = 0.0;
        double imbalance_10 = 0.0;
        double imbalance_20 = 0.0;
        double top_imbalance = 0.0;

        // Top of book
        Price best_bid = INVALID_PRICE;
        Price best_ask = INVALID_PRICE;
        Quantity best_bid_qty = 0;
        Quantity best_ask_qty = 0;
    };

    Metrics get_metrics() const { return metrics_; }

    void reset() {
        metrics_ = Metrics{};
        last_update_us_ = 0;
    }

private:
    Metrics metrics_;
    uint64_t last_update_us_ = 0;

    void calculate_metrics(const BookSnapshot& snapshot) {
        // Top of book
        metrics_.best_bid = snapshot.best_bid;
        metrics_.best_ask = snapshot.best_ask;
        metrics_.best_bid_qty = snapshot.best_bid_qty;
        metrics_.best_ask_qty = snapshot.best_ask_qty;

        // Spread (branchless, optimized for instruction pipelining)
        // Start independent computations that can execute in parallel
        // Cast to signed before subtraction to handle crossed book (negative spread)
        double spread =
            static_cast<double>(static_cast<int64_t>(snapshot.best_ask) - static_cast<int64_t>(snapshot.best_bid));
        double mid_price = (static_cast<double>(snapshot.best_bid) + static_cast<double>(snapshot.best_ask)) / 2.0;

        // While spread/mid_price compute, start comparison (independent of above)
        double valid =
            static_cast<double>((snapshot.best_bid != INVALID_PRICE) && (snapshot.best_ask != INVALID_PRICE));

        // Compute spread_bps (depends on mid_price, which should be ready)
        double spread_bps = (spread / std::max(mid_price, 1.0)) * 10000.0;

        // Compute bps_valid (uses valid and mid_price, both should be ready)
        double bps_valid = valid * static_cast<double>(mid_price > 0.0);

        // Apply masks (uses all computed values)
        metrics_.spread = spread * valid;
        metrics_.mid_price = mid_price * valid;
        metrics_.spread_bps = spread_bps * bps_valid;

        // Depth calculations (single pass per side)
        calculate_all_depths(snapshot.bid_levels, snapshot.bid_level_count, snapshot.best_bid, true,
                             metrics_.bid_depth_5, metrics_.bid_depth_10, metrics_.bid_depth_20);
        calculate_all_depths(snapshot.ask_levels, snapshot.ask_level_count, snapshot.best_ask, false,
                             metrics_.ask_depth_5, metrics_.ask_depth_10, metrics_.ask_depth_20);

        // Imbalance ratios
        metrics_.imbalance_5 = calculate_imbalance(metrics_.bid_depth_5, metrics_.ask_depth_5);
        metrics_.imbalance_10 = calculate_imbalance(metrics_.bid_depth_10, metrics_.ask_depth_10);
        metrics_.imbalance_20 = calculate_imbalance(metrics_.bid_depth_20, metrics_.ask_depth_20);
        metrics_.top_imbalance =
            calculate_imbalance(static_cast<double>(snapshot.best_bid_qty), static_cast<double>(snapshot.best_ask_qty));
    }

    // Calculate all depth thresholds in a single pass
    static void calculate_all_depths(const LevelInfo* levels, int level_count, Price best_price, bool is_bid,
                                     double& depth_5, double& depth_10, double& depth_20) {
        depth_5 = depth_10 = depth_20 = 0.0;

        if (best_price == INVALID_PRICE || level_count == 0) {
            return;
        }

        // Calculate thresholds once
        Price threshold_5, threshold_10, threshold_20;
        if (is_bid) {
            threshold_5 = best_price - static_cast<Price>((static_cast<int64_t>(best_price) * 5) / 10000);
            threshold_10 = best_price - static_cast<Price>((static_cast<int64_t>(best_price) * 10) / 10000);
            threshold_20 = best_price - static_cast<Price>((static_cast<int64_t>(best_price) * 20) / 10000);
        } else {
            threshold_5 = best_price + static_cast<Price>((static_cast<int64_t>(best_price) * 5) / 10000);
            threshold_10 = best_price + static_cast<Price>((static_cast<int64_t>(best_price) * 10) / 10000);
            threshold_20 = best_price + static_cast<Price>((static_cast<int64_t>(best_price) * 20) / 10000);
        }

        // Single pass: accumulate depths based on thresholds (branchless)
        for (int i = 0; i < level_count; ++i) {
            const auto& level = levels[i];
            double qty = static_cast<double>(level.quantity);

            // Branchless: use comparison results as 0/1 multipliers
            bool within_5, within_10, within_20;
            if (is_bid) {
                within_5 = level.price >= threshold_5;
                within_10 = level.price >= threshold_10;
                within_20 = level.price >= threshold_20;
            } else {
                within_5 = level.price <= threshold_5;
                within_10 = level.price <= threshold_10;
                within_20 = level.price <= threshold_20;
            }

            depth_5 += qty * within_5;
            depth_10 += qty * within_10;
            depth_20 += qty * within_20;

            // Early exit when beyond all thresholds (not branchless, but necessary)
            if (!within_20) {
                break;
            }
        }
    }


    // Calculate imbalance ratio: (bid - ask) / (bid + ask) (branchless)
    static double calculate_imbalance(double bid_depth, double ask_depth) {
        double total = bid_depth + ask_depth;
        // Branchless: use max(total, 1.0) to avoid division by zero
        // When total=0, (0-0)/1.0 = 0 which is correct
        return (bid_depth - ask_depth) / std::max(total, 1.0);
    }
};

} // namespace hft
