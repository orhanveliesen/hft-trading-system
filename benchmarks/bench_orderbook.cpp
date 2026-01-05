#include <iostream>
#include <iomanip>
#include <random>
#include "../include/types.hpp"
#include "../include/orderbook.hpp"
#include "../include/order_sender.hpp"
#include "../include/trading_engine.hpp"
#include "../include/benchmark/timer.hpp"
#include "../include/benchmark/histogram.hpp"

using namespace hft;
using namespace hft::benchmark;

// Use NullOrderSender for benchmarks
using BenchEngine = TradingEngine<NullOrderSender>;

// Benchmark configuration
constexpr size_t WARMUP_OPS = 1000;
constexpr size_t BENCH_OPS = 100000;

void print_stats(const char* name, const Histogram<>& hist, double freq_ghz) {
    std::cout << std::fixed << std::setprecision(1);
    std::cout << name << ":\n";
    std::cout << "  Count: " << hist.count() << " ops\n";
    std::cout << "  Mean:  " << RdtscTimer::cycles_to_ns(hist.mean(), freq_ghz) << " ns\n";
    std::cout << "  Min:   " << RdtscTimer::cycles_to_ns(hist.min(), freq_ghz) << " ns\n";
    std::cout << "  P50:   " << RdtscTimer::cycles_to_ns(hist.p50(), freq_ghz) << " ns\n";
    std::cout << "  P90:   " << RdtscTimer::cycles_to_ns(hist.p90(), freq_ghz) << " ns\n";
    std::cout << "  P99:   " << RdtscTimer::cycles_to_ns(hist.p99(), freq_ghz) << " ns\n";
    std::cout << "  P99.9: " << RdtscTimer::cycles_to_ns(hist.p999(), freq_ghz) << " ns\n";
    std::cout << "  Max:   " << RdtscTimer::cycles_to_ns(hist.max(), freq_ghz) << " ns\n";
    std::cout << "\n";
}

void bench_add_order(OrderBook& book, double freq_ghz) {
    Histogram<> hist;
    std::mt19937 rng(42);
    std::uniform_int_distribution<Price> price_dist(90000, 110000);

    // Warmup
    for (size_t i = 0; i < WARMUP_OPS; ++i) {
        Price price = price_dist(rng);
        Side side = (i % 2 == 0) ? Side::Buy : Side::Sell;
        book.add_order(i, side, price, 100);
    }

    // Clear and benchmark
    book = OrderBook();

    for (size_t i = 0; i < BENCH_OPS; ++i) {
        Price price = price_dist(rng);
        Side side = (i % 2 == 0) ? Side::Buy : Side::Sell;

        uint64_t start = RdtscTimer::now_serialized();
        book.add_order(i, side, price, 100);
        uint64_t end = RdtscTimer::now_serialized();

        hist.record(end - start);
    }

    print_stats("Add Order", hist, freq_ghz);
}

void bench_cancel_order(OrderBook& book, double freq_ghz) {
    Histogram<> hist;

    // Pre-fill book
    book = OrderBook();
    for (size_t i = 0; i < BENCH_OPS; ++i) {
        Price price = 100000 + (i % 1000);
        Side side = (i % 2 == 0) ? Side::Buy : Side::Sell;
        book.add_order(i, side, price, 100);
    }

    // Benchmark cancels
    for (size_t i = 0; i < BENCH_OPS; ++i) {
        uint64_t start = RdtscTimer::now_serialized();
        book.cancel_order(i);
        uint64_t end = RdtscTimer::now_serialized();

        hist.record(end - start);
    }

    print_stats("Cancel Order", hist, freq_ghz);
}

void bench_execute_order(OrderBook& book, double freq_ghz) {
    Histogram<> hist;

    // Pre-fill book
    book = OrderBook();
    for (size_t i = 0; i < BENCH_OPS; ++i) {
        Price price = 100000 + (i % 1000);
        Side side = (i % 2 == 0) ? Side::Buy : Side::Sell;
        book.add_order(i, side, price, 1000);  // Large quantity for partial executions
    }

    // Benchmark partial executions
    for (size_t i = 0; i < BENCH_OPS; ++i) {
        uint64_t start = RdtscTimer::now_serialized();
        book.execute_order(i, 10);  // Partial execution
        uint64_t end = RdtscTimer::now_serialized();

        hist.record(end - start);
    }

    print_stats("Execute Order (Partial)", hist, freq_ghz);
}

void bench_best_bid_ask(OrderBook& book, double freq_ghz) {
    Histogram<> hist;

    // Pre-fill book with realistic structure
    book = OrderBook();
    for (size_t i = 0; i < 10000; ++i) {
        Price price = 100000 + (i % 100);
        Side side = (i % 2 == 0) ? Side::Buy : Side::Sell;
        book.add_order(i, side, price, 100);
    }

    // Benchmark best bid/ask queries
    volatile Price bid, ask;  // Prevent optimization
    for (size_t i = 0; i < BENCH_OPS; ++i) {
        uint64_t start = RdtscTimer::now_serialized();
        bid = book.best_bid();
        ask = book.best_ask();
        uint64_t end = RdtscTimer::now_serialized();

        hist.record(end - start);
    }

    (void)bid; (void)ask;
    print_stats("Best Bid/Ask Query", hist, freq_ghz);
}

void bench_throughput(OrderBook& book, double freq_ghz) {
    constexpr size_t OPS = 1000000;  // 1M ops

    book = OrderBook();

    uint64_t start = RdtscTimer::now_serialized();

    for (size_t i = 0; i < OPS; ++i) {
        Price price = 100000 + (i % 1000);
        Side side = (i % 2 == 0) ? Side::Buy : Side::Sell;
        book.add_order(i % 100000, side, price, 100);

        if (i % 3 == 0) {
            book.cancel_order(i % 100000);
        }
    }

    uint64_t end = RdtscTimer::now_serialized();

    double seconds = static_cast<double>(end - start) / (freq_ghz * 1e9);
    double ops_per_sec = OPS / seconds;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Throughput (mixed workload):\n";
    std::cout << "  " << ops_per_sec / 1e6 << " million ops/sec\n";
    std::cout << "  " << (seconds * 1e9 / OPS) << " ns/op average\n\n";
}

// ============================================
// TradingEngine / SymbolWorld Benchmarks
// ============================================

void bench_symbol_world_lookup_by_id(BenchEngine& engine, double freq_ghz) {
    Histogram<> hist;

    // Get the symbol IDs we added
    std::vector<Symbol> symbols;
    engine.for_each_symbol([&](const SymbolWorld& world) {
        symbols.push_back(world.id());
    });

    // Warmup
    for (size_t i = 0; i < WARMUP_OPS; ++i) {
        volatile auto* world = engine.get_symbol_world(symbols[i % symbols.size()]);
        (void)world;
    }

    // Benchmark lookup by Symbol ID
    for (size_t i = 0; i < BENCH_OPS; ++i) {
        Symbol id = symbols[i % symbols.size()];

        uint64_t start = RdtscTimer::now_serialized();
        volatile auto* world = engine.get_symbol_world(id);
        uint64_t end = RdtscTimer::now_serialized();

        (void)world;
        hist.record(end - start);
    }

    print_stats("SymbolWorld Lookup (by ID)", hist, freq_ghz);
}

void bench_symbol_world_lookup_by_ticker(BenchEngine& engine, double freq_ghz) {
    Histogram<> hist;

    // Get tickers
    std::vector<std::string> tickers;
    engine.for_each_symbol([&](const SymbolWorld& world) {
        tickers.push_back(world.ticker());
    });

    // Warmup
    for (size_t i = 0; i < WARMUP_OPS; ++i) {
        volatile auto* world = engine.get_symbol_world(tickers[i % tickers.size()]);
        (void)world;
    }

    // Benchmark lookup by ticker string
    for (size_t i = 0; i < BENCH_OPS; ++i) {
        const auto& ticker = tickers[i % tickers.size()];

        uint64_t start = RdtscTimer::now_serialized();
        volatile auto* world = engine.get_symbol_world(ticker);
        uint64_t end = RdtscTimer::now_serialized();

        (void)world;
        hist.record(end - start);
    }

    print_stats("SymbolWorld Lookup (by ticker)", hist, freq_ghz);
}

void bench_symbol_world_full_path(BenchEngine& engine, double freq_ghz) {
    Histogram<> hist;

    // Get symbol IDs
    std::vector<Symbol> symbols;
    engine.for_each_symbol([&](const SymbolWorld& world) {
        symbols.push_back(world.id());
    });

    // Pre-fill order books
    for (auto id : symbols) {
        auto* world = engine.get_symbol_world(id);
        for (size_t i = 0; i < 1000; ++i) {
            Price price = world->config().base_price + (i % 100);
            Side side = (i % 2 == 0) ? Side::Buy : Side::Sell;
            world->book().add_order(id * 100000 + i, side, price, 100);
        }
    }

    // Benchmark full path: get_symbol_world() -> book() -> best_bid/ask
    for (size_t i = 0; i < BENCH_OPS; ++i) {
        Symbol id = symbols[i % symbols.size()];

        uint64_t start = RdtscTimer::now_serialized();
        auto* world = engine.get_symbol_world(id);
        volatile Price bid = world->book().best_bid();
        volatile Price ask = world->book().best_ask();
        uint64_t end = RdtscTimer::now_serialized();

        (void)bid; (void)ask;
        hist.record(end - start);
    }

    print_stats("Full Path: get_symbol_world()->book()->BBO", hist, freq_ghz);
}

void bench_direct_vs_engine_comparison(BenchEngine& engine, OrderBook& direct_book, double freq_ghz) {
    Histogram<> hist_direct, hist_engine;

    // Get first symbol
    Symbol sym_id = 0;
    engine.for_each_symbol([&](const SymbolWorld& world) {
        if (sym_id == 0) sym_id = world.id();
    });

    // Pre-fill both books identically
    direct_book = OrderBook();
    auto* world = engine.get_symbol_world(sym_id);

    for (size_t i = 0; i < 10000; ++i) {
        Price price = 100000 + (i % 100);
        Side side = (i % 2 == 0) ? Side::Buy : Side::Sell;
        direct_book.add_order(i, side, price, 100);
        world->book().add_order(i + 1000000, side, price, 100);
    }

    // Benchmark direct OrderBook access
    for (size_t i = 0; i < BENCH_OPS; ++i) {
        uint64_t start = RdtscTimer::now_serialized();
        volatile Price bid = direct_book.best_bid();
        volatile Price ask = direct_book.best_ask();
        uint64_t end = RdtscTimer::now_serialized();

        (void)bid; (void)ask;
        hist_direct.record(end - start);
    }

    // Benchmark via TradingEngine
    for (size_t i = 0; i < BENCH_OPS; ++i) {
        uint64_t start = RdtscTimer::now_serialized();
        auto* w = engine.get_symbol_world(sym_id);
        volatile Price bid = w->book().best_bid();
        volatile Price ask = w->book().best_ask();
        uint64_t end = RdtscTimer::now_serialized();

        (void)bid; (void)ask;
        hist_engine.record(end - start);
    }

    print_stats("Direct OrderBook BBO", hist_direct, freq_ghz);
    print_stats("Via TradingEngine BBO", hist_engine, freq_ghz);
}

int main() {
    std::cout << "=== Order Book Benchmark ===\n\n";

    std::cout << "Measuring CPU frequency... ";
    double freq_ghz = RdtscTimer::measure_frequency_ghz();
    std::cout << std::fixed << std::setprecision(3) << freq_ghz << " GHz\n\n";

    OrderBook book;

    bench_add_order(book, freq_ghz);
    bench_cancel_order(book, freq_ghz);
    bench_execute_order(book, freq_ghz);
    bench_best_bid_ask(book, freq_ghz);
    bench_throughput(book, freq_ghz);

    // TradingEngine / SymbolWorld benchmarks
    std::cout << "=== TradingEngine / SymbolWorld Benchmark ===\n\n";

    NullOrderSender null_sender;
    BenchEngine engine(null_sender);

    // Add multiple symbols to test realistic scenario
    std::vector<std::string> tickers = {"AAPL", "GOOGL", "MSFT", "AMZN", "TSLA", "META", "NVDA", "AMD"};
    for (const auto& ticker : tickers) {
        SymbolConfig config(ticker, 100000, 10000);
        engine.add_symbol(config);
    }

    std::cout << "Symbols loaded: " << engine.symbol_count() << "\n\n";

    bench_symbol_world_lookup_by_id(engine, freq_ghz);
    bench_symbol_world_lookup_by_ticker(engine, freq_ghz);
    bench_symbol_world_full_path(engine, freq_ghz);
    bench_direct_vs_engine_comparison(engine, book, freq_ghz);

    std::cout << "=== Benchmark Complete ===\n";
    return 0;
}
