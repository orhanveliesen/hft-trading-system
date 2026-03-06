#include "../include/config/strategy_factory.hpp"

#include <cassert>
#include <iostream>

using namespace hft;
using namespace hft::config;

// Test 1: SMA strategy name
void test_sma_name() {
    StrategyParams params;
    params.sma_fast = 10;
    params.sma_slow = 20;

    std::string name = StrategyFactory::get_name(StrategyType::SMA, params);
    assert(name == "SMA(10/20)");
    std::cout << "[PASS] test_sma_name\n";
}

// Test 2: RSI strategy name
void test_rsi_name() {
    StrategyParams params;
    params.rsi_period = 14;
    params.rsi_oversold = 30.0;
    params.rsi_overbought = 70.0;

    std::string name = StrategyFactory::get_name(StrategyType::RSI, params);
    assert(name == "RSI(14,30/70)");
    std::cout << "[PASS] test_rsi_name\n";
}

// Test 3: MeanReversion strategy name
void test_mean_reversion_name() {
    StrategyParams params;
    params.mr_lookback = 50;
    params.mr_std_mult = 2.0;

    std::string name = StrategyFactory::get_name(StrategyType::MeanReversion, params);
    // get_name uses substr(0,3) so "2.0" becomes "2.0"
    assert(name.find("MeanRev(50,") == 0);
    std::cout << "[PASS] test_mean_reversion_name\n";
}

// Test 4: Breakout strategy name
void test_breakout_name() {
    StrategyParams params;
    params.breakout_lookback = 100;

    std::string name = StrategyFactory::get_name(StrategyType::Breakout, params);
    assert(name == "Breakout(100)");
    std::cout << "[PASS] test_breakout_name\n";
}

// Test 5: MACD strategy name
void test_macd_name() {
    StrategyParams params;
    params.macd_fast = 12;
    params.macd_slow = 26;
    params.macd_signal = 9;

    std::string name = StrategyFactory::get_name(StrategyType::MACD, params);
    assert(name == "MACD(12/26/9)");
    std::cout << "[PASS] test_macd_name\n";
}

// Test 6: SimpleMR_HFT strategy name
void test_simple_mr_hft_name() {
    StrategyParams params;

    std::string name = StrategyFactory::get_name(StrategyType::SimpleMR_HFT, params);
    assert(name == "SimpleMR_HFT");
    std::cout << "[PASS] test_simple_mr_hft_name\n";
}

// Test 7: Momentum_HFT strategy name
void test_momentum_hft_name() {
    StrategyParams params;
    params.momentum_lookback = 30;
    params.momentum_threshold_bps = 5;

    std::string name = StrategyFactory::get_name(StrategyType::Momentum_HFT, params);
    assert(name == "Momentum_HFT(30,5bps)");
    std::cout << "[PASS] test_momentum_hft_name\n";
}

// Test 8: Invalid StrategyType enum value → returns "Unknown"
void test_invalid_strategy_type() {
    StrategyParams params;

    // Cast invalid value to StrategyType enum
    StrategyType invalid_type = static_cast<StrategyType>(99);

    std::string name = StrategyFactory::get_name(invalid_type, params);
    assert(name == "Unknown");
    std::cout << "[PASS] test_invalid_strategy_type\n";
}

int main() {
    std::cout << "Running StrategyFactory tests...\n\n";

    test_sma_name();
    test_rsi_name();
    test_mean_reversion_name();
    test_breakout_name();
    test_macd_name();
    test_simple_mr_hft_name();
    test_momentum_hft_name();
    test_invalid_strategy_type();

    std::cout << "\n8 StrategyFactory tests passed!\n";
    std::cout << "✓ Coverage: All StrategyType enum values tested\n";
    std::cout << "✓ Invalid enum default case covered\n";
    return 0;
}
