#pragma once

#include "../types.hpp"
#include <cstdint>
#include <cstdlib>

namespace hft {
namespace strategy {

struct RiskConfig {
    int64_t max_position = 1000;     // Maximum absolute position
    Quantity max_order_size = 100;   // Maximum single order size
    int64_t max_loss = 100000;       // Maximum loss before halt
    int64_t max_notional = 10000000; // Maximum notional exposure
};

// Real-time risk manager
// Checks limits before allowing trades
class RiskManager {
public:
    explicit RiskManager(const RiskConfig& config)
        : config_(config)
        , current_pnl_(0)
        , halted_(false)
    {}

    // Check if a trade is allowed
    bool can_trade(Side side, Quantity size, int64_t current_position) const {
        if (halted_) return false;

        // Check order size limit
        if (size > config_.max_order_size) return false;

        // Check position limit after trade
        int64_t new_position = current_position;
        if (side == Side::Buy) {
            new_position += size;
        } else {
            new_position -= size;
        }

        if (std::abs(new_position) > config_.max_position) return false;

        return true;
    }

    // Update P&L and check for halt condition
    void update_pnl(int64_t pnl) {
        current_pnl_ = pnl;
        if (pnl < -config_.max_loss) {
            halted_ = true;
        }
    }

    bool is_halted() const { return halted_; }

    void reset_halt() { halted_ = false; }

    int64_t current_pnl() const { return current_pnl_; }

    const RiskConfig& config() const { return config_; }

private:
    RiskConfig config_;
    int64_t current_pnl_;
    bool halted_;
};

}  // namespace strategy
}  // namespace hft
