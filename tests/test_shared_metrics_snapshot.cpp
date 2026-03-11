#include "../include/ipc/shared_metrics_snapshot.hpp"

#include <cassert>
#include <cstring>
#include <iostream>

using namespace hft;
using namespace hft::ipc;
using namespace hft::strategy;

#define TEST(name) void name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "Running " << #name << "... ";                                                                    \
        name();                                                                                                        \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

const char* test_shm_name = "/test_metrics_snapshot";

void cleanup_shm() {
    SharedMetricsSnapshot::destroy(nullptr, test_shm_name);
}

// ============================================================================
// Creation and Opening
// ============================================================================

TEST(test_create_and_open) {
    cleanup_shm();

    auto* writer = SharedMetricsSnapshot::create(test_shm_name);
    assert(writer != nullptr);

    assert(writer->magic == METRICS_SNAPSHOT_MAGIC);
    assert(writer->version == METRICS_SNAPSHOT_VERSION);
    assert(writer->symbol_count == 0);

    auto* reader = SharedMetricsSnapshot::open_readonly(test_shm_name);
    assert(reader != nullptr);

    assert(reader->magic == METRICS_SNAPSHOT_MAGIC);
    assert(reader->version == METRICS_SNAPSHOT_VERSION);

    SharedMetricsSnapshot::destroy(writer, test_shm_name);
    SharedMetricsSnapshot::destroy(reader, test_shm_name);
}

TEST(test_open_nonexistent) {
    auto* reader = SharedMetricsSnapshot::open_readonly("/nonexistent_metrics");
    assert(reader == nullptr);
}

TEST(test_magic_validation) {
    cleanup_shm();

    auto* writer = SharedMetricsSnapshot::create(test_shm_name);
    assert(writer != nullptr);

    // Corrupt magic
    writer->magic = 0xDEADBEEF;

    auto* reader = SharedMetricsSnapshot::open_readonly(test_shm_name);
    assert(reader == nullptr); // Should fail validation

    SharedMetricsSnapshot::destroy(writer, test_shm_name);
}

// ============================================================================
// Symbol Snapshots
// ============================================================================

TEST(test_symbol_ticker_initialization) {
    cleanup_shm();

    auto* snap = SharedMetricsSnapshot::create(test_shm_name);
    assert(snap != nullptr);

    // Set ticker for symbol 0
    std::strncpy(snap->symbols[0].ticker, "BTCUSDT", sizeof(snap->symbols[0].ticker) - 1);
    snap->symbols[0].ticker[sizeof(snap->symbols[0].ticker) - 1] = '\0';

    assert(std::strcmp(snap->symbols[0].ticker, "BTCUSDT") == 0);
    assert(snap->symbols[1].ticker[0] == '\0'); // Symbol 1 should be empty

    SharedMetricsSnapshot::destroy(snap, test_shm_name);
}

TEST(test_update_count_increment) {
    cleanup_shm();

    auto* snap = SharedMetricsSnapshot::create(test_shm_name);
    assert(snap != nullptr);

    auto& sym = snap->symbols[0];
    assert(sym.update_count.load() == 0);

    // Simulate updates
    sym.update_count.fetch_add(1, std::memory_order_relaxed);
    assert(sym.update_count.load() == 1);

    sym.update_count.fetch_add(1, std::memory_order_relaxed);
    assert(sym.update_count.load() == 2);

    SharedMetricsSnapshot::destroy(snap, test_shm_name);
}

// ============================================================================
// Atomic Field Access
// ============================================================================

TEST(test_trade_metrics_atomic_writes) {
    cleanup_shm();

    auto* snap = SharedMetricsSnapshot::create(test_shm_name);
    assert(snap != nullptr);

    auto& sym = snap->symbols[0];

    // Write some trade metrics (w1s window)
    sym.trade_w1s_buy_volume_x8.store(100000000, std::memory_order_relaxed); // 1.0
    sym.trade_w1s_sell_volume_x8.store(50000000, std::memory_order_relaxed); // 0.5
    sym.trade_w1s_total_trades.store(42, std::memory_order_relaxed);

    // Read back
    assert(sym.trade_w1s_buy_volume_x8.load() == 100000000);
    assert(sym.trade_w1s_sell_volume_x8.load() == 50000000);
    assert(sym.trade_w1s_total_trades.load() == 42);

    SharedMetricsSnapshot::destroy(snap, test_shm_name);
}

TEST(test_order_book_metrics_atomic_writes) {
    cleanup_shm();

    auto* snap = SharedMetricsSnapshot::create(test_shm_name);
    assert(snap != nullptr);

    auto& sym = snap->symbols[0];

    // Write order book metrics
    sym.book_current_spread_bps_x8.store(500000, std::memory_order_relaxed); // 0.005 bps
    sym.book_current_best_bid.store(9100000, std::memory_order_relaxed);
    sym.book_current_best_ask.store(9100100, std::memory_order_relaxed);

    // Read back
    assert(sym.book_current_spread_bps_x8.load() == 500000);
    assert(sym.book_current_best_bid.load() == 9100000);
    assert(sym.book_current_best_ask.load() == 9100100);

    SharedMetricsSnapshot::destroy(snap, test_shm_name);
}

TEST(test_regime_fields) {
    cleanup_shm();

    auto* snap = SharedMetricsSnapshot::create(test_shm_name);
    assert(snap != nullptr);

    auto& sym = snap->symbols[0];

    // Write regime
    sym.regime.store(static_cast<uint8_t>(MarketRegime::TrendingUp), std::memory_order_relaxed);
    sym.regime_current_confidence_x8.store(85000000, std::memory_order_relaxed); // 0.85
    sym.regime_change_count.store(5, std::memory_order_relaxed);

    // Read back
    assert(sym.regime.load() == static_cast<uint8_t>(MarketRegime::TrendingUp));
    assert(sym.regime_current_confidence_x8.load() == 85000000);
    assert(sym.regime_change_count.load() == 5);

    SharedMetricsSnapshot::destroy(snap, test_shm_name);
}

// ============================================================================
// Multiple Symbols
// ============================================================================

TEST(test_multiple_symbols) {
    cleanup_shm();

    auto* snap = SharedMetricsSnapshot::create(test_shm_name);
    assert(snap != nullptr);

    // Initialize multiple symbols
    std::strncpy(snap->symbols[0].ticker, "BTCUSDT", 15);
    std::strncpy(snap->symbols[1].ticker, "ETHUSDT", 15);
    std::strncpy(snap->symbols[2].ticker, "SOLUSDT", 15);

    snap->symbols[0].update_count.store(10, std::memory_order_relaxed);
    snap->symbols[1].update_count.store(20, std::memory_order_relaxed);
    snap->symbols[2].update_count.store(30, std::memory_order_relaxed);

    assert(std::strcmp(snap->symbols[0].ticker, "BTCUSDT") == 0);
    assert(std::strcmp(snap->symbols[1].ticker, "ETHUSDT") == 0);
    assert(std::strcmp(snap->symbols[2].ticker, "SOLUSDT") == 0);

    assert(snap->symbols[0].update_count.load() == 10);
    assert(snap->symbols[1].update_count.load() == 20);
    assert(snap->symbols[2].update_count.load() == 30);

    SharedMetricsSnapshot::destroy(snap, test_shm_name);
}

// ============================================================================
// Reader/Writer Scenario
// ============================================================================

TEST(test_writer_reader_scenario) {
    cleanup_shm();

    // Writer creates and writes
    auto* writer = SharedMetricsSnapshot::create(test_shm_name);
    assert(writer != nullptr);

    std::strncpy(writer->symbols[0].ticker, "BTCUSDT", 15);
    writer->symbols[0].trade_w1s_buy_volume_x8.store(123456789, std::memory_order_relaxed);
    writer->symbols[0].book_current_spread_bps_x8.store(987654, std::memory_order_relaxed);
    writer->symbols[0].update_count.fetch_add(1, std::memory_order_relaxed);

    // Reader opens and reads
    auto* reader = SharedMetricsSnapshot::open_readonly(test_shm_name);
    assert(reader != nullptr);

    assert(std::strcmp(reader->symbols[0].ticker, "BTCUSDT") == 0);
    assert(reader->symbols[0].trade_w1s_buy_volume_x8.load() == 123456789);
    assert(reader->symbols[0].book_current_spread_bps_x8.load() == 987654);
    assert(reader->symbols[0].update_count.load() == 1);

    // Writer updates
    writer->symbols[0].update_count.fetch_add(1, std::memory_order_relaxed);

    // Reader sees update
    assert(reader->symbols[0].update_count.load() == 2);

    SharedMetricsSnapshot::destroy(writer, test_shm_name);
    SharedMetricsSnapshot::destroy(reader, test_shm_name);
}

// ============================================================================
// Additional Metric Fields
// ============================================================================

TEST(test_futures_metrics_fields) {
    cleanup_shm();

    auto* snap = SharedMetricsSnapshot::create(test_shm_name);
    assert(snap != nullptr);

    auto& sym = snap->symbols[0];

    // Write futures metrics (w1s window)
    sym.futures_w1s_funding_rate_x8.store(500000, std::memory_order_relaxed); // 0.005
    sym.futures_w1s_basis_bps_x8.store(1000000, std::memory_order_relaxed);   // 0.01
    sym.futures_w1s_liquidation_count.store(5, std::memory_order_relaxed);
    sym.futures_w1s_funding_rate_extreme.store(1, std::memory_order_relaxed);
    sym.futures_next_funding_time_ms.store(1234567890, std::memory_order_relaxed);

    // Read back
    assert(sym.futures_w1s_funding_rate_x8.load() == 500000);
    assert(sym.futures_w1s_basis_bps_x8.load() == 1000000);
    assert(sym.futures_w1s_liquidation_count.load() == 5);
    assert(sym.futures_w1s_funding_rate_extreme.load() == 1);
    assert(sym.futures_next_funding_time_ms.load() == 1234567890);

    SharedMetricsSnapshot::destroy(snap, test_shm_name);
}

TEST(test_combined_metrics_fields) {
    cleanup_shm();

    auto* snap = SharedMetricsSnapshot::create(test_shm_name);
    assert(snap != nullptr);

    auto& sym = snap->symbols[0];

    // Write combined metrics (sec1 window)
    sym.combined_sec1_trade_to_depth_ratio_x8.store(150000000, std::memory_order_relaxed); // 1.5
    sym.combined_sec1_absorption_ratio_bid_x8.store(200000000, std::memory_order_relaxed); // 2.0
    sym.combined_sec1_spread_mean_x8.store(50000000, std::memory_order_relaxed);           // 0.5

    // Read back
    assert(sym.combined_sec1_trade_to_depth_ratio_x8.load() == 150000000);
    assert(sym.combined_sec1_absorption_ratio_bid_x8.load() == 200000000);
    assert(sym.combined_sec1_spread_mean_x8.load() == 50000000);

    SharedMetricsSnapshot::destroy(snap, test_shm_name);
}

TEST(test_order_flow_metrics_fields) {
    cleanup_shm();

    auto* snap = SharedMetricsSnapshot::create(test_shm_name);
    assert(snap != nullptr);

    auto& sym = snap->symbols[0];

    // Write order flow metrics (sec1 window)
    sym.flow_sec1_bid_volume_added_x8.store(100000000, std::memory_order_relaxed);  // 1.0
    sym.flow_sec1_ask_volume_removed_x8.store(50000000, std::memory_order_relaxed); // 0.5
    sym.flow_sec1_cancel_ratio_bid_x8.store(25000000, std::memory_order_relaxed);   // 0.25
    sym.flow_sec1_book_update_count.store(42, std::memory_order_relaxed);

    // Read back
    assert(sym.flow_sec1_bid_volume_added_x8.load() == 100000000);
    assert(sym.flow_sec1_ask_volume_removed_x8.load() == 50000000);
    assert(sym.flow_sec1_cancel_ratio_bid_x8.load() == 25000000);
    assert(sym.flow_sec1_book_update_count.load() == 42);

    SharedMetricsSnapshot::destroy(snap, test_shm_name);
}

TEST(test_last_update_timestamp) {
    cleanup_shm();

    auto* snap = SharedMetricsSnapshot::create(test_shm_name);
    assert(snap != nullptr);

    auto& sym = snap->symbols[0];

    // Initially 0
    assert(sym.last_update_ns.load() == 0);

    // Set timestamp
    uint64_t now_ns = 1234567890123456ULL;
    sym.last_update_ns.store(now_ns, std::memory_order_relaxed);
    assert(sym.last_update_ns.load() == now_ns);

    SharedMetricsSnapshot::destroy(snap, test_shm_name);
}

TEST(test_version_validation_failure) {
    cleanup_shm();

    auto* writer = SharedMetricsSnapshot::create(test_shm_name);
    assert(writer != nullptr);

    // Corrupt version
    writer->version = 999;

    auto* reader = SharedMetricsSnapshot::open_readonly(test_shm_name);
    assert(reader == nullptr); // Should fail validation

    SharedMetricsSnapshot::destroy(writer, test_shm_name);
}

TEST(test_multiple_windows_trade_metrics) {
    cleanup_shm();

    auto* snap = SharedMetricsSnapshot::create(test_shm_name);
    assert(snap != nullptr);

    auto& sym = snap->symbols[0];

    // Write metrics for different windows
    sym.trade_w1s_buy_volume_x8.store(100000000, std::memory_order_relaxed);
    sym.trade_w5s_buy_volume_x8.store(500000000, std::memory_order_relaxed);
    sym.trade_w10s_buy_volume_x8.store(1000000000, std::memory_order_relaxed);
    sym.trade_w30s_buy_volume_x8.store(3000000000, std::memory_order_relaxed);
    sym.trade_w1min_buy_volume_x8.store(6000000000, std::memory_order_relaxed);

    // Verify
    assert(sym.trade_w1s_buy_volume_x8.load() == 100000000);
    assert(sym.trade_w5s_buy_volume_x8.load() == 500000000);
    assert(sym.trade_w10s_buy_volume_x8.load() == 1000000000);
    assert(sym.trade_w30s_buy_volume_x8.load() == 3000000000);
    assert(sym.trade_w1min_buy_volume_x8.load() == 6000000000);

    SharedMetricsSnapshot::destroy(snap, test_shm_name);
}

TEST(test_regime_last_change) {
    cleanup_shm();

    auto* snap = SharedMetricsSnapshot::create(test_shm_name);
    assert(snap != nullptr);

    auto& sym = snap->symbols[0];

    // Set regime change tracking
    sym.regime.store(static_cast<uint8_t>(MarketRegime::HighVolatility), std::memory_order_relaxed);
    sym.regime_last_change_ns.store(9876543210ULL, std::memory_order_relaxed);
    sym.regime_change_count.store(15, std::memory_order_relaxed);

    // Verify
    assert(sym.regime.load() == static_cast<uint8_t>(MarketRegime::HighVolatility));
    assert(sym.regime_last_change_ns.load() == 9876543210ULL);
    assert(sym.regime_change_count.load() == 15);

    SharedMetricsSnapshot::destroy(snap, test_shm_name);
}

int main() {
    RUN_TEST(test_create_and_open);
    RUN_TEST(test_open_nonexistent);
    RUN_TEST(test_magic_validation);
    RUN_TEST(test_symbol_ticker_initialization);
    RUN_TEST(test_update_count_increment);
    RUN_TEST(test_trade_metrics_atomic_writes);
    RUN_TEST(test_order_book_metrics_atomic_writes);
    RUN_TEST(test_regime_fields);
    RUN_TEST(test_multiple_symbols);
    RUN_TEST(test_writer_reader_scenario);
    RUN_TEST(test_futures_metrics_fields);
    RUN_TEST(test_combined_metrics_fields);
    RUN_TEST(test_order_flow_metrics_fields);
    RUN_TEST(test_last_update_timestamp);
    RUN_TEST(test_version_validation_failure);
    RUN_TEST(test_multiple_windows_trade_metrics);
    RUN_TEST(test_regime_last_change);

    std::cout << "\nAll 17 SharedMetricsSnapshot tests passed!\n";
    return 0;
}
