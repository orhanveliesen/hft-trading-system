#pragma once

/**
 * ConfigStrategy - Runtime Config-Driven Trading Strategy
 *
 * Reads ALL parameters from SymbolTuningConfig (per-symbol).
 * Only checks SharedConfig for emergency stop (trading_enabled) and tuner state.
 *
 * Key features:
 * 1. Per-symbol configuration: Each symbol has independent thresholds
 * 2. Per-symbol state: Streaks, mode, performance tracked per-symbol
 * 3. Runtime tunable: Tuner can modify SymbolTuningConfig at runtime
 * 4. IStrategy compatible: Can be used with existing trading infrastructure
 *
 * Mode transitions based on SymbolTuningConfig thresholds:
 * - AGGRESSIVE (0): Good performance, take more signals
 * - NORMAL (1): Standard operation
 * - CAUTIOUS (2): After some losses, require stronger signals
 * - DEFENSIVE (3): Significant losses, reduce exposure
 * - EXIT_ONLY (4): No new positions, only close existing
 */

#include "../config/defaults.hpp"
#include "../ipc/shared_config.hpp"
#include "../ipc/symbol_config.hpp"
#include "../risk/enhanced_risk_manager.hpp"
#include "istrategy.hpp"
#include "rolling_sharpe.hpp"
#include "technical_indicators.hpp"

#include <algorithm>
#include <cstring>

namespace hft {
namespace strategy {

// Mode enum (matches SymbolTuningConfig::current_mode)
enum class ConfigMode : int8_t { AGGRESSIVE = 0, NORMAL = 1, CAUTIOUS = 2, DEFENSIVE = 3, EXIT_ONLY = 4 };

inline const char* config_mode_str(ConfigMode mode) {
    switch (mode) {
    case ConfigMode::AGGRESSIVE:
        return "AGGR";
    case ConfigMode::NORMAL:
        return "NORM";
    case ConfigMode::CAUTIOUS:
        return "CAUT";
    case ConfigMode::DEFENSIVE:
        return "DEF";
    case ConfigMode::EXIT_ONLY:
        return "EXIT";
    default:
        return "?";
    }
}

/**
 * ConfigStrategy implements IStrategy using runtime config.
 *
 * Constructor parameters:
 * - global_config: SharedConfig* for trading_enabled check only
 * - symbol_configs: SharedSymbolConfigs* for ALL trading parameters
 * - symbol_name: Which symbol this instance trades
 */
class ConfigStrategy : public IStrategy {
public:
    // Minimum ticks before strategy is ready
    static constexpr int MIN_TICKS_TO_READY = 20;

    // Sharpe window size
    static constexpr size_t SHARPE_WINDOW = 100;

    ConfigStrategy(const ipc::SharedConfig* global_config, ipc::SharedSymbolConfigs* symbol_configs,
                   const char* symbol_name)
        : global_(global_config), symbol_configs_(symbol_configs), tick_count_(0), cumulative_pnl_(0), peak_pnl_(0) {
        std::strncpy(symbol_, symbol_name, sizeof(symbol_) - 1);
        symbol_[sizeof(symbol_) - 1] = '\0';

        // Ensure symbol config exists
        if (symbol_configs_) {
            symbol_configs_->get_or_create(symbol_);
        }
    }

    // =========================================================================
    // IStrategy Interface
    // =========================================================================

    Signal generate(Symbol symbol, const MarketSnapshot& market, const StrategyPosition& position,
                    MarketRegime regime) override {
        (void)symbol; // We use symbol_ instead

        // 1. Emergency stop check (global)
        if (global_ && !global_->is_trading_enabled()) {
            return Signal::none();
        }

        // 2. Get symbol config
        ipc::SymbolTuningConfig* sym = nullptr;
        if (symbol_configs_) {
            sym = symbol_configs_->get_or_create(symbol_);
        }
        if (!sym) {
            return Signal::none();
        }

        // 3. Check if symbol is enabled
        if (!sym->is_enabled()) {
            return Signal::none();
        }

        // 4. Check if strategy is ready
        if (!ready()) {
            return Signal::none();
        }

        // 5. Update mode based on current state
        update_mode(sym);

        ConfigMode mode = static_cast<ConfigMode>(sym->current_mode);

        // 6. EXIT_ONLY mode: no new positions
        if (mode == ConfigMode::EXIT_ONLY && !position.has_position()) {
            return Signal::none();
        }

        // 7. Generate signal based on mode and market conditions
        return generate_signal(sym, market, position, regime, mode);
    }

    std::string_view name() const override { return "Config"; }

    OrderPreference default_order_preference() const override { return OrderPreference::Either; }

    bool suitable_for_regime(MarketRegime /*regime*/) const override {
        // ConfigStrategy is suitable for all regimes (config-driven)
        return true;
    }

    void on_tick(const MarketSnapshot& market) override {
        ++tick_count_;
        if (market.valid()) {
            double mid_price = market.mid_usd(risk::PRICE_SCALE);
            indicators_.update(mid_price);
        }
    }

    void reset() override {
        tick_count_ = 0;
        cumulative_pnl_ = 0;
        peak_pnl_ = 0;
        sharpe_.reset();
        indicators_.reset();
    }

    bool ready() const override { return tick_count_ >= MIN_TICKS_TO_READY && indicators_.ready(); }

    // =========================================================================
    // Performance Tracking
    // =========================================================================

    /**
     * Record trade result for performance tracking
     * Called by trader when a trade closes.
     *
     * @param pnl_pct P&L as percentage (e.g., 1.5 for 1.5% profit)
     * @param was_win True if trade was profitable
     */
    void record_trade_result(double pnl_pct, bool was_win) {
        // Update Sharpe
        sharpe_.add_return(pnl_pct / 100.0); // Convert % to decimal

        // Update cumulative P&L
        cumulative_pnl_ += pnl_pct;
        if (cumulative_pnl_ > peak_pnl_) {
            peak_pnl_ = cumulative_pnl_;
        }

        // Update symbol config state
        if (symbol_configs_) {
            auto* sym = symbol_configs_->get_or_create(symbol_);
            if (sym) {
                sym->record_trade(was_win, pnl_pct);
            }
        }
    }

    // =========================================================================
    // Accessors
    // =========================================================================

    double sharpe_ratio() const { return sharpe_.sharpe_ratio(); }
    double cumulative_pnl() const { return cumulative_pnl_; }
    double current_drawdown() const { return peak_pnl_ > 0 ? (peak_pnl_ - cumulative_pnl_) / peak_pnl_ : 0; }

    // =========================================================================
    // Accumulation Factor Calculation (Tuner-Controlled)
    // =========================================================================

    /**
     * Calculate accumulation factor based on tuner's parameters.
     * This determines how aggressively we add to existing positions.
     *
     * @param sym Symbol tuning config with accumulation parameters
     * @param position Current position state
     * @param regime Current market regime
     * @param raw_signal_strength Signal strength (0-1)
     * @return Accumulation factor (0.1 to accum_max)
     */
    double calculate_accumulation_factor(const ipc::SymbolTuningConfig* sym, const StrategyPosition& position,
                                         MarketRegime regime, double raw_signal_strength) const {
        if (!sym)
            return 0.3; // Default fallback

        // Base factor: inverse of position %
        double base = 1.0 - position.position_pct();

        // Get floor from tuner's config based on regime
        double floor;
        switch (regime) {
        case MarketRegime::TrendingUp:
        case MarketRegime::TrendingDown:
            floor = sym->accum_floor_trending();
            break;
        case MarketRegime::HighVolatility:
            floor = sym->accum_floor_highvol();
            break;
        default: // Ranging, Unknown
            floor = sym->accum_floor_ranging();
            break;
        }

        // Win streak boost (tuner-controlled rate)
        int wins = sym->consecutive_wins;
        double boost_per_win = sym->accum_boost_per_win();
        floor += wins * boost_per_win;

        // Loss streak penalty (tuner-controlled rate)
        int losses = sym->consecutive_losses;
        double penalty_per_loss = sym->accum_penalty_per_loss();
        floor -= losses * penalty_per_loss;

        // Strong signal boost
        static constexpr double STRONG_SIGNAL_THRESHOLD = 0.7;
        if (raw_signal_strength >= STRONG_SIGNAL_THRESHOLD) {
            floor += sym->accum_signal_boost();
        }

        // Clamp to [0.1, accum_max]
        static constexpr double ACCUM_MIN = 0.1;
        double max_factor = sym->accum_max();
        floor = std::clamp(floor, ACCUM_MIN, max_factor);

        return std::max(floor, base);
    }

private:
    // Config sources (not owned)
    const ipc::SharedConfig* global_;
    ipc::SharedSymbolConfigs* symbol_configs_;
    char symbol_[16];

    // Internal state
    int tick_count_;
    double cumulative_pnl_;
    double peak_pnl_;

    // Performance tracking
    RollingSharpe<SHARPE_WINDOW> sharpe_;

    // Technical indicators for multi-factor signal generation
    TechnicalIndicators indicators_;

    // =========================================================================
    // Mode Management
    // =========================================================================

    void update_mode(ipc::SymbolTuningConfig* sym) {
        if (!sym)
            return;

        int losses = sym->consecutive_losses;
        int wins = sym->consecutive_wins;

        // Priority: Exit conditions first

        // 1. Check EXIT_ONLY threshold
        if (losses >= sym->losses_to_exit_only) {
            sym->current_mode = static_cast<int8_t>(ConfigMode::EXIT_ONLY);
            return;
        }

        // 2. Check drawdown for EXIT_ONLY
        double drawdown = current_drawdown();
        double dd_exit = sym->drawdown_to_exit();
        if (dd_exit > 0 && drawdown >= dd_exit) {
            sym->current_mode = static_cast<int8_t>(ConfigMode::EXIT_ONLY);
            return;
        }

        // 3. Check PAUSE/DEFENSIVE threshold (loss streak)
        if (losses >= sym->losses_to_pause) {
            sym->current_mode = static_cast<int8_t>(ConfigMode::DEFENSIVE);
            return;
        }

        // 4. Check DEFENSIVE (loss streak or drawdown)
        if (losses >= sym->losses_to_defensive) {
            sym->current_mode = static_cast<int8_t>(ConfigMode::DEFENSIVE);
            return;
        }

        double dd_def = sym->drawdown_to_defensive();
        if (dd_def > 0 && drawdown >= dd_def) {
            sym->current_mode = static_cast<int8_t>(ConfigMode::DEFENSIVE);
            return;
        }

        // 5. Check Sharpe for DEFENSIVE
        double sharpe = sharpe_.sharpe_ratio();
        if (sharpe_.count() >= 20 && sharpe < sym->sharpe_defensive()) {
            sym->current_mode = static_cast<int8_t>(ConfigMode::DEFENSIVE);
            return;
        }

        // 6. Check CAUTIOUS (loss streak)
        if (losses >= sym->losses_to_cautious) {
            sym->current_mode = static_cast<int8_t>(ConfigMode::CAUTIOUS);
            return;
        }

        // 7. Check Sharpe for CAUTIOUS
        if (sharpe_.count() >= 20 && sharpe < sym->sharpe_cautious()) {
            sym->current_mode = static_cast<int8_t>(ConfigMode::CAUTIOUS);
            return;
        }

        // 8. Check win rate for CAUTIOUS
        if (sym->total_trades >= 20) {
            double win_rate = sym->win_rate();
            if (win_rate < sym->win_rate_cautious_threshold()) {
                sym->current_mode = static_cast<int8_t>(ConfigMode::CAUTIOUS);
                return;
            }
        }

        // 9. Check AGGRESSIVE conditions
        if (wins >= sym->wins_to_aggressive) {
            // Check Sharpe also supports AGGRESSIVE
            if (sharpe_.count() < 20 || sharpe >= sym->sharpe_aggressive()) {
                sym->current_mode = static_cast<int8_t>(ConfigMode::AGGRESSIVE);
                return;
            }
        }

        // 10. Check win rate for AGGRESSIVE
        if (sym->total_trades >= 20) {
            double win_rate = sym->win_rate();
            if (win_rate > sym->win_rate_aggressive_threshold() &&
                (sharpe_.count() < 20 || sharpe >= sym->sharpe_aggressive())) {
                sym->current_mode = static_cast<int8_t>(ConfigMode::AGGRESSIVE);
                return;
            }
        }

        // Default: NORMAL
        sym->current_mode = static_cast<int8_t>(ConfigMode::NORMAL);
    }

    // =========================================================================
    // Signal Generation
    // =========================================================================

    Signal generate_signal(ipc::SymbolTuningConfig* sym, const MarketSnapshot& market, const StrategyPosition& position,
                           MarketRegime regime, ConfigMode mode) {
        if (!market.valid()) {
            return Signal::none();
        }

        // Get signal threshold based on mode
        double threshold = get_signal_threshold(sym, mode);

        // Simple signal score calculation based on market conditions
        double score = calculate_signal_score(sym, market, position, regime);

        // Check if score meets threshold
        if (std::abs(score) < threshold) {
            return Signal::none();
        }

        // Check minimum confidence
        double confidence = std::abs(score);
        if (confidence < sym->min_confidence()) {
            return Signal::none();
        }

        // Get current price for position sizing (convert from scaled Price to USD)
        double current_price = market.mid_usd(risk::PRICE_SCALE);
        if (current_price <= 0) {
            return Signal::none();
        }

        // Calculate position size
        double qty = calculate_position_size(sym, position, mode, confidence, current_price);
        if (qty <= 0) {
            return Signal::none();
        }

        // Generate signal
        SignalStrength strength = confidence_to_strength(confidence);

        if (score > 0 && position.can_buy()) {
            // Buy signal
            if (mode == ConfigMode::EXIT_ONLY) {
                return Signal::none(); // No new buys in EXIT_ONLY
            }
            return Signal::buy(strength, qty, "Config:BUY");
        } else if (score < 0 && position.can_sell()) {
            // Sell signal
            return Signal::sell(strength, qty, "Config:SELL");
        }

        return Signal::none();
    }

    double get_signal_threshold(ipc::SymbolTuningConfig* sym, ConfigMode mode) const {
        switch (mode) {
        case ConfigMode::AGGRESSIVE:
            return sym->signal_threshold_aggressive();
        case ConfigMode::NORMAL:
            return sym->signal_threshold_normal();
        case ConfigMode::CAUTIOUS:
        case ConfigMode::DEFENSIVE:
        case ConfigMode::EXIT_ONLY:
            return sym->signal_threshold_cautious();
        default:
            return sym->signal_threshold_normal();
        }
    }

    /**
     * Calculate signal score using multi-factor technical analysis.
     *
     * Signal Components (weights):
     * 1. EMA Trend (0.4): Fast/slow EMA crossover and trend direction
     * 2. RSI (0.3): Overbought/oversold conditions
     * 3. Bollinger Bands (0.2): Price position relative to bands
     * 4. Order Book Imbalance (0.1): Reduced from sole input
     *
     * @return Score in range [-1.0, 1.0], positive = buy, negative = sell
     */
    double calculate_signal_score(ipc::SymbolTuningConfig* sym, const MarketSnapshot& market,
                                  const StrategyPosition& position, MarketRegime regime) const {
        // Need indicators to be warmed up
        if (!indicators_.ready()) {
            return 0.0;
        }

        double score = 0.0;

        // Signal component weights
        static constexpr double EMA_CROSSOVER_WEIGHT = 0.6;
        static constexpr double EMA_TREND_WEIGHT = 0.3;
        static constexpr double RSI_EXTREME_WEIGHT = 0.4;
        static constexpr double RSI_MILD_WEIGHT = 0.2;
        static constexpr double BB_OUTSIDE_WEIGHT = 0.3;
        static constexpr double BB_NEAR_WEIGHT = 0.15;
        static constexpr double OB_IMBALANCE_WEIGHT = 0.2;

        // 1. EMA Trend Component (weight: 0.4)
        if (indicators_.ema_crossed_up()) {
            score += EMA_CROSSOVER_WEIGHT; // Strong bullish signal
        } else if (indicators_.ema_bullish()) {
            score += EMA_TREND_WEIGHT; // Moderate bullish
        } else if (indicators_.ema_crossed_down()) {
            score -= EMA_CROSSOVER_WEIGHT; // Strong bearish signal
        } else if (indicators_.ema_bearish()) {
            score -= EMA_TREND_WEIGHT; // Moderate bearish
        }

        // 2. RSI Component (weight: 0.3)
        double rsi = indicators_.rsi();
        static constexpr double RSI_OVERSOLD = 30.0;
        static constexpr double RSI_MILD_OVERSOLD = 40.0;
        static constexpr double RSI_OVERBOUGHT = 70.0;
        static constexpr double RSI_MILD_OVERBOUGHT = 60.0;

        if (rsi < RSI_OVERSOLD) {
            score += RSI_EXTREME_WEIGHT; // Oversold = buy
        } else if (rsi < RSI_MILD_OVERSOLD) {
            score += RSI_MILD_WEIGHT; // Mildly oversold
        } else if (rsi > RSI_OVERBOUGHT) {
            score -= RSI_EXTREME_WEIGHT; // Overbought = sell
        } else if (rsi > RSI_MILD_OVERBOUGHT) {
            score -= RSI_MILD_WEIGHT; // Mildly overbought
        }

        // 3. Bollinger Band Component (weight: 0.2)
        if (indicators_.below_lower_band()) {
            score += BB_OUTSIDE_WEIGHT; // Below lower = buy signal
        } else if (indicators_.near_lower_band()) {
            score += BB_NEAR_WEIGHT;
        } else if (indicators_.above_upper_band()) {
            score -= BB_OUTSIDE_WEIGHT; // Above upper = sell signal
        } else if (indicators_.near_upper_band()) {
            score -= BB_NEAR_WEIGHT;
        }

        // 4. Order Book Imbalance (weight: 0.1, reduced from sole input)
        double total_size = static_cast<double>(market.bid_size + market.ask_size);
        if (total_size > 0) {
            double imbalance =
                (static_cast<double>(market.bid_size) - static_cast<double>(market.ask_size)) / total_size;
            score += imbalance * OB_IMBALANCE_WEIGHT;
        }

        // 5. Regime Adjustment
        switch (regime) {
        case MarketRegime::TrendingUp:
            if (score > 0)
                score *= 1.2; // Boost buys in uptrend
            else
                score *= 0.7; // Penalize sells
            break;
        case MarketRegime::TrendingDown:
            if (score < 0)
                score *= 1.2; // Boost sells in downtrend
            else
                score *= 0.7; // Penalize buys
            break;
        case MarketRegime::HighVolatility:
            score *= 0.5; // Reduce all signals in high volatility
            break;
        case MarketRegime::Ranging:
            // Mean reversion works better - no adjustment
            break;
        default:
            break;
        }

        // 6. Position Accumulation Factor (keep existing logic)
        if (position.has_position()) {
            double raw_signal = std::abs(score);
            double accum_factor = calculate_accumulation_factor(sym, position, regime, raw_signal);
            score *= accum_factor;
        }

        return std::clamp(score, -1.0, 1.0);
    }

public:
    // =========================================================================
    // Position Sizing (Public for testing)
    // =========================================================================

    /**
     * Calculate position size (quantity) based on cash, risk %, and price.
     *
     * Formula: qty = (cash_available * size_pct) / current_price
     *
     * @param sym Symbol config for position limits
     * @param position Current portfolio state
     * @param mode Trading mode (affects size multiplier)
     * @param confidence Signal confidence (0-1, scales position size)
     * @param current_price Current asset price in USD
     * @return Quantity to trade
     */
    double calculate_position_size(ipc::SymbolTuningConfig* sym, const StrategyPosition& position, ConfigMode mode,
                                   double confidence, double current_price) const {
        // Validate price
        if (current_price <= 0) {
            return 0.0;
        }

        // Base position from config
        double base_pct = sym->base_position_pct() / 100.0; // Convert from % to ratio
        double max_pct = sym->max_position_pct() / 100.0;
        double min_pct = sym->min_position_pct() / 100.0;

        // Scale by confidence
        double size_pct = base_pct * confidence;

        // Mode adjustment
        switch (mode) {
        case ConfigMode::AGGRESSIVE:
            size_pct *= 1.25;
            break;
        case ConfigMode::CAUTIOUS:
            size_pct *= 0.75;
            break;
        case ConfigMode::DEFENSIVE:
            size_pct *= 0.5;
            break;
        case ConfigMode::EXIT_ONLY:
            // Only allow selling existing position
            return position.quantity;
        default:
            break;
        }

        // Clamp to limits
        size_pct = std::clamp(size_pct, min_pct, max_pct);

        // FIX: Convert to quantity using target value and current price
        // target_value = cash_available * size_pct
        // qty = target_value / current_price
        double target_value = position.cash_available * size_pct;
        double qty = target_value / current_price;

        return std::max(0.0, qty);
    }

private:
    SignalStrength confidence_to_strength(double confidence) const {
        if (confidence >= 0.8)
            return SignalStrength::Strong;
        if (confidence >= 0.5)
            return SignalStrength::Medium;
        if (confidence >= 0.3)
            return SignalStrength::Weak;
        return SignalStrength::None;
    }
};

} // namespace strategy
} // namespace hft
