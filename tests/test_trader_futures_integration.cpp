#include "../include/exchange/binance_futures_ws.hpp"
#include "../include/exchange/futures_market_data.hpp"
#include "../include/metrics/futures_metrics.hpp"

#include <iostream>

using namespace hft;
using namespace hft::exchange;

/**
 * Test that FuturesMetrics can be integrated with BinanceFuturesWs callbacks.
 * This is a compilation test to verify the types and signatures are compatible.
 */
void test_compilation() {
    // Verify types compile together
    FuturesMetrics fm;
    BinanceFuturesWs ws(false);

    // Test mark_price callback integration
    ws.set_mark_price_callback([&](const MarkPriceUpdate& mp) {
        fm.on_mark_price(mp.mark_price, mp.index_price, mp.funding_rate, mp.next_funding_time, mp.event_time * 1000);
    });

    // Test liquidation callback integration
    ws.set_liquidation_callback(
        [&](const LiquidationOrder& lo) { fm.on_liquidation(lo.side, lo.price, lo.quantity, lo.event_time * 1000); });

    // Test book_ticker callback integration
    ws.set_book_ticker_callback(
        [&](const FuturesBookTicker& fbt) { fm.on_futures_bbo(fbt.bid_price, fbt.ask_price, fbt.event_time * 1000); });

    // Verify metrics can be retrieved
    auto metrics = fm.get_metrics(FuturesWindow::W1s);

    // Verify all metric fields exist and are accessible
    static_cast<void>(metrics.funding_rate);
    static_cast<void>(metrics.funding_rate_ema);
    static_cast<void>(metrics.funding_rate_extreme);
    static_cast<void>(metrics.basis);
    static_cast<void>(metrics.basis_bps);
    static_cast<void>(metrics.basis_ema);
    static_cast<void>(metrics.liquidation_volume);
    static_cast<void>(metrics.liquidation_count);
    static_cast<void>(metrics.long_liquidation_volume);
    static_cast<void>(metrics.short_liquidation_volume);
    static_cast<void>(metrics.liquidation_imbalance);

    std::cout << "✓ test_compilation\n";
}

int main() {
    test_compilation();
    std::cout << "\nAll integration tests passed!\n";
    return 0;
}
