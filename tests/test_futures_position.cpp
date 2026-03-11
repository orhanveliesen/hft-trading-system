/**
 * Test suite for FuturesPosition
 *
 * Tests per-symbol futures position tracking with source (Hedge/Directional/Farming).
 */

#include "../include/execution/futures_position.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace hft;
using namespace hft::execution;

void test_open_hedge_position() {
    std::cout << "Test: Open hedge position..." << std::endl;

    FuturesPosition positions;

    Symbol symbol = 0;
    int slot = positions.open_position(symbol, PositionSource::Hedge, Side::Sell, 1.5, 50000 * 10000, 1000000000);

    assert(slot >= 0);
    assert(slot < 4);

    // Verify stored correctly
    auto* pos = positions.get_position(symbol, PositionSource::Hedge, Side::Sell);
    assert(pos != nullptr);
    assert(pos->source == PositionSource::Hedge);
    assert(pos->side == Side::Sell);
    assert(std::abs(pos->quantity - 1.5) < 1e-9);
    assert(pos->entry_price == 50000 * 10000);
    assert(pos->open_time_ns == 1000000000);

    std::cout << "  ✓ Hedge short position stored correctly" << std::endl;
}

void test_open_multiple_positions_same_symbol() {
    std::cout << "Test: Open multiple positions on same symbol..." << std::endl;

    FuturesPosition positions;

    Symbol symbol = 1;

    // Open Hedge long
    int slot1 = positions.open_position(symbol, PositionSource::Hedge, Side::Buy, 2.0, 50000 * 10000, 1000000000);
    assert(slot1 >= 0);

    // Open Farming short on same symbol
    int slot2 = positions.open_position(symbol, PositionSource::Farming, Side::Sell, 1.0, 51000 * 10000, 2000000000);
    assert(slot2 >= 0);
    assert(slot2 != slot1); // Different slots

    // Verify both exist
    auto* hedge = positions.get_position(symbol, PositionSource::Hedge, Side::Buy);
    assert(hedge != nullptr);
    assert(std::abs(hedge->quantity - 2.0) < 1e-9);

    auto* farming = positions.get_position(symbol, PositionSource::Farming, Side::Sell);
    assert(farming != nullptr);
    assert(std::abs(farming->quantity - 1.0) < 1e-9);

    std::cout << "  ✓ Hedge long + Farming short coexist" << std::endl;
}

void test_get_net_position_long_and_short() {
    std::cout << "Test: Get net position (long - short)..." << std::endl;

    FuturesPosition positions;

    Symbol symbol = 2;

    // Open 2.0 long
    positions.open_position(symbol, PositionSource::Hedge, Side::Buy, 2.0, 50000 * 10000, 1000000000);

    // Open 1.5 short
    positions.open_position(symbol, PositionSource::Farming, Side::Sell, 1.5, 51000 * 10000, 2000000000);

    // Net = 2.0 - 1.5 = 0.5 (net long)
    double net = positions.get_net_position(symbol);
    assert(std::abs(net - 0.5) < 1e-9);

    std::cout << "  ✓ Net position = 2.0 long - 1.5 short = 0.5" << std::endl;
}

void test_close_position_by_symbol_slot() {
    std::cout << "Test: Close position by symbol and slot..." << std::endl;

    FuturesPosition positions;

    Symbol symbol = 3;

    int slot = positions.open_position(symbol, PositionSource::Farming, Side::Buy, 1.0, 50000 * 10000, 1000000000);
    assert(slot >= 0);

    // Verify it exists
    auto* pos = positions.get_position(symbol, PositionSource::Farming, Side::Buy);
    assert(pos != nullptr);

    // Close it
    bool closed = positions.close_position(symbol, slot);
    assert(closed);

    // Verify it's gone
    auto* pos_after = positions.get_position(symbol, PositionSource::Farming, Side::Buy);
    assert(pos_after == nullptr);

    std::cout << "  ✓ Position closed successfully" << std::endl;
}

void test_get_position_by_source() {
    std::cout << "Test: Get position by source and side (O(4) search)..." << std::endl;

    FuturesPosition positions;

    Symbol symbol = 4;

    // Open Hedge long
    positions.open_position(symbol, PositionSource::Hedge, Side::Buy, 3.0, 50000 * 10000, 1000000000);

    // Open Farming short
    positions.open_position(symbol, PositionSource::Farming, Side::Sell, 2.0, 51000 * 10000, 2000000000);

    // Find Hedge long (should succeed)
    auto* hedge = positions.get_position(symbol, PositionSource::Hedge, Side::Buy);
    assert(hedge != nullptr);
    assert(std::abs(hedge->quantity - 3.0) < 1e-9);

    // Find Farming long (should fail - it's short, not long)
    auto* farming_long = positions.get_position(symbol, PositionSource::Farming, Side::Buy);
    assert(farming_long == nullptr);

    // Find Farming short (should succeed)
    auto* farming_short = positions.get_position(symbol, PositionSource::Farming, Side::Sell);
    assert(farming_short != nullptr);
    assert(std::abs(farming_short->quantity - 2.0) < 1e-9);

    std::cout << "  ✓ O(4) search finds correct positions" << std::endl;
}

void test_max_positions_reached_per_symbol() {
    std::cout << "Test: Max 4 positions per symbol..." << std::endl;

    FuturesPosition positions;

    Symbol symbol = 5;

    // Open 4 positions (max)
    int slot1 = positions.open_position(symbol, PositionSource::Hedge, Side::Buy, 1.0, 50000 * 10000, 1000000000);
    int slot2 = positions.open_position(symbol, PositionSource::Directional, Side::Buy, 1.0, 50000 * 10000, 1000000000);
    int slot3 = positions.open_position(symbol, PositionSource::Farming, Side::Sell, 1.0, 50000 * 10000, 1000000000);
    int slot4 = positions.open_position(symbol, PositionSource::Farming, Side::Buy, 1.0, 50000 * 10000, 1000000000);

    assert(slot1 >= 0);
    assert(slot2 >= 0);
    assert(slot3 >= 0);
    assert(slot4 >= 0);

    // Try to open 5th position (should fail)
    int slot5 = positions.open_position(symbol, PositionSource::Hedge, Side::Sell, 1.0, 50000 * 10000, 1000000000);
    assert(slot5 == -1); // Failed

    std::cout << "  ✓ 4 positions opened, 5th rejected" << std::endl;
}

void test_symbol_indexed_isolation() {
    std::cout << "Test: Symbol-indexed isolation..." << std::endl;

    FuturesPosition positions;

    // Open position for symbol 0
    positions.open_position(0, PositionSource::Hedge, Side::Buy, 1.0, 50000 * 10000, 1000000000);

    // Open position for symbol 1
    positions.open_position(1, PositionSource::Hedge, Side::Buy, 2.0, 51000 * 10000, 2000000000);

    // Verify symbol 0 has quantity 1.0
    auto* pos0 = positions.get_position(0, PositionSource::Hedge, Side::Buy);
    assert(pos0 != nullptr);
    assert(std::abs(pos0->quantity - 1.0) < 1e-9);

    // Verify symbol 1 has quantity 2.0
    auto* pos1 = positions.get_position(1, PositionSource::Hedge, Side::Buy);
    assert(pos1 != nullptr);
    assert(std::abs(pos1->quantity - 2.0) < 1e-9);

    // Verify symbol 2 has no positions
    auto* pos2 = positions.get_position(2, PositionSource::Hedge, Side::Buy);
    assert(pos2 == nullptr);

    std::cout << "  ✓ Positions isolated by symbol index" << std::endl;
}

void test_clear_position_sets_none() {
    std::cout << "Test: Clear position sets source=None..." << std::endl;

    FuturesPosition positions;

    Symbol symbol = 6;

    int slot = positions.open_position(symbol, PositionSource::Farming, Side::Buy, 1.0, 50000 * 10000, 1000000000);
    assert(slot >= 0);

    // Close position
    bool closed = positions.close_position(symbol, slot);
    assert(closed);

    // Verify can_open_position returns true (slot is free again)
    assert(positions.can_open_position(symbol));

    std::cout << "  ✓ Cleared position is reusable" << std::endl;
}

int main() {
    std::cout << "\n=== FuturesPosition Tests ===" << std::endl;

    test_open_hedge_position();
    test_open_multiple_positions_same_symbol();
    test_get_net_position_long_and_short();
    test_close_position_by_symbol_slot();
    test_get_position_by_source();
    test_max_positions_reached_per_symbol();
    test_symbol_indexed_isolation();
    test_clear_position_sets_none();

    std::cout << "\n✓ All 8 tests passed!" << std::endl;
    return 0;
}
