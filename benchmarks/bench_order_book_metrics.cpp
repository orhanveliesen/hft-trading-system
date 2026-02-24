#include "../include/metrics/order_book_metrics.hpp"
#include "../include/orderbook.hpp"

#include <chrono>
#include <iostream>

using namespace hft;

// Create a realistic order book with 20 levels on each side
OrderBook create_realistic_book() {
    OrderBook book(90000, 200000);

    // Add 20 bid levels
    Price bid_price = 10000;
    for (int i = 0; i < 20; ++i) {
        book.add_order(100 + i, Side::Buy, bid_price - i, 100 + i * 5);
    }

    // Add 20 ask levels
    Price ask_price = 10010;
    for (int i = 0; i < 20; ++i) {
        book.add_order(200 + i, Side::Sell, ask_price + i, 100 + i * 5);
    }

    return book;
}

int main() {
    constexpr int NUM_ITERATIONS = 100'000;

    auto book = create_realistic_book();
    OrderBookMetrics metrics;

    // Warmup
    for (int i = 0; i < 1000; ++i) {
        metrics.on_order_book_update(book, i * 1000);
    }

    // Benchmark on_order_book_update()
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        uint64_t timestamp = i * 1000; // 1ms between updates
        metrics.on_order_book_update(book, timestamp);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration) / NUM_ITERATIONS;
    double avg_us = avg_ns / 1000.0;

    std::cout << "=== OrderBookMetrics Performance Benchmark ===\n";
    std::cout << "Iterations: " << NUM_ITERATIONS << "\n";
    std::cout << "Total time: " << duration / 1e9 << " seconds\n";
    std::cout << "Average time per on_order_book_update(): " << avg_ns << " ns (" << avg_us << " μs)\n";

    // Get metrics to verify calculation
    auto m = metrics.get_metrics();
    std::cout << "\nSample metrics:\n";
    std::cout << "  Spread: " << m.spread << " (" << m.spread_bps << " bps)\n";
    std::cout << "  Mid price: " << m.mid_price << "\n";
    std::cout << "  Bid depth (5 bps): " << m.bid_depth_5 << "\n";
    std::cout << "  Ask depth (5 bps): " << m.ask_depth_5 << "\n";
    std::cout << "  Imbalance (5 bps): " << m.imbalance_5 << "\n";

    if (avg_us < 5.0) {
        std::cout << "\n✓ Target met: < 5 μs per on_order_book_update()\n";
        return 0;
    } else {
        std::cout << "\n✗ Target missed: " << avg_us << " μs per on_order_book_update() (target: < 5 μs)\n";
        return 1;
    }
}
