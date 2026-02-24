#pragma once

#include "../strategy/fair_value.hpp"
#include "../strategy/momentum.hpp"
#include "../strategy/order_flow_imbalance.hpp"
#include "../strategy/simple_mean_reversion.hpp"
#include "kline_backtest.hpp"

namespace hft {
namespace backtest {

/**
 * Adapter: SimpleMeanReversion -> IStrategy
 *
 * Mevcut stratejileri backtest engine'de kullanılabilir hale getirir.
 */
class SimpleMRAdapter : public IStrategy {
public:
    explicit SimpleMRAdapter(const strategy::SimpleMRConfig& config = {}) : strategy_(config), position_(0) {}

    void on_start(double /*capital*/) override {
        strategy_.reset();
        position_ = 0;
    }

    Signal on_kline(const exchange::Kline& kline, const BacktestPosition& pos) override {
        // Kline'dan bid/ask simüle et
        Price bid = kline.close;
        Price ask = kline.close + 100; // 1 cent spread

        // Mevcut pozisyonu güncelle
        position_ = static_cast<int64_t>(pos.quantity);
        if (pos.is_short())
            position_ = -position_;

        // Strateji sinyali al
        auto sig = strategy_(bid, ask, position_);

        // Dönüştür
        switch (sig) {
        case strategy::Signal::Buy:
            return Signal::Buy;
        case strategy::Signal::Sell:
            return Signal::Sell;
        default:
            return Signal::None;
        }
    }

private:
    strategy::SimpleMeanReversion strategy_;
    int64_t position_;
};

/**
 * Adapter: MomentumStrategy -> IStrategy
 */
class MomentumAdapter : public IStrategy {
public:
    explicit MomentumAdapter(const strategy::MomentumConfig& config = {}) : strategy_(config), position_(0) {}

    void on_start(double /*capital*/) override {
        strategy_.reset();
        position_ = 0;
    }

    Signal on_kline(const exchange::Kline& kline, const BacktestPosition& pos) override {
        Price bid = kline.close;
        Price ask = kline.close + 100;

        position_ = static_cast<int64_t>(pos.quantity);
        if (pos.is_short())
            position_ = -position_;

        auto sig = strategy_(bid, ask, position_);

        switch (sig) {
        case strategy::MomentumSignal::Buy:
            return Signal::Buy;
        case strategy::MomentumSignal::Sell:
            return Signal::Sell;
        default:
            return Signal::None;
        }
    }

    int64_t current_momentum() const { return strategy_.current_momentum_bps(); }

private:
    strategy::MomentumStrategy strategy_;
    int64_t position_;
};

/**
 * Generic adapter for any strategy that has:
 *   - Signal operator()(Price bid, Price ask, int64_t position)
 *   - void reset()
 *
 * Template parameter S: Strategy class
 * Template parameter Sig: Signal enum type (default: strategy::Signal)
 */
template <typename S, typename Sig = strategy::Signal>
class GenericStrategyAdapter : public IStrategy {
public:
    template <typename... Args>
    explicit GenericStrategyAdapter(Args&&... args) : strategy_(std::forward<Args>(args)...), position_(0) {}

    void on_start(double /*capital*/) override {
        strategy_.reset();
        position_ = 0;
    }

    Signal on_kline(const exchange::Kline& kline, const BacktestPosition& pos) override {
        Price bid = kline.close;
        Price ask = kline.close + 100;

        position_ = static_cast<int64_t>(pos.quantity);
        if (pos.is_short())
            position_ = -position_;

        auto sig = strategy_(bid, ask, position_);

        // Convert strategy-specific signal to backtest Signal
        if (static_cast<int>(sig) == 1)
            return Signal::Buy; // Buy
        if (static_cast<int>(sig) == 2)
            return Signal::Sell; // Sell
        return Signal::None;     // Hold
    }

    S& strategy() { return strategy_; }
    const S& strategy() const { return strategy_; }

private:
    S strategy_;
    int64_t position_;
};

// Convenience typedefs
using SimpleMRBacktest = GenericStrategyAdapter<strategy::SimpleMeanReversion, strategy::Signal>;
using MomentumBacktest = GenericStrategyAdapter<strategy::MomentumStrategy, strategy::MomentumSignal>;

} // namespace backtest
} // namespace hft
