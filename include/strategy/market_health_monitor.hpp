#pragma once

#include <array>
#include <cstdint>

namespace hft {
namespace strategy {

/**
 * Market Health Monitor
 *
 * Detects market-wide crashes by tracking how many symbols are in spike state.
 * When crash threshold is exceeded, triggers emergency liquidation.
 *
 * Usage:
 *   MarketHealthMonitor monitor(num_symbols, crash_threshold, cooldown_ticks);
 *
 *   // On each price update:
 *   monitor.update_symbol(symbol_id, regime.is_spike());
 *
 *   // Check for crash:
 *   if (monitor.should_liquidate()) {
 *       // EMERGENCY: Sell all positions at market
 *   }
 *
 * Parameters:
 *   - crash_threshold: 0.5 = crash when 50% of symbols spike
 *   - cooldown_ticks: How many ticks to stay in cooldown after crash
 */
class MarketHealthMonitor {
public:
    static constexpr size_t MAX_SYMBOLS = 64;

    explicit MarketHealthMonitor(
        size_t num_symbols,
        double crash_threshold = 0.5,
        int cooldown_ticks = 60
    )
        : num_symbols_(num_symbols)
        , crash_threshold_(crash_threshold)
        , cooldown_ticks_(cooldown_ticks)
        , cooldown_remaining_(0)
        , spike_count_(0)
        , active_count_(0)
        , liquidation_triggered_(false)
    {
        symbol_is_spike_.fill(false);
        symbol_is_active_.fill(false);
    }

    /**
     * Update spike state for a symbol
     */
    void update_symbol(size_t symbol_id, bool is_spike) {
        if (symbol_id >= MAX_SYMBOLS) return;

        // Track if symbol was previously spiking
        bool was_spike = symbol_is_spike_[symbol_id];
        bool was_active = symbol_is_active_[symbol_id];

        // Update state
        symbol_is_spike_[symbol_id] = is_spike;
        symbol_is_active_[symbol_id] = true;

        // Update counts
        if (!was_active) {
            active_count_++;
        }

        if (is_spike && !was_spike) {
            spike_count_++;
        } else if (!is_spike && was_spike) {
            spike_count_--;
        }

        // Check for crash and start cooldown
        if (is_crash() && !liquidation_triggered_) {
            cooldown_remaining_ = cooldown_ticks_;
        }
    }

    /**
     * Tick cooldown (call once per update cycle)
     */
    void tick() {
        if (cooldown_remaining_ > 0) {
            cooldown_remaining_--;
            if (cooldown_remaining_ == 0) {
                // Cooldown expired, allow new liquidation
                liquidation_triggered_ = false;
            }
        }
    }

    /**
     * Check if market is in crash state
     * Crash = spike_ratio >= crash_threshold
     */
    bool is_crash() const {
        if (active_count_ == 0) return false;
        return spike_ratio() >= crash_threshold_;
    }

    /**
     * Check if in cooldown period after crash
     */
    bool in_cooldown() const {
        return cooldown_remaining_ > 0;
    }

    /**
     * Check if emergency liquidation should be triggered
     * Returns true ONCE per crash event
     */
    bool should_liquidate() {
        if (is_crash() && !liquidation_triggered_) {
            liquidation_triggered_ = true;
            cooldown_remaining_ = cooldown_ticks_;
            return true;
        }
        return false;
    }

    /**
     * Get current spike count
     */
    int spike_count() const { return spike_count_; }

    /**
     * Get active symbol count (symbols that have been updated)
     */
    int active_count() const { return active_count_; }

    /**
     * Get spike ratio (spike_count / active_count)
     */
    double spike_ratio() const {
        if (active_count_ == 0) return 0.0;
        return static_cast<double>(spike_count_) / active_count_;
    }

    /**
     * Get cooldown remaining ticks
     */
    int cooldown_remaining() const { return cooldown_remaining_; }

    /**
     * Reset all state
     */
    void reset() {
        symbol_is_spike_.fill(false);
        symbol_is_active_.fill(false);
        spike_count_ = 0;
        active_count_ = 0;
        cooldown_remaining_ = 0;
        liquidation_triggered_ = false;
    }

private:
    size_t num_symbols_;
    double crash_threshold_;
    int cooldown_ticks_;
    int cooldown_remaining_;
    int spike_count_;
    int active_count_;
    bool liquidation_triggered_;

    std::array<bool, MAX_SYMBOLS> symbol_is_spike_;
    std::array<bool, MAX_SYMBOLS> symbol_is_active_;
};

}  // namespace strategy
}  // namespace hft
