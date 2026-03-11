#include "../include/strategy/risk_manager.hpp"

#include <cassert>
#include <iostream>

using namespace hft;
using namespace hft::strategy;

void test_basic_trade_allowed() {
    RiskConfig config;
    config.max_position = 1000;
    config.max_order_size = 100;

    RiskManager rm(config);

    // Trade well within limits
    assert(rm.can_trade(Side::Buy, 50, 0));
    assert(rm.can_trade(Side::Sell, 50, 0));
}

void test_order_size_limit() {
    RiskConfig config;
    config.max_position = 1000;
    config.max_order_size = 100;

    RiskManager rm(config);

    // Order size exceeds limit
    assert(!rm.can_trade(Side::Buy, 150, 0));
    assert(!rm.can_trade(Side::Sell, 150, 0));
}

void test_position_limit_exceeded() {
    // Test that covers line 42: if (std::abs(new_position) > config_.max_position) return false;
    RiskConfig config;
    config.max_position = 1000;
    config.max_order_size = 100;

    RiskManager rm(config);

    // Current position is 950, try to buy 100 more -> 1050 > 1000 (exceeds limit)
    assert(!rm.can_trade(Side::Buy, 100, 950));

    // Current position is -950, try to sell 100 more -> -1050 < -1000 (exceeds limit)
    assert(!rm.can_trade(Side::Sell, 100, -950));

    // Exactly at limit should be allowed
    assert(rm.can_trade(Side::Buy, 100, 900)); // 900 + 100 = 1000
}

void test_halt_on_max_loss() {
    RiskConfig config;
    config.max_loss = 100000;

    RiskManager rm(config);

    assert(!rm.is_halted());

    // Trigger halt with loss exceeding max_loss
    rm.update_pnl(-150000);

    assert(rm.is_halted());

    // No trades allowed when halted
    assert(!rm.can_trade(Side::Buy, 10, 0));
    assert(!rm.can_trade(Side::Sell, 10, 0));
}

void test_reset_halt() {
    RiskConfig config;
    config.max_loss = 100000;

    RiskManager rm(config);

    rm.update_pnl(-150000);
    assert(rm.is_halted());

    rm.reset_halt();
    assert(!rm.is_halted());

    // Can trade again after reset
    assert(rm.can_trade(Side::Buy, 10, 0));
}

void test_pnl_tracking() {
    RiskConfig config;

    RiskManager rm(config);

    assert(rm.current_pnl() == 0);

    rm.update_pnl(50000);
    assert(rm.current_pnl() == 50000);

    rm.update_pnl(-30000);
    assert(rm.current_pnl() == -30000);
}

int main() {
    test_basic_trade_allowed();
    test_order_size_limit();
    test_position_limit_exceeded();
    test_halt_on_max_loss();
    test_reset_halt();
    test_pnl_tracking();

    std::cout << "All risk_manager tests passed!\n";
    return 0;
}
