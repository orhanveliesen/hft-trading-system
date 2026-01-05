/**
 * Binance WebSocket Test
 *
 * Tests real-time market data streaming from Binance.
 *
 * Usage:
 *   ./ws_test BTCUSDT [duration_seconds]
 */

#include "../include/exchange/binance_ws.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>

using namespace hft;
using namespace hft::exchange;

std::atomic<bool> g_running{true};

void signal_handler(int) {
    std::cout << "\nShutting down...\n";
    g_running = false;
}

std::string format_price(Price price) {
    return std::to_string(price / 10000) + "." +
           std::to_string((price % 10000) / 100);
}

int main(int argc, char* argv[]) {
    std::string symbol = "BTCUSDT";
    int duration = 30;  // Default 30 seconds

    if (argc >= 2) {
        symbol = argv[1];
    }
    if (argc >= 3) {
        duration = std::stoi(argv[2]);
    }

    std::cout << "Binance WebSocket Test\n";
    std::cout << "======================\n";
    std::cout << "Symbol: " << symbol << "\n";
    std::cout << "Duration: " << duration << " seconds\n\n";

    signal(SIGINT, signal_handler);

    // Stats
    std::atomic<uint64_t> trade_count{0};
    std::atomic<uint64_t> book_update_count{0};
    Price last_bid = 0, last_ask = 0;
    Price last_trade_price = 0;

    BinanceWs ws(false);  // Use mainnet

    // Connection callback
    ws.set_connect_callback([](bool connected) {
        if (connected) {
            std::cout << "[CONNECTED] WebSocket connected to Binance\n";
        } else {
            std::cout << "[DISCONNECTED] WebSocket disconnected\n";
        }
    });

    // Error callback
    ws.set_error_callback([](const std::string& error) {
        std::cerr << "[ERROR] " << error << "\n";
    });

    // Book ticker callback
    ws.set_book_ticker_callback([&](const BookTicker& bt) {
        book_update_count++;
        last_bid = bt.bid_price;
        last_ask = bt.ask_price;
    });

    // Trade callback
    ws.set_trade_callback([&](const WsTrade& trade) {
        trade_count++;
        last_trade_price = trade.price;

        // Print every 100th trade
        if (trade_count % 100 == 0) {
            std::cout << "[TRADE] " << trade.symbol
                      << " price=" << format_price(trade.price)
                      << " qty=" << std::fixed << std::setprecision(4) << trade.quantity
                      << " side=" << (trade.is_buyer_maker ? "SELL" : "BUY")
                      << " (total: " << trade_count << ")\n";
        }
    });

    // Subscribe to streams
    ws.subscribe_book_ticker(symbol);
    ws.subscribe_trade(symbol);

    std::cout << "Connecting...\n";

    if (!ws.connect()) {
        std::cerr << "Failed to start WebSocket\n";
        return 1;
    }

    // Wait for connection
    for (int i = 0; i < 50 && !ws.is_connected() && g_running; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!ws.is_connected()) {
        std::cerr << "Connection timeout\n";
        return 1;
    }

    // Run for duration
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::seconds(duration);

    std::cout << "\nReceiving data...\n\n";

    while (g_running && std::chrono::steady_clock::now() < end_time) {
        std::this_thread::sleep_for(std::chrono::seconds(5));

        // Print summary
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time).count();

        std::cout << "[SUMMARY] " << elapsed << "s - "
                  << "Trades: " << trade_count
                  << ", Book updates: " << book_update_count
                  << ", Bid: " << format_price(last_bid)
                  << ", Ask: " << format_price(last_ask)
                  << ", Spread: " << format_price(last_ask - last_bid)
                  << "\n";
    }

    std::cout << "\nDisconnecting...\n";
    ws.disconnect();

    // Final stats
    auto total_time = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start_time).count();

    std::cout << "\n=== Final Statistics ===\n";
    std::cout << "Duration: " << total_time << " seconds\n";
    std::cout << "Total trades: " << trade_count << "\n";
    std::cout << "Total book updates: " << book_update_count << "\n";
    if (total_time > 0) {
        std::cout << "Trades/sec: " << (trade_count / total_time) << "\n";
        std::cout << "Updates/sec: " << (book_update_count / total_time) << "\n";
    }

    return 0;
}
