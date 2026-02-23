#include "../include/mock_order_sender.hpp"
#include "../include/symbol_config.hpp"
#include "../include/trading_engine.hpp"
#include "../include/types.hpp"

#include <cassert>
#include <cstring>
#include <iostream>

using namespace hft;

void test_symbol_config_creation() {
    SymbolConfig config;
    config.symbol = "AAPL";
    config.base_price = 170'0000; // $170.00 (4 decimals)
    config.price_range = 100'000; // $10 range

    assert(config.symbol == "AAPL");
    assert(config.base_price == 170'0000);
    assert(config.price_range == 100'000);
    std::cout << "[PASS] test_symbol_config_creation\n";
}

void test_trading_engine_add_symbol() {
    MockOrderSender sender;
    TradingEngine<MockOrderSender> engine(sender);

    SymbolConfig aapl_config;
    aapl_config.symbol = "AAPL";
    aapl_config.base_price = 170'0000;
    aapl_config.price_range = 100'000;

    engine.add_symbol(aapl_config);

    assert(engine.has_symbol("AAPL"));
    assert(!engine.has_symbol("GOOGL"));
    std::cout << "[PASS] test_trading_engine_add_symbol\n";
}

void test_trading_engine_multiple_symbols() {
    MockOrderSender sender;
    TradingEngine<MockOrderSender> engine(sender);

    SymbolConfig aapl{"AAPL", 170'0000, 100'000};
    SymbolConfig tsla{"TSLA", 250'0000, 200'000};
    SymbolConfig nvda{"NVDA", 450'0000, 300'000};

    engine.add_symbol(aapl);
    engine.add_symbol(tsla);
    engine.add_symbol(nvda);

    assert(engine.has_symbol("AAPL"));
    assert(engine.has_symbol("TSLA"));
    assert(engine.has_symbol("NVDA"));
    assert(engine.symbol_count() == 3);
    std::cout << "[PASS] test_trading_engine_multiple_symbols\n";
}

void test_trading_engine_get_orderbook() {
    MockOrderSender sender;
    TradingEngine<MockOrderSender> engine(sender);

    SymbolConfig aapl{"AAPL", 170'0000, 100'000};
    engine.add_symbol(aapl);

    OrderBook* book = engine.get_orderbook("AAPL");
    assert(book != nullptr);

    // Add order through engine
    book->add_order(1, Side::Buy, 170'5000, 100); // $170.50
    assert(book->best_bid() == 170'5000);

    OrderBook* null_book = engine.get_orderbook("GOOGL");
    assert(null_book == nullptr);

    std::cout << "[PASS] test_trading_engine_get_orderbook\n";
}

void test_trading_engine_process_message() {
    MockOrderSender sender;
    TradingEngine<MockOrderSender> engine(sender);

    SymbolConfig aapl{"AAPL", 170'0000, 100'000};
    SymbolConfig tsla{"TSLA", 250'0000, 200'000};
    engine.add_symbol(aapl);
    engine.add_symbol(tsla);

    // Process add order for AAPL
    engine.on_add_order("AAPL", 1, Side::Buy, 170'5000, 100);
    engine.on_add_order("TSLA", 2, Side::Sell, 251'0000, 50);

    // Ignored symbol
    engine.on_add_order("GOOGL", 3, Side::Buy, 140'0000, 200);

    assert(engine.get_orderbook("AAPL")->best_bid() == 170'5000);
    assert(engine.get_orderbook("TSLA")->best_ask() == 251'0000);

    std::cout << "[PASS] test_trading_engine_process_message\n";
}

void test_trading_engine_cancel_execute() {
    MockOrderSender sender;
    TradingEngine<MockOrderSender> engine(sender);

    SymbolConfig aapl{"AAPL", 170'0000, 100'000};
    engine.add_symbol(aapl);

    engine.on_add_order("AAPL", 1, Side::Buy, 170'5000, 100);
    engine.on_add_order("AAPL", 2, Side::Buy, 170'5000, 200);

    assert(engine.get_orderbook("AAPL")->bid_quantity_at(170'5000) == 300);

    // Partial execute
    engine.on_execute_order("AAPL", 1, 50);
    assert(engine.get_orderbook("AAPL")->bid_quantity_at(170'5000) == 250);

    // Cancel
    engine.on_cancel_order("AAPL", 2);
    assert(engine.get_orderbook("AAPL")->bid_quantity_at(170'5000) == 50);

    std::cout << "[PASS] test_trading_engine_cancel_execute\n";
}

void test_symbol_from_itch_format() {
    // ITCH uses 8-char padded symbols: "AAPL    "
    char itch_symbol[8] = {'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '};

    std::string symbol = trim_symbol(itch_symbol, 8);
    assert(symbol == "AAPL");

    char itch_tsla[8] = {'T', 'S', 'L', 'A', ' ', ' ', ' ', ' '};
    assert(trim_symbol(itch_tsla, 8) == "TSLA");

    char itch_brk[8] = {'B', 'R', 'K', '.', 'A', ' ', ' ', ' '};
    assert(trim_symbol(itch_brk, 8) == "BRK.A");

    std::cout << "[PASS] test_symbol_from_itch_format\n";
}

int main() {
    std::cout << "=== Symbol Config Tests ===\n\n";

    test_symbol_config_creation();
    test_trading_engine_add_symbol();
    test_trading_engine_multiple_symbols();
    test_trading_engine_get_orderbook();
    test_trading_engine_process_message();
    test_trading_engine_cancel_execute();
    test_symbol_from_itch_format();

    std::cout << "\n=== All Symbol Config Tests Passed ===\n";
    return 0;
}
