#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>
#include "../include/logging/async_logger.hpp"
#include "../include/paper/paper_trading_engine.hpp"

using namespace hft;
using namespace hft::logging;
using namespace hft::paper;
using namespace hft::strategy;
using namespace hft::risk;

#define TEST(name) void name()
#define RUN_TEST(name) do { \
    std::cout << "  " << #name << "... "; \
    name(); \
    std::cout << "PASSED\n"; \
} while(0)

#define ASSERT_EQ(a, b) assert((a) == (b))
#define ASSERT_TRUE(x) assert(x)
#define ASSERT_FALSE(x) assert(!(x))
#define ASSERT_GT(a, b) assert((a) > (b))
#define ASSERT_GE(a, b) assert((a) >= (b))
#define ASSERT_LT(a, b) assert((a) < (b))
#define ASSERT_LE(a, b) assert((a) <= (b))

// ============================================
// Async Logger Tests
// ============================================

TEST(test_log_entry_size) {
    ASSERT_EQ(sizeof(LogEntry), 64);  // One cache line
}

TEST(test_ring_buffer_push_pop) {
    LogRingBuffer<64> buffer;

    ASSERT_TRUE(buffer.empty());
    ASSERT_EQ(buffer.size(), 0u);

    LogEntry entry;
    entry.level = LogLevel::Info;
    entry.set_message("Test message");

    ASSERT_TRUE(buffer.try_push(entry));
    ASSERT_FALSE(buffer.empty());
    ASSERT_EQ(buffer.size(), 1u);

    LogEntry popped;
    ASSERT_TRUE(buffer.try_pop(popped));
    ASSERT_EQ(popped.level, LogLevel::Info);
    ASSERT_TRUE(buffer.empty());
}

TEST(test_ring_buffer_full) {
    LogRingBuffer<4> buffer;  // Very small buffer

    LogEntry entry;
    entry.level = LogLevel::Info;

    // Fill buffer (capacity - 1 = 3 entries)
    ASSERT_TRUE(buffer.try_push(entry));
    ASSERT_TRUE(buffer.try_push(entry));
    ASSERT_TRUE(buffer.try_push(entry));

    // Buffer should be full now
    ASSERT_TRUE(buffer.full());
    ASSERT_FALSE(buffer.try_push(entry));  // Should fail

    // Pop one
    LogEntry popped;
    ASSERT_TRUE(buffer.try_pop(popped));
    ASSERT_FALSE(buffer.full());

    // Now we can push again
    ASSERT_TRUE(buffer.try_push(entry));
}

TEST(test_async_logger_basic) {
    AsyncLogger logger;
    logger.set_min_level(LogLevel::Debug);

    // Capture output
    std::vector<LogEntry> captured;
    logger.set_output_callback([&](const LogEntry& entry) {
        captured.push_back(entry);
    });

    logger.start();

    // Log some messages
    LOG_INFO(logger, "Test message 1");
    LOG_WARN(logger, "Test message 2");
    LOG_DEBUG(logger, "Test message 3");

    // Give consumer thread time to process
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    logger.stop();

    ASSERT_EQ(captured.size(), 3u);
    ASSERT_EQ(captured[0].level, LogLevel::Info);
    ASSERT_EQ(captured[1].level, LogLevel::Warn);
    ASSERT_EQ(captured[2].level, LogLevel::Debug);
}

TEST(test_async_logger_filtering) {
    AsyncLogger logger;
    logger.set_min_level(LogLevel::Warn);  // Only Warn and above

    std::vector<LogEntry> captured;
    logger.set_output_callback([&](const LogEntry& entry) {
        captured.push_back(entry);
    });

    logger.start();

    LOG_DEBUG(logger, "Debug - should be filtered");
    LOG_INFO(logger, "Info - should be filtered");
    LOG_WARN(logger, "Warn - should pass");
    LOG_ERROR(logger, "Error - should pass");

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    logger.stop();

    ASSERT_EQ(captured.size(), 2u);
    ASSERT_EQ(captured[0].level, LogLevel::Warn);
    ASSERT_EQ(captured[1].level, LogLevel::Error);
}

TEST(test_async_logger_categories) {
    AsyncLogger logger;

    std::vector<LogEntry> captured;
    logger.set_output_callback([&](const LogEntry& entry) {
        captured.push_back(entry);
    });

    logger.start();

    LOG_CATEGORY(logger, LogLevel::Info, Order, "Order message");
    LOG_CATEGORY(logger, LogLevel::Info, Strategy, "Strategy message");
    LOG_CATEGORY(logger, LogLevel::Info, Risk, "Risk message");

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    logger.stop();

    ASSERT_EQ(captured.size(), 3u);
    ASSERT_EQ(captured[0].category, LogCategory::Order);
    ASSERT_EQ(captured[1].category, LogCategory::Strategy);
    ASSERT_EQ(captured[2].category, LogCategory::Risk);
}

// ============================================
// Paper Order Sender Tests
// ============================================

TEST(test_paper_sender_order_concept) {
    // Verify it satisfies OrderSender concept
    static_assert(is_order_sender_v<PaperOrderSender>, "PaperOrderSender must satisfy OrderSender");
}

TEST(test_paper_sender_basic_order) {
    FillSimConfig config;
    config.min_latency_ns = 0;  // No latency for testing
    config.max_latency_ns = 0;
    config.enable_partial_fills = false;

    PaperOrderSender sender(config);

    std::vector<FillEvent> fills;
    sender.set_fill_callback([&](const FillEvent& event) {
        fills.push_back(event);
    });

    ASSERT_TRUE(sender.send_order(1, Side::Buy, 100, true));
    ASSERT_EQ(sender.total_orders(), 1u);
    ASSERT_EQ(sender.pending_count(), 1u);

    // Process fills with market price
    sender.process_fills(1, 1000000, 1001000);  // Bid: $100.00, Ask: $100.10

    ASSERT_EQ(fills.size(), 1u);
    ASSERT_EQ(fills[0].symbol, 1u);
    ASSERT_EQ(fills[0].side, Side::Buy);
    ASSERT_EQ(fills[0].quantity, 100u);
    ASSERT_GT(fills[0].price, 0u);
}

TEST(test_paper_sender_cancel_order) {
    FillSimConfig config;
    config.min_latency_ns = 1'000'000'000;  // 1 second latency (won't fill)
    config.max_latency_ns = 1'000'000'000;

    PaperOrderSender sender(config);

    sender.send_order(1, Side::Buy, 100, true);
    ASSERT_EQ(sender.pending_count(), 1u);

    ASSERT_TRUE(sender.cancel_order(1, 1));  // Cancel order ID 1
    ASSERT_EQ(sender.pending_count(), 0u);
}

TEST(test_paper_sender_slippage) {
    FillSimConfig config;
    config.min_latency_ns = 0;
    config.max_latency_ns = 0;
    config.slippage_bps = 10.0;  // 10 bps = 0.1%
    config.enable_partial_fills = false;

    PaperOrderSender sender(config);

    std::vector<FillEvent> fills;
    sender.set_fill_callback([&](const FillEvent& event) {
        fills.push_back(event);
    });

    // Buy order
    sender.send_order(1, Side::Buy, 100, true);
    sender.process_fills(1, 1000000, 1001000);  // Ask: $100.10

    // Fill price should be >= ask (slippage goes against us)
    ASSERT_GE(fills[0].price, 1001000u);
}

// ============================================
// Paper Trading Engine Tests
// ============================================

TEST(test_paper_engine_initialization) {
    PaperTradingEngine::Config config;
    config.initial_capital = 50000.0;
    config.enable_logging = false;

    PaperTradingEngine engine(config);

    ASSERT_EQ(engine.equity(), 50000.0);
    ASSERT_EQ(engine.total_pnl(), 0.0);
    ASSERT_EQ(engine.drawdown(), 0.0);
    ASSERT_FALSE(engine.is_halted());
}

TEST(test_paper_engine_market_data) {
    PaperTradingEngine::Config config;
    config.enable_logging = false;

    PaperTradingEngine engine(config);

    // Feed some market data
    for (int i = 0; i < 30; ++i) {
        Price bid = 1000000 + i * 100;  // $100.00 + $0.01 per tick
        Price ask = bid + 1000;          // $0.10 spread
        engine.on_market_data(1, bid, ask, i * 1000000);
    }

    // Regime should be detected after enough data
    ASSERT_TRUE(engine.current_regime() != strategy::MarketRegime::Unknown);
}

TEST(test_paper_engine_order_submission) {
    PaperTradingEngine::Config config;
    config.enable_logging = false;
    config.fill_config.min_latency_ns = 0;
    config.fill_config.max_latency_ns = 0;
    config.fill_config.enable_partial_fills = false;

    PaperTradingEngine engine(config);

    // Submit order
    ASSERT_TRUE(engine.submit_order(1, Side::Buy, 100, true));
    ASSERT_EQ(engine.total_orders(), 1u);

    // Process fill
    engine.on_market_data(1, 1000000, 1001000, 0);

    ASSERT_EQ(engine.total_fills(), 1u);

    // Check position
    const auto& pos = engine.get_position(1);
    ASSERT_EQ(pos.quantity, 100);
}

TEST(test_paper_engine_pnl_calculation) {
    PaperTradingEngine::Config config;
    config.initial_capital = 100000.0;
    config.enable_logging = false;
    config.fill_config.min_latency_ns = 0;
    config.fill_config.max_latency_ns = 0;
    config.fill_config.slippage_bps = 0;  // No slippage for exact calculation
    config.fill_config.enable_partial_fills = false;

    PaperTradingEngine engine(config);

    // Buy 100 shares
    engine.submit_order(1, Side::Buy, 100, true);
    engine.on_market_data(1, 1000000, 1001000, 0);  // Entry @ ~$100.10

    // Price goes up
    engine.on_market_data(1, 1010000, 1011000, 1000000);  // Bid: $101.00

    // Should have unrealized profit
    const auto& pos = engine.get_position(1);
    ASSERT_GT(pos.unrealized_pnl, 0.0);
}

TEST(test_paper_engine_risk_halt) {
    PaperTradingEngine::Config config;
    config.initial_capital = 10000 * risk::PRICE_SCALE;  // $10,000 scaled
    config.max_drawdown_pct = 0.01;  // 1% max drawdown = $100
    config.daily_loss_limit = 100 * risk::PRICE_SCALE;   // $100 daily loss limit
    config.enable_logging = false;
    config.fill_config.min_latency_ns = 0;
    config.fill_config.max_latency_ns = 0;
    config.fill_config.slippage_bps = 0;
    config.fill_config.enable_partial_fills = false;

    PaperTradingEngine engine(config);

    // Buy 100 shares at $100
    engine.submit_order(1, Side::Buy, 100, true);
    engine.on_market_data(1, 1000000, 1001000, 0);

    ASSERT_FALSE(engine.is_halted());

    // Price drops significantly (more than 1% loss on position)
    engine.on_market_data(1, 980000, 981000, 1000000);  // Bid: $98.00 (2% loss)

    // Should be halted due to drawdown
    ASSERT_TRUE(engine.is_halted());

    // Should not be able to submit new orders
    ASSERT_FALSE(engine.submit_order(1, Side::Buy, 100, true));
}

TEST(test_paper_engine_position_limit) {
    PaperTradingEngine::Config config;
    config.default_max_position = 100;  // Max 100 shares per symbol
    config.enable_logging = false;
    config.fill_config.min_latency_ns = 0;
    config.fill_config.max_latency_ns = 0;

    PaperTradingEngine engine(config);

    // Register symbol with position limit
    engine.register_symbol("TEST", 100, 0);  // max 100 shares

    // Submit order for 100 shares - should work
    ASSERT_TRUE(engine.submit_order(0, Side::Buy, 100, true));

    // Fill the order
    engine.on_market_data(0, 1000000, 1001000, 0);

    // Try to buy more - should be rejected by risk manager
    ASSERT_FALSE(engine.submit_order(0, Side::Buy, 50, true));

    // Selling should work
    ASSERT_TRUE(engine.submit_order(0, Side::Sell, 50, true));
}

// ============================================
// Main
// ============================================

int main() {
    std::cout << "\n=== Paper Trading Tests ===\n\n";

    std::cout << "Async Logger:\n";
    RUN_TEST(test_log_entry_size);
    RUN_TEST(test_ring_buffer_push_pop);
    RUN_TEST(test_ring_buffer_full);
    RUN_TEST(test_async_logger_basic);
    RUN_TEST(test_async_logger_filtering);
    RUN_TEST(test_async_logger_categories);

    std::cout << "\nPaper Order Sender:\n";
    RUN_TEST(test_paper_sender_order_concept);
    RUN_TEST(test_paper_sender_basic_order);
    RUN_TEST(test_paper_sender_cancel_order);
    RUN_TEST(test_paper_sender_slippage);

    std::cout << "\nPaper Trading Engine:\n";
    RUN_TEST(test_paper_engine_initialization);
    RUN_TEST(test_paper_engine_market_data);
    RUN_TEST(test_paper_engine_order_submission);
    RUN_TEST(test_paper_engine_pnl_calculation);
    RUN_TEST(test_paper_engine_risk_halt);
    RUN_TEST(test_paper_engine_position_limit);

    std::cout << "\n=== All Paper Trading Tests Passed! ===\n";
    return 0;
}
