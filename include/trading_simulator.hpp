#pragma once

#include "types.hpp"
#include "strategy/market_maker.hpp"
#include "strategy/position.hpp"
#include "risk/enhanced_risk_manager.hpp"
#include <cstdint>
#include <string>

namespace hft {

struct SimulatorConfig {
    // Market maker settings
    uint32_t spread_bps = 10;        // Spread in basis points
    Quantity quote_size = 100;       // Default quote size
    double skew_factor = 0.5;        // Quote skew factor

    // Risk settings (maps to EnhancedRiskConfig)
    Capital initial_capital = 100000;   // Starting capital
    Position max_position = 1000;       // Position limit per symbol
    PnL daily_loss_limit = 100000;      // Max loss before halt
    Quantity max_order_size = 100;      // Max single order size
    double max_drawdown_pct = 0.10;     // 10% max drawdown
    Notional max_notional = 10000000;   // Max notional per symbol

    // Symbol to trade
    std::string symbol = "SIM";
};

class TradingSimulator {
public:
    explicit TradingSimulator(const SimulatorConfig& config)
        : config_(config)
        , market_maker_(create_mm_config(config))
        , risk_manager_(create_risk_config(config))
        , symbol_index_(risk::INVALID_SYMBOL_INDEX)
        , quotes_generated_(0)
        , last_mid_(0)
    {
        // Register symbol and cache index for hot path
        symbol_index_ = risk_manager_.register_symbol(
            config.symbol,
            config.max_position,
            config.max_notional
        );
    }

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
        auto quote = market_maker_.generate_quotes(last_mid_, pos_tracker_.position());

        if (quote.has_bid || quote.has_ask) {
            ++quotes_generated_;
        }

        return quote;
    }

    // Process a fill (our order was executed)
    void on_fill(Side side, Quantity qty, Price price) {
        // Update position tracker
        pos_tracker_.on_fill(side, qty, price);

        // Update risk manager position tracking (hot path)
        risk_manager_.on_fill(symbol_index_, side, qty, price);

        // Use fill price as mark if no market data yet
        Price mark = (last_mid_ > 0) ? last_mid_ : price;

        // Update risk manager with current P&L
        risk_manager_.update_pnl(pos_tracker_.total_pnl(mark));
    }

    // Check if order passes risk checks (call before sending)
    bool check_order(Side side, Quantity qty, Price price) const {
        return risk_manager_.check_order(symbol_index_, side, qty, price);
    }

    // Accessors
    int64_t position() const { return pos_tracker_.position(); }
    int64_t realized_pnl() const { return pos_tracker_.realized_pnl(); }
    int64_t unrealized_pnl() const { return pos_tracker_.unrealized_pnl(last_mid_); }
    int64_t total_pnl() const { return pos_tracker_.total_pnl(last_mid_); }
    bool is_halted() const { return risk_manager_.is_halted(); }
    uint64_t total_quotes_generated() const { return quotes_generated_; }

    // Reset for new simulation
    void reset() {
        pos_tracker_.reset();
        risk_manager_.reset_halt();
        quotes_generated_ = 0;
        last_mid_ = 0;
    }

    const SimulatorConfig& config() const { return config_; }

    // Access to risk manager for advanced queries
    const risk::EnhancedRiskManager& risk_manager() const { return risk_manager_; }

    // Get risk state snapshot
    risk::RiskState risk_state() const { return risk_manager_.build_state(); }

private:
    static strategy::MarketMakerConfig create_mm_config(const SimulatorConfig& cfg) {
        return strategy::MarketMakerConfig{
            cfg.spread_bps,
            cfg.quote_size,
            static_cast<int64_t>(cfg.max_position),
            cfg.skew_factor
        };
    }

    static risk::EnhancedRiskConfig create_risk_config(const SimulatorConfig& cfg) {
        risk::EnhancedRiskConfig risk_cfg;
        risk_cfg.initial_capital = cfg.initial_capital;
        risk_cfg.daily_loss_limit = cfg.daily_loss_limit;
        risk_cfg.max_drawdown_pct = cfg.max_drawdown_pct;
        risk_cfg.max_order_size = cfg.max_order_size;
        risk_cfg.max_total_notional = cfg.max_notional;
        risk_cfg.max_total_position = cfg.max_position;
        return risk_cfg;
    }

    SimulatorConfig config_;
    strategy::MarketMaker market_maker_;
    strategy::PositionTracker pos_tracker_;
    risk::EnhancedRiskManager risk_manager_;
    risk::SymbolIndex symbol_index_;
    uint64_t quotes_generated_;
    Price last_mid_;
};

}  // namespace hft
