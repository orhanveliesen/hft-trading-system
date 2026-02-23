#include "../include/paper/queue_fill_detector.hpp"

#include <cassert>
#include <iostream>
#include <vector>

using namespace hft;
using namespace hft::paper;

#define TEST(name) void name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "  " << #name << "... ";                                                                          \
        name();                                                                                                        \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

#define ASSERT_EQ(a, b) assert((a) == (b))
#define ASSERT_TRUE(x) assert(x)
#define ASSERT_FALSE(x) assert(!(x))
#define ASSERT_GT(a, b) assert((a) > (b))
#define ASSERT_GE(a, b) assert((a) >= (b))

// ============================================
// Fill Confidence Tests
// ============================================

TEST(test_confidence_weights) {
    ASSERT_EQ(confidence_weight(FillConfidence::Confirmed), 1.0);
    ASSERT_GT(confidence_weight(FillConfidence::VeryLikely), 0.8);
    ASSERT_GT(confidence_weight(FillConfidence::Likely), 0.5);
    ASSERT_GT(confidence_weight(FillConfidence::Possible), 0.2);
    ASSERT_GT(confidence_weight(FillConfidence::Unlikely), 0.0);
}

TEST(test_confidence_to_string) {
    ASSERT_EQ(std::string(confidence_to_string(FillConfidence::Confirmed)), "CONFIRMED");
    ASSERT_EQ(std::string(confidence_to_string(FillConfidence::VeryLikely)), "VERY_LIKELY");
    ASSERT_EQ(std::string(confidence_to_string(FillConfidence::Likely)), "LIKELY");
}

// ============================================
// Queue Fill Detector Tests
// ============================================

TEST(test_register_order) {
    QueueFillDetector detector;

    detector.register_order(1, 100, Side::Buy, 1000000, 500, 1000);

    ASSERT_EQ(detector.active_orders(), 1u);
}

TEST(test_cancel_order) {
    QueueFillDetector detector;

    detector.register_order(1, 100, Side::Buy, 1000000, 500, 1000);
    ASSERT_EQ(detector.active_orders(), 1u);

    detector.cancel_order(1);
    ASSERT_EQ(detector.active_orders(), 0u);
}

TEST(test_fill_estimate_initial) {
    QueueFillDetector detector;

    detector.register_order(1, 100, Side::Buy, 1000000, 500, 1000);

    auto estimate = detector.get_fill_estimate(1);
    ASSERT_FALSE(estimate.filled);
    ASSERT_EQ(estimate.confidence, FillConfidence::Unlikely);
}

TEST(test_pessimistic_fill_confirmation) {
    // Test the key pessimistic logic:
    // Only confirm fill when order AFTER us gets filled

    QueueFillDetector detector;

    std::vector<FillResult> fills;
    detector.set_fill_callback([&](OrderId id, const FillResult& result) { fills.push_back(result); });

    // Register our order at price 100, 500 shares
    detector.register_order(1, 100, Side::Buy, 1000000, 500, 1000);

    // Set initial L2 snapshot: 2000 shares ahead of us in queue
    detector.set_initial_queue_depth(100, Side::Buy, 1000000, 2000);

    // Trade happens - but not enough to reach us (only 500 out of 2000 ahead)
    detector.on_trade(100, 1000000, 500, Side::Sell, 2000, 0);

    // No fill yet - trade only consumed part of queue ahead
    ASSERT_EQ(fills.size(), 0u);

    // More trades - now an order AFTER us (sequence > ours) gets filled
    // This simulates seeing a trade where passive_sequence > our_sequence
    detector.on_trade(100, 1000000, 2000, Side::Sell, 3000, 999); // seq 999 > our seq

    // NOW we should be confirmed filled
    ASSERT_EQ(fills.size(), 1u);
    ASSERT_EQ(fills[0].confidence, FillConfidence::Confirmed);
    ASSERT_EQ(fills[0].fill_quantity, 500u);
}

TEST(test_fifo_fill_detection) {
    QueueFillDetector detector;

    std::vector<FillResult> fills;
    detector.set_fill_callback([&](OrderId id, const FillResult& result) { fills.push_back(result); });

    // Our order
    detector.register_order(1, 100, Side::Buy, 1000000, 300, 1000);

    // Simulate trades eating through queue
    // Our order is at position 0 (first), so any trade should fill us
    detector.on_trade(100, 1000000, 300, Side::Sell, 2000, 0);

    // We should be filled via FIFO
    ASSERT_GE(fills.size(), 1u);
}

TEST(test_queue_position_tracking) {
    QueueFillDetector detector;

    // Register our order
    detector.register_order(1, 100, Side::Buy, 1000000, 500, 1000);

    // Add orders behind us via L2 update
    detector.on_l2_update(100, Side::Buy, 1000000, 500, 1500, 2000); // +1000 shares

    auto estimate = detector.get_fill_estimate(1);
    // Our order is at front, so queue_ahead should be 0
    ASSERT_EQ(estimate.queue_ahead_at_fill, 0u);
}

TEST(test_l2_update_removal) {
    QueueFillDetector detector;

    std::vector<FillResult> fills;
    detector.set_fill_callback([&](OrderId id, const FillResult& result) { fills.push_back(result); });

    // Our order at front
    detector.register_order(1, 100, Side::Buy, 1000000, 500, 1000);

    // L2 shows reduction (fills/cancels at this level)
    detector.on_l2_update(100, Side::Buy, 1000000, 500, 0, 2000); // Level emptied

    // Our order should be filled
    ASSERT_EQ(fills.size(), 1u);
    ASSERT_EQ(fills[0].confidence, FillConfidence::Confirmed);
}

TEST(test_multiple_orders_same_level) {
    QueueFillDetector detector;

    std::vector<std::pair<OrderId, FillResult>> fills;
    detector.set_fill_callback([&](OrderId id, const FillResult& result) { fills.push_back({id, result}); });

    // Two of our orders at same price
    detector.register_order(1, 100, Side::Buy, 1000000, 300, 1000);
    detector.register_order(2, 100, Side::Buy, 1000000, 200, 2000);

    // Trade fills first order
    detector.on_trade(100, 1000000, 300, Side::Sell, 3000, 0);

    // First order should be filled
    ASSERT_GE(fills.size(), 1u);
}

TEST(test_probabilistic_tracking) {
    QueueFillDetector::Config config;
    config.pessimistic_mode = true;
    config.track_probabilistic = true;
    config.partial_fill_threshold = 0.9;

    QueueFillDetector detector(config);

    detector.register_order(1, 100, Side::Buy, 1000000, 500, 1000);

    // Some trading activity
    detector.on_trade(100, 1000000, 100, Side::Sell, 2000, 0);

    auto estimate = detector.get_fill_estimate(1);
    // Should have some confidence level based on activity
    ASSERT_TRUE(estimate.confidence == FillConfidence::Possible || estimate.confidence == FillConfidence::Unlikely);
}

TEST(test_queue_wait_time) {
    QueueFillDetector detector;

    FillResult last_fill;
    detector.set_fill_callback([&](OrderId id, const FillResult& result) { last_fill = result; });

    uint64_t submit_time = 1'000'000'000; // 1 second in ns
    uint64_t fill_time = 1'500'000'000;   // 1.5 seconds in ns

    detector.register_order(1, 100, Side::Buy, 1000000, 500, submit_time);

    // Trade at later time
    detector.on_l2_update(100, Side::Buy, 1000000, 500, 0, fill_time);

    ASSERT_TRUE(last_fill.filled);
    ASSERT_EQ(last_fill.queue_wait_ns, 500'000'000u); // 0.5 seconds
}

// ============================================
// Paper Trading Stats Tests
// ============================================

TEST(test_stats_recording) {
    PaperTradingStats stats;

    FillResult confirmed_fill{.filled = true,
                              .confidence = FillConfidence::Confirmed,
                              .fill_quantity = 100,
                              .fill_price = 1000000,
                              .queue_wait_ns = 1'000'000};

    stats.record_fill(confirmed_fill, 50.0);

    ASSERT_EQ(stats.confirmed_fills, 1u);
    ASSERT_EQ(stats.confirmed_pnl, 50.0);
}

TEST(test_stats_pnl_levels) {
    PaperTradingStats stats;

    FillResult confirmed{.confidence = FillConfidence::Confirmed};
    FillResult likely{.confidence = FillConfidence::Likely};
    FillResult possible{.confidence = FillConfidence::Possible};

    stats.record_fill(confirmed, 100.0);
    stats.record_fill(likely, 50.0);
    stats.record_fill(possible, 30.0);

    ASSERT_EQ(stats.pessimistic_pnl(), 100.0);
    ASSERT_GT(stats.expected_pnl(), stats.pessimistic_pnl());
    ASSERT_GT(stats.optimistic_pnl(), stats.expected_pnl());
}

TEST(test_stats_queue_wait_avg) {
    PaperTradingStats stats;

    FillResult fill1{
        .filled = true,
        .confidence = FillConfidence::Confirmed,
        .queue_wait_ns = 1'000'000 // 1ms
    };

    FillResult fill2{
        .filled = true,
        .confidence = FillConfidence::Confirmed,
        .queue_wait_ns = 3'000'000 // 3ms
    };

    stats.record_fill(fill1, 0);
    stats.record_fill(fill2, 0);

    ASSERT_EQ(stats.confirmed_fills, 2u);
    ASSERT_EQ(stats.avg_queue_wait_ms(), 2.0); // (1+3)/2 = 2ms
}

// ============================================
// Main
// ============================================

int main() {
    std::cout << "\n=== Queue Fill Detector Tests ===\n\n";

    std::cout << "Fill Confidence:\n";
    RUN_TEST(test_confidence_weights);
    RUN_TEST(test_confidence_to_string);

    std::cout << "\nQueue Fill Detector:\n";
    RUN_TEST(test_register_order);
    RUN_TEST(test_cancel_order);
    RUN_TEST(test_fill_estimate_initial);
    RUN_TEST(test_pessimistic_fill_confirmation);
    RUN_TEST(test_fifo_fill_detection);
    RUN_TEST(test_queue_position_tracking);
    RUN_TEST(test_l2_update_removal);
    RUN_TEST(test_multiple_orders_same_level);
    RUN_TEST(test_probabilistic_tracking);
    RUN_TEST(test_queue_wait_time);

    std::cout << "\nPaper Trading Stats:\n";
    RUN_TEST(test_stats_recording);
    RUN_TEST(test_stats_pnl_levels);
    RUN_TEST(test_stats_queue_wait_avg);

    std::cout << "\n=== All Queue Fill Detector Tests Passed! ===\n";
    return 0;
}
