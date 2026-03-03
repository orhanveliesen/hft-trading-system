#include "../include/exchange/futures_market_data.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace hft;
using namespace hft::exchange;

// ============================================================================
// FundingRate Tests
// ============================================================================

void test_funding_rate_default_construction() {
    FundingRate fr;
    assert(fr.symbol.empty());
    assert(fr.funding_rate == 0.0);
    assert(fr.mark_price == 0.0);
    assert(fr.funding_time == 0);
    assert(fr.event_time == 0);
    std::cout << "✓ test_funding_rate_default_construction\n";
}

void test_funding_rate_field_assignment() {
    FundingRate fr;
    fr.symbol = "BTCUSDT";
    fr.funding_rate = 0.0001;
    fr.mark_price = 42000.50;
    fr.funding_time = 1700028800000;
    fr.event_time = 1700000000000;

    assert(fr.symbol == "BTCUSDT");
    assert(std::abs(fr.funding_rate - 0.0001) < 1e-10);
    assert(std::abs(fr.mark_price - 42000.50) < 0.01);
    assert(fr.funding_time == 1700028800000);
    assert(fr.event_time == 1700000000000);
    std::cout << "✓ test_funding_rate_field_assignment\n";
}

// ============================================================================
// MarkPriceUpdate Tests
// ============================================================================

void test_mark_price_default_construction() {
    MarkPriceUpdate mp;
    assert(mp.symbol.empty());
    assert(mp.mark_price == 0.0);
    assert(mp.index_price == 0.0);
    assert(mp.funding_rate == 0.0);
    assert(mp.next_funding_time == 0);
    assert(mp.event_time == 0);
    std::cout << "✓ test_mark_price_default_construction\n";
}

void test_mark_price_field_assignment() {
    MarkPriceUpdate mp;
    mp.symbol = "BTCUSDT";
    mp.mark_price = 42000.50;
    mp.index_price = 41950.00;
    mp.funding_rate = 0.00010000;
    mp.next_funding_time = 1700028800000;
    mp.event_time = 1700000000000;

    assert(mp.symbol == "BTCUSDT");
    assert(std::abs(mp.mark_price - 42000.50) < 0.01);
    assert(std::abs(mp.index_price - 41950.00) < 0.01);
    assert(std::abs(mp.funding_rate - 0.0001) < 1e-10);
    assert(mp.next_funding_time == 1700028800000);
    assert(mp.event_time == 1700000000000);
    std::cout << "✓ test_mark_price_field_assignment\n";
}

// Test that mark_price uses double (not Price) to avoid overflow
void test_mark_price_large_values() {
    MarkPriceUpdate mp;
    mp.mark_price = 430000.0; // BTC at 430K+ would overflow uint32_t * 10000
    mp.index_price = 429500.0;

    assert(std::abs(mp.mark_price - 430000.0) < 0.01);
    assert(std::abs(mp.index_price - 429500.0) < 0.01);
    std::cout << "✓ test_mark_price_large_values\n";
}

// ============================================================================
// OpenInterest Tests
// ============================================================================

void test_open_interest_default_construction() {
    OpenInterest oi;
    assert(oi.symbol.empty());
    assert(oi.open_interest == 0.0);
    assert(oi.open_interest_value == 0.0);
    assert(oi.time == 0);
    std::cout << "✓ test_open_interest_default_construction\n";
}

void test_open_interest_field_assignment() {
    OpenInterest oi;
    oi.symbol = "BTCUSDT";
    oi.open_interest = 50000.0;
    oi.open_interest_value = 2100000000.0;
    oi.time = 1700000000000;

    assert(oi.symbol == "BTCUSDT");
    assert(std::abs(oi.open_interest - 50000.0) < 0.01);
    assert(std::abs(oi.open_interest_value - 2100000000.0) < 1.0);
    assert(oi.time == 1700000000000);
    std::cout << "✓ test_open_interest_field_assignment\n";
}

// ============================================================================
// LiquidationOrder Tests
// ============================================================================

void test_liquidation_order_default_construction() {
    LiquidationOrder lo;
    assert(lo.symbol.empty());
    assert(lo.side == Side::Buy); // Default Side
    assert(lo.price == 0.0);
    assert(lo.quantity == 0.0);
    assert(lo.avg_price == 0.0);
    assert(lo.order_status.empty());
    assert(lo.trade_time == 0);
    assert(lo.event_time == 0);
    std::cout << "✓ test_liquidation_order_default_construction\n";
}

void test_liquidation_order_field_assignment() {
    LiquidationOrder lo;
    lo.symbol = "BTCUSDT";
    lo.side = Side::Sell;
    lo.price = 42000.00; // double (prevents overflow)
    lo.quantity = 0.010;
    lo.avg_price = 42100.00;
    lo.order_status = "FILLED";
    lo.trade_time = 1700000000000;
    lo.event_time = 1700000000000;

    assert(lo.symbol == "BTCUSDT");
    assert(lo.side == Side::Sell);
    assert(std::abs(lo.price - 42000.00) < 0.01);
    assert(std::abs(lo.quantity - 0.010) < 0.001);
    assert(std::abs(lo.avg_price - 42100.00) < 0.01);
    assert(lo.order_status == "FILLED");
    assert(lo.trade_time == 1700000000000);
    assert(lo.event_time == 1700000000000);
    std::cout << "✓ test_liquidation_order_field_assignment\n";
}

void test_liquidation_order_side_enum() {
    LiquidationOrder buy_liq;
    buy_liq.side = Side::Buy;
    assert(buy_liq.side == Side::Buy);

    LiquidationOrder sell_liq;
    sell_liq.side = Side::Sell;
    assert(sell_liq.side == Side::Sell);

    std::cout << "✓ test_liquidation_order_side_enum\n";
}

// ============================================================================
// FuturesBookTicker Tests
// ============================================================================

void test_futures_book_ticker_default_construction() {
    FuturesBookTicker fbt;
    assert(fbt.symbol.empty());
    assert(fbt.bid_price == 0.0);
    assert(fbt.bid_qty == 0.0);
    assert(fbt.ask_price == 0.0);
    assert(fbt.ask_qty == 0.0);
    assert(fbt.transaction_time == 0);
    assert(fbt.event_time == 0);
    std::cout << "✓ test_futures_book_ticker_default_construction\n";
}

void test_futures_book_ticker_field_assignment() {
    FuturesBookTicker fbt;
    fbt.symbol = "BTCUSDT";
    fbt.bid_price = 42000.50; // double (prevents overflow)
    fbt.bid_qty = 1.5;
    fbt.ask_price = 42001.00;
    fbt.ask_qty = 2.0;
    fbt.transaction_time = 1700000000000;
    fbt.event_time = 1700000001000;

    assert(fbt.symbol == "BTCUSDT");
    assert(std::abs(fbt.bid_price - 42000.50) < 0.01);
    assert(std::abs(fbt.bid_qty - 1.5) < 0.01);
    assert(std::abs(fbt.ask_price - 42001.00) < 0.01);
    assert(std::abs(fbt.ask_qty - 2.0) < 0.01);
    assert(fbt.transaction_time == 1700000000000);
    assert(fbt.event_time == 1700000001000);
    std::cout << "✓ test_futures_book_ticker_field_assignment\n";
}

// Test difference between FuturesBookTicker and BookTicker (spot)
// FuturesBookTicker has transaction_time + event_time
// BookTicker (spot) only has update_time
void test_futures_vs_spot_book_ticker() {
    FuturesBookTicker fbt;
    fbt.transaction_time = 1700000000000;
    fbt.event_time = 1700000001000;

    // Verify both timestamps exist
    assert(fbt.transaction_time == 1700000000000);
    assert(fbt.event_time == 1700000001000);

    std::cout << "✓ test_futures_vs_spot_book_ticker\n";
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "Running futures market data tests...\n\n";

    // FundingRate
    test_funding_rate_default_construction();
    test_funding_rate_field_assignment();

    // MarkPriceUpdate
    test_mark_price_default_construction();
    test_mark_price_field_assignment();
    test_mark_price_large_values();

    // OpenInterest
    test_open_interest_default_construction();
    test_open_interest_field_assignment();

    // LiquidationOrder
    test_liquidation_order_default_construction();
    test_liquidation_order_field_assignment();
    test_liquidation_order_side_enum();

    // FuturesBookTicker
    test_futures_book_ticker_default_construction();
    test_futures_book_ticker_field_assignment();
    test_futures_vs_spot_book_ticker();

    std::cout << "\n✅ All futures market data tests passed!\n";
    return 0;
}
