#pragma once

#include "istrategy.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace hft {
namespace strategy {

/**
 * StrategySelector - Manages strategy registration and selection
 *
 * Features:
 * - Register multiple strategies
 * - Select by name (config-based)
 * - Select by regime (adaptive)
 * - Composite mode (multiple strategies vote)
 *
 * Usage:
 *   StrategySelector selector;
 *   selector.register_strategy(std::make_unique<TechnicalIndicatorsStrategy>());
 *   selector.register_strategy(std::make_unique<MarketMakerStrategy>());
 *
 *   // Config-based selection
 *   auto* strategy = selector.select_by_name("MarketMaker");
 *
 *   // Regime-based selection
 *   auto* strategy = selector.select_for_regime(MarketRegime::Ranging);
 */
class StrategySelector {
public:
    using StrategyPtr = std::unique_ptr<IStrategy>;

    StrategySelector() = default;

    // =========================================================================
    // Registration
    // =========================================================================

    /// Register a strategy instance
    void register_strategy(StrategyPtr strategy) {
        if (strategy) {
            strategies_.push_back(std::move(strategy));
        }
    }

    /// Register strategy and set as default
    void register_default(StrategyPtr strategy) {
        if (strategy) {
            default_strategy_ = strategy.get();
            strategies_.push_back(std::move(strategy));
        }
    }

    /// Set default strategy by name (must already be registered)
    bool set_default(std::string_view name) {
        auto* s = select_by_name(name);
        if (s) {
            default_strategy_ = s;
            return true;
        }
        return false;
    }

    // =========================================================================
    // Selection Methods
    // =========================================================================

    /// Select strategy by exact name match
    IStrategy* select_by_name(std::string_view name) const {
        for (const auto& s : strategies_) {
            if (s->name() == name) {
                return s.get();
            }
        }
        return nullptr;
    }

    /// Select first suitable and ready strategy for given regime
    IStrategy* select_for_regime(MarketRegime regime) const {
        // First pass: find strategy that's suitable AND ready
        for (const auto& s : strategies_) {
            if (s->suitable_for_regime(regime) && s->ready()) {
                return s.get();
            }
        }

        // Second pass: find strategy that's just suitable (might need warmup)
        for (const auto& s : strategies_) {
            if (s->suitable_for_regime(regime)) {
                return s.get();
            }
        }

        // Fallback to default
        return default_strategy_;
    }

    /// Select with priority list (try each in order)
    IStrategy* select_priority(std::initializer_list<std::string_view> priority_names, MarketRegime regime) const {
        for (auto name : priority_names) {
            auto* s = select_by_name(name);
            if (s && s->suitable_for_regime(regime) && s->ready()) {
                return s;
            }
        }
        return select_for_regime(regime);
    }

    /// Get default strategy
    IStrategy* get_default() const { return default_strategy_; }

    // =========================================================================
    // Composite/Voting Mode
    // =========================================================================

    /// Get signals from all suitable strategies and combine them
    Signal composite_signal(Symbol symbol, const MarketSnapshot& market, const StrategyPosition& position,
                            MarketRegime regime) const {
        std::vector<Signal> signals;
        signals.reserve(strategies_.size());

        // Collect signals from all suitable strategies
        for (const auto& s : strategies_) {
            if (s->suitable_for_regime(regime) && s->ready()) {
                Signal sig = s->generate(symbol, market, position, regime);
                if (sig.is_actionable()) {
                    signals.push_back(sig);
                }
            }
        }

        if (signals.empty()) {
            return Signal::none();
        }

        // Simple voting: count buy vs sell
        int buy_votes = 0, sell_votes = 0;
        int buy_strength_sum = 0, sell_strength_sum = 0;
        double total_qty = 0;

        for (const auto& sig : signals) {
            int strength = static_cast<int>(sig.strength);
            if (sig.is_buy()) {
                buy_votes++;
                buy_strength_sum += strength;
                total_qty += sig.suggested_qty;
            } else if (sig.is_sell()) {
                sell_votes++;
                sell_strength_sum += strength;
                total_qty += sig.suggested_qty;
            }
        }

        // Need majority
        if (buy_votes > sell_votes && buy_votes >= 2) {
            Signal result;
            result.type = SignalType::Buy;
            result.strength = to_signal_strength(buy_strength_sum / buy_votes);
            result.suggested_qty = total_qty / buy_votes;
            result.order_pref = OrderPreference::Either;
            result.reason = "Composite: multiple strategies agree on BUY";
            return result;
        } else if (sell_votes > buy_votes && sell_votes >= 2) {
            Signal result;
            result.type = SignalType::Sell;
            result.strength = to_signal_strength(sell_strength_sum / sell_votes);
            result.suggested_qty = total_qty / sell_votes;
            result.order_pref = OrderPreference::Either;
            result.reason = "Composite: multiple strategies agree on SELL";
            return result;
        }

        return Signal::none(); // No consensus
    }

    // =========================================================================
    // Bulk Operations
    // =========================================================================

    /// Update all strategies with new tick
    void on_tick_all(const MarketSnapshot& market) {
        for (auto& s : strategies_) {
            s->on_tick(market);
        }
    }

    /// Reset all strategies
    void reset_all() {
        for (auto& s : strategies_) {
            s->reset();
        }
    }

    // =========================================================================
    // Iteration
    // =========================================================================

    /// Iterate over all strategies
    template <typename Func>
    void for_each(Func&& fn) const {
        for (const auto& s : strategies_) {
            fn(*s);
        }
    }

    /// Get list of strategy names
    std::vector<std::string_view> strategy_names() const {
        std::vector<std::string_view> names;
        names.reserve(strategies_.size());
        for (const auto& s : strategies_) {
            names.push_back(s->name());
        }
        return names;
    }

    /// Number of registered strategies
    size_t count() const { return strategies_.size(); }

    /// Check if empty
    bool empty() const { return strategies_.empty(); }

private:
    std::vector<StrategyPtr> strategies_;
    IStrategy* default_strategy_ = nullptr;
};

// =============================================================================
// Regime-to-Strategy Mapping Helper
// =============================================================================

/**
 * RegimeStrategyMapping - Configure which strategies to use for each regime
 */
struct RegimeStrategyMapping {
    std::string_view ranging_strategy = "TechnicalIndicators";
    std::string_view trending_up_strategy = "Momentum";
    std::string_view trending_down_strategy = "TechnicalIndicators";
    std::string_view high_volatility_strategy = ""; // Empty = don't trade
    std::string_view low_volatility_strategy = "MarketMaker";
    std::string_view unknown_strategy = "TechnicalIndicators";

    std::string_view get_strategy_for_regime(MarketRegime regime) const {
        switch (regime) {
        case MarketRegime::Ranging:
            return ranging_strategy;
        case MarketRegime::TrendingUp:
            return trending_up_strategy;
        case MarketRegime::TrendingDown:
            return trending_down_strategy;
        case MarketRegime::HighVolatility:
            return high_volatility_strategy;
        case MarketRegime::LowVolatility:
            return low_volatility_strategy;
        default:
            return unknown_strategy;
        }
    }
};

/**
 * MappedStrategySelector - Selects strategy based on regime mapping
 */
class MappedStrategySelector {
public:
    MappedStrategySelector(StrategySelector& selector, const RegimeStrategyMapping& mapping = {})
        : selector_(selector), mapping_(mapping) {}

    IStrategy* select(MarketRegime regime) const {
        auto strategy_name = mapping_.get_strategy_for_regime(regime);
        if (strategy_name.empty()) {
            return nullptr; // Don't trade in this regime
        }
        auto* s = selector_.select_by_name(strategy_name);
        if (s && s->ready()) {
            return s;
        }
        // Fallback to regime-based selection
        return selector_.select_for_regime(regime);
    }

    void set_mapping(const RegimeStrategyMapping& mapping) { mapping_ = mapping; }

    const RegimeStrategyMapping& mapping() const { return mapping_; }

private:
    StrategySelector& selector_;
    RegimeStrategyMapping mapping_;
};

} // namespace strategy
} // namespace hft
