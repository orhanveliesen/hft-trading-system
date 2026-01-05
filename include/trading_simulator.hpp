#pragma once

#include "types.hpp"
#include "strategy/market_maker.hpp"
#include "strategy/position.hpp"
#include "strategy/risk_manager.hpp"
#include <cstdint>

namespace hft {

struct SimulatorConfig {
    uint32_t spread_bps = 10;        // Spread in basis points
    Quantity quote_size = 100;       // Default quote size
    int64_t max_position = 1000;     // Position limit
    double skew_factor = 0.5;        // Quote skew factor
    int64_t max_loss = 100000;       // Max loss before halt
    Quantity max_order_size = 100;   // Max single order size
};

class TradingSimulator {
public:
    explicit TradingSimulator(const SimulatorConfig& config)
        : config_(config)
        , market_maker_(create_mm_config(config))
        , risk_manager_(create_risk_config(config))
        , quotes_generated_(0)
        , last_mid_(0)
    {}

    // Process market data tick - returns quotes to place
    strategy::Quote on_market_data(Price bid, Price ask,
                                    Quantity bid_size, Quantity ask_size) {
        (void)bid_size;  // Could use for liquidity-based sizing
        (void)ask_size;

        // Calculate mid price
        last_mid_ = (bid + ask) / 2;

        // Check if trading is halted
        if (risk_manager_.is_halted()) {
            return strategy::Quote{};  // No quotes
        }

        // Generate quotes from market maker
        auto quote = market_maker_.generate_quotes(last_mid_, position_.position());

        if (quote.has_bid || quote.has_ask) {
            ++quotes_generated_;
        }

        return quote;
    }

    // Process a fill (our order was executed)
    void on_fill(Side side, Quantity qty, Price price) {
        // Update position
        position_.on_fill(side, qty, price);

        // Use fill price as mark if no market data yet
        Price mark = (last_mid_ > 0) ? last_mid_ : price;

        // Update risk manager with current P&L
        risk_manager_.update_pnl(position_.total_pnl(mark));
    }

    // Accessors
    int64_t position() const { return position_.position(); }
    int64_t realized_pnl() const { return position_.realized_pnl(); }
    int64_t unrealized_pnl() const { return position_.unrealized_pnl(last_mid_); }
    int64_t total_pnl() const { return position_.total_pnl(last_mid_); }
    bool is_halted() const { return risk_manager_.is_halted(); }
    uint64_t total_quotes_generated() const { return quotes_generated_; }

    // Reset for new simulation
    void reset() {
        position_.reset();
        risk_manager_.reset_halt();
        quotes_generated_ = 0;
        last_mid_ = 0;
    }

    const SimulatorConfig& config() const { return config_; }

private:
    static strategy::MarketMakerConfig create_mm_config(const SimulatorConfig& cfg) {
        return strategy::MarketMakerConfig{
            cfg.spread_bps,
            cfg.quote_size,
            cfg.max_position,
            cfg.skew_factor
        };
    }

    static strategy::RiskConfig create_risk_config(const SimulatorConfig& cfg) {
        return strategy::RiskConfig{
            cfg.max_position,
            cfg.max_order_size,
            cfg.max_loss,
            10000000  // Default max notional
        };
    }

    SimulatorConfig config_;
    strategy::MarketMaker market_maker_;
    strategy::PositionTracker position_;
    strategy::RiskManager risk_manager_;
    uint64_t quotes_generated_;
    Price last_mid_;
};

}  // namespace hft
