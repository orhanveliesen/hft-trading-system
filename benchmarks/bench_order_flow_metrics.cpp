#include "../include/ipc/trade_event.hpp"
#include "../include/metrics/order_flow_metrics.hpp"
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
    OrderFlowMetrics<20> metrics;

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
    auto duration_update = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    // Benchmark on_trade()
    start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        ipc::TradeEvent trade;
        trade.clear();
        trade.price = 10005.0;
        trade.quantity = 100.0;
        trade.timestamp_ns = i * 1000000; // 1ms between trades
        trade.side = (i % 2);             // 0=Buy, 1=Sell
        metrics.on_trade(trade);
    }

    end = std::chrono::high_resolution_clock::now();
    auto duration_trade = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    double avg_ns_update = static_cast<double>(duration_update) / NUM_ITERATIONS;
    double avg_us_update = avg_ns_update / 1000.0;
    double avg_ns_trade = static_cast<double>(duration_trade) / NUM_ITERATIONS;

    std::cout << "=== OrderFlowMetrics Performance Benchmark ===\n";
    std::cout << "Iterations: " << NUM_ITERATIONS << "\n\n";

    std::cout << "on_order_book_update():\n";
    std::cout << "  Total time: " << duration_update / 1e9 << " seconds\n";
    std::cout << "  Average: " << avg_ns_update << " ns (" << avg_us_update << " μs)\n";
    std::cout << "  Target: < 5 μs\n";

    std::cout << "\non_trade():\n";
    std::cout << "  Total time: " << duration_trade / 1e9 << " seconds\n";
    std::cout << "  Average: " << avg_ns_trade << " ns\n";
    std::cout << "  Target: < 100 ns\n";

    // Get metrics to verify calculation
    auto m = metrics.get_metrics(Window::SEC_1);
    std::cout << "\nSample metrics (1 second window):\n";
    std::cout << "  Bid volume added: " << m.bid_volume_added << "\n";
    std::cout << "  Ask volume added: " << m.ask_volume_added << "\n";
    std::cout << "  Bid cancel ratio: " << m.cancel_ratio_bid << "\n";
    std::cout << "  Ask cancel ratio: " << m.cancel_ratio_ask << "\n";
    std::cout << "  Book update count: " << m.book_update_count << "\n";

    bool update_ok = (avg_us_update < 5.0);
    bool trade_ok = (avg_ns_trade < 100.0);

    if (update_ok && trade_ok) {
        std::cout << "\n✓ All targets met\n";
        return 0;
    } else {
        std::cout << "\n✗ Performance targets missed:\n";
        if (!update_ok) {
            std::cout << "  - on_order_book_update: " << avg_us_update << " μs (target: < 5 μs)\n";
        }
        if (!trade_ok) {
            std::cout << "  - on_trade: " << avg_ns_trade << " ns (target: < 100 ns)\n";
        }
        return 1;
    }
}
