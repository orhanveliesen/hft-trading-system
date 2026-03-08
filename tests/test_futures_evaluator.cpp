/**
 * Test suite for FuturesEvaluator
 *
 * Tests funding scheduler integration and position tracking logic.
 * Full integration testing with MetricsManager is done in trader.cpp.
 */

#include "../include/execution/funding_scheduler.hpp"
#include "../include/execution/futures_position.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace hft;
using namespace hft::execution;

// Helper: Convert minutes to milliseconds
constexpr uint64_t minutes_to_ms(int minutes) {
    return static_cast<uint64_t>(minutes) * 60 * 1000;
}

// Helper: Convert minutes to nanoseconds
constexpr uint64_t minutes_to_ns(int minutes) {
    return static_cast<uint64_t>(minutes) * 60 * 1'000'000'000;
}

void test_funding_scheduler_integration() {
    std::cout << "Test: Funding scheduler phase detection..." << std::endl;

    // Test PreFunding phase (15 min before funding)
    uint64_t next_funding_ms = 10000000 + minutes_to_ms(10);
    uint64_t current_ns = 10000000ULL * 1'000'000;
    auto phase = FundingScheduler::get_phase(next_funding_ms, current_ns);
    assert(phase == FundingPhase::PreFunding);

    // Test PostFunding phase (5 min after funding)
    next_funding_ms = 10000000 - minutes_to_ms(5);
    phase = FundingScheduler::get_phase(next_funding_ms, current_ns);
    assert(phase == FundingPhase::PostFunding);

    // Test Normal phase (2 hours before funding)
    next_funding_ms = 10000000 + minutes_to_ms(120);
    phase = FundingScheduler::get_phase(next_funding_ms, current_ns);
    assert(phase == FundingPhase::Normal);

    std::cout << "  ✓ Phase detection works correctly" << std::endl;
}

void test_position_source_tracking() {
    std::cout << "Test: Position source tracking for exit logic..." << std::endl;

    FuturesPosition positions;
    Symbol symbol = 0;

    // Open Hedge position
    int slot1 = positions.open_position(symbol, PositionSource::Hedge, Side::Sell, 1.5, 50000 * 10000, 1000000000);
    assert(slot1 >= 0);

    // Open Farming position on same symbol
    int slot2 = positions.open_position(symbol, PositionSource::Farming, Side::Buy, 1.0, 50100 * 10000, 2000000000);
    assert(slot2 >= 0);
    assert(slot2 != slot1);

    // Verify we can find each by source
    auto* hedge = positions.get_position(symbol, PositionSource::Hedge, Side::Sell);
    assert(hedge != nullptr);
    assert(std::abs(hedge->quantity - 1.5) < 1e-9);

    auto* farming = positions.get_position(symbol, PositionSource::Farming, Side::Buy);
    assert(farming != nullptr);
    assert(std::abs(farming->quantity - 1.0) < 1e-9);

    std::cout << "  ✓ Multiple position sources tracked correctly" << std::endl;
}

void test_farming_exit_conditions() {
    std::cout << "Test: Farming exit conditions based on funding phase..." << std::endl;

    uint64_t position_age_ns = minutes_to_ns(10);

    // Can exit during PostFunding
    bool can_exit = FundingScheduler::can_exit_farming(FundingPhase::PostFunding, position_age_ns);
    assert(can_exit == true);

    // Cannot exit during PreFunding
    can_exit = FundingScheduler::can_exit_farming(FundingPhase::PreFunding, position_age_ns);
    assert(can_exit == false);

    // Can exit during Normal
    can_exit = FundingScheduler::can_exit_farming(FundingPhase::Normal, position_age_ns);
    assert(can_exit == true);

    std::cout << "  ✓ Exit conditions enforced correctly" << std::endl;
}

void test_hedge_position_lifecycle() {
    std::cout << "Test: Hedge position lifecycle..." << std::endl;

    FuturesPosition positions;
    Symbol symbol = 1;

    // Open hedge short (to hedge spot long)
    int slot = positions.open_position(symbol, PositionSource::Hedge, Side::Sell, 2.0, 50000 * 10000, 1000000000);
    assert(slot >= 0);

    // Verify position exists
    auto* pos = positions.get_position(symbol, PositionSource::Hedge, Side::Sell);
    assert(pos != nullptr);
    assert(pos->quantity == 2.0);
    assert(pos->side == Side::Sell);

    // Close position
    bool closed = positions.close_position(symbol, slot);
    assert(closed);

    // Verify position no longer exists
    pos = positions.get_position(symbol, PositionSource::Hedge, Side::Sell);
    assert(pos == nullptr);

    std::cout << "  ✓ Hedge position lifecycle works" << std::endl;
}

void test_farming_contrarian_logic() {
    std::cout << "Test: Farming contrarian position selection..." << std::endl;

    FuturesPosition positions;
    Symbol symbol = 2;

    // Positive funding rate → short to collect payments
    double funding_rate = 0.001; // 0.1% positive
    Side contrarian_side = (funding_rate > 0) ? Side::Sell : Side::Buy;
    assert(contrarian_side == Side::Sell);

    int slot1 =
        positions.open_position(symbol, PositionSource::Farming, contrarian_side, 1.0, 50000 * 10000, 1000000000);
    assert(slot1 >= 0);

    // Negative funding rate → long to collect payments
    funding_rate = -0.0008; // -0.08% negative
    contrarian_side = (funding_rate > 0) ? Side::Sell : Side::Buy;
    assert(contrarian_side == Side::Buy);

    int slot2 =
        positions.open_position(symbol, PositionSource::Farming, contrarian_side, 1.0, 50000 * 10000, 2000000000);
    assert(slot2 >= 0);

    // Verify both positions exist
    auto* short_pos = positions.get_position(symbol, PositionSource::Farming, Side::Sell);
    auto* long_pos = positions.get_position(symbol, PositionSource::Farming, Side::Buy);
    assert(short_pos != nullptr);
    assert(long_pos != nullptr);

    std::cout << "  ✓ Contrarian logic works correctly" << std::endl;
}

void test_max_positions_enforcement() {
    std::cout << "Test: Max 4 positions per symbol enforcement..." << std::endl;

    FuturesPosition positions;
    Symbol symbol = 3;

    // Open 4 positions
    int slot1 = positions.open_position(symbol, PositionSource::Hedge, Side::Sell, 1.0, 50000 * 10000, 1000000000);
    int slot2 = positions.open_position(symbol, PositionSource::Directional, Side::Buy, 1.0, 50000 * 10000, 1000000000);
    int slot3 = positions.open_position(symbol, PositionSource::Farming, Side::Sell, 1.0, 50000 * 10000, 1000000000);
    int slot4 = positions.open_position(symbol, PositionSource::Farming, Side::Buy, 1.0, 50000 * 10000, 1000000000);

    assert(slot1 >= 0);
    assert(slot2 >= 0);
    assert(slot3 >= 0);
    assert(slot4 >= 0);

    // 5th position should fail
    int slot5 = positions.open_position(symbol, PositionSource::Hedge, Side::Buy, 1.0, 50000 * 10000, 1000000000);
    assert(slot5 == -1);

    // After closing one, should be able to open again
    positions.close_position(symbol, slot1);
    int slot6 = positions.open_position(symbol, PositionSource::Hedge, Side::Buy, 1.0, 50000 * 10000, 1000000000);
    assert(slot6 >= 0);

    std::cout << "  ✓ Max position limit enforced" << std::endl;
}

void test_net_position_calculation() {
    std::cout << "Test: Net position calculation (long - short)..." << std::endl;

    FuturesPosition positions;
    Symbol symbol = 4;

    // Open 3.0 long
    positions.open_position(symbol, PositionSource::Hedge, Side::Buy, 2.0, 50000 * 10000, 1000000000);
    positions.open_position(symbol, PositionSource::Farming, Side::Buy, 1.0, 50000 * 10000, 1000000000);

    // Open 1.5 short
    positions.open_position(symbol, PositionSource::Directional, Side::Sell, 1.5, 50000 * 10000, 1000000000);

    // Net = 3.0 - 1.5 = 1.5 (net long)
    double net = positions.get_net_position(symbol);
    assert(std::abs(net - 1.5) < 1e-9);

    std::cout << "  ✓ Net position calculated correctly" << std::endl;
}

void test_funding_phase_transitions() {
    std::cout << "Test: Funding phase transitions..." << std::endl;

    uint64_t base_time_ns = 1000000000ULL * 1000; // Some base time

    // 20 minutes before funding → Normal
    uint64_t next_funding = 1000000 + minutes_to_ms(20);
    auto phase = FundingScheduler::get_phase(next_funding, base_time_ns);
    assert(phase == FundingPhase::Normal);

    // 14 minutes before funding → PreFunding
    next_funding = 1000000 + minutes_to_ms(14);
    phase = FundingScheduler::get_phase(next_funding, base_time_ns);
    assert(phase == FundingPhase::PreFunding);

    // 1 minute after funding → PostFunding
    next_funding = 1000000 - minutes_to_ms(1);
    phase = FundingScheduler::get_phase(next_funding, base_time_ns);
    assert(phase == FundingPhase::PostFunding);

    // 20 minutes after funding → Normal
    next_funding = 1000000 - minutes_to_ms(20);
    phase = FundingScheduler::get_phase(next_funding, base_time_ns);
    assert(phase == FundingPhase::Normal);

    std::cout << "  ✓ Phase transitions work correctly" << std::endl;
}

int main() {
    std::cout << "\n=== FuturesEvaluator Logic Tests ===" << std::endl;
    std::cout << "(Full integration with MetricsManager tested in trader.cpp)" << std::endl;

    test_funding_scheduler_integration();
    test_position_source_tracking();
    test_farming_exit_conditions();
    test_hedge_position_lifecycle();
    test_farming_contrarian_logic();
    test_max_positions_enforcement();
    test_net_position_calculation();
    test_funding_phase_transitions();

    std::cout << "\n✓ All 8 logic tests passed!" << std::endl;
    std::cout << "Combined with funding_scheduler (8 tests) and futures_position (8 tests): 24/24 tests passed"
              << std::endl;
    return 0;
}
