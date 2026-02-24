#pragma once

/**
 * TradeRecorder - Single Source of Truth for P&L Tracking
 *
 * This class encapsulates ALL trade state updates to ensure:
 * 1. No forgotten updates - cash, P&L, commission all updated together
 * 2. Testable - one class to test, not scattered logic
 * 3. Consistent - same accounting logic everywhere
 *
 * Key Invariant (MUST ALWAYS HOLD):
 *   equity_pnl == realized_pnl + unrealized_pnl - total_commission
 *
 * Usage:
 *   TradeRecorder recorder;
 *   recorder.init(100000.0);  // $100k initial
 *
 *   // Record trades
 *   recorder.record_buy({.symbol=0, .price=100, .quantity=1, .commission=0.1});
 *   recorder.record_sell({.symbol=0, .price=110, .quantity=1, .commission=0.11});
 *
 *   // Check P&L
 *   double pnl = recorder.realized_pnl();  // +10
 */

#include "../ipc/shared_ledger.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

// Forward declaration for optional IPC sync
namespace hft {
namespace ipc {
struct SharedPortfolioState;
}
} // namespace hft
namespace hft {
namespace ipc {
struct SharedLedger;
struct SharedLedgerEntry;
} // namespace ipc
} // namespace hft

namespace hft {
namespace trading {

// Maximum symbols supported
static constexpr size_t MAX_RECORDER_SYMBOLS = 64;

// Ledger configuration
static constexpr size_t MAX_LEDGER_ENTRIES = 10000; // ~1MB circular buffer

// Exit reason for explicit exits
enum class ExitReason {
    TARGET,    // Profit target hit
    STOP,      // Stop loss hit
    PULLBACK,  // Trend pullback
    EMERGENCY, // Market crash
    SIGNAL     // Strategy signal
};

// Input for trade operations
struct TradeInput {
    uint32_t symbol = 0;    // Symbol index (0 = BTCUSDT, etc.)
    double price = 0;       // Execution price
    double quantity = 0;    // Quantity traded
    double commission = 0;  // Commission paid
    double spread_cost = 0; // Spread cost (informational)
    char ticker[16] = {};   // Symbol name for IPC updates
};

// Internal position tracking
struct RecorderPosition {
    double quantity = 0;     // Current quantity held
    double avg_price = 0;    // Average entry price
    double last_price = 0;   // Last market price (for unrealized P&L)
    double realized_pnl = 0; // Per-symbol realized P&L
    bool active = false;     // Is position active?

    void clear() {
        quantity = 0;
        avg_price = 0;
        last_price = 0;
        realized_pnl = 0;
        active = false;
    }

    double unrealized_pnl() const {
        if (quantity <= 0 || last_price <= 0)
            return 0;
        return quantity * (last_price - avg_price);
    }

    double market_value() const { return quantity * last_price; }
};

/**
 * LedgerEntry - Single transaction record for audit trail
 * ~160 bytes per entry, includes calculation breakdown for debugging
 */
struct LedgerEntry {
    uint64_t timestamp_ns; // Nanosecond timestamp
    uint32_t sequence;     // Monotonic sequence number
    uint32_t symbol;       // Symbol index
    char ticker[12];       // Symbol name (truncated)

    // Transaction details
    double price;      // Execution price
    double quantity;   // Quantity traded
    double commission; // Commission paid

    // Cash flow
    double cash_before;   // Cash before transaction
    double cash_after;    // Cash after transaction
    double cash_expected; // What cash_after SHOULD be (for verification)

    // Calculation breakdown (for debugging)
    double trade_value;          // price × quantity
    double expected_cash_change; // BUY: -(trade_value + commission)
                                 // SELL: +(trade_value - commission)

    // P&L (for sells)
    double realized_pnl; // Realized P&L (0 for buys)
    double avg_entry;    // Avg entry price at time of trade

    // P&L breakdown (for debugging)
    double pnl_per_unit; // sell_price - avg_entry (0 for buys)
    double expected_pnl; // pnl_per_unit × quantity (0 for buys)

    // Position state after
    double position_qty; // Position quantity after
    double position_avg; // Position avg price after

    // Flags
    uint8_t is_buy;      // 1=buy, 0=sell
    uint8_t is_exit;     // 1=explicit exit, 0=regular trade
    uint8_t exit_reason; // ExitReason enum value
    uint8_t balance_ok;  // 1=cash_after matches expected, 0=MISMATCH!
    uint8_t pnl_ok;      // 1=realized_pnl matches expected, 0=MISMATCH!
    uint8_t padding[3];  // Alignment

    // Running totals for verification
    double running_realized_pnl; // Cumulative realized P&L
    double running_commission;   // Cumulative commission

    // Check if this entry has a balance mismatch
    bool has_mismatch() const { return balance_ok == 0 || pnl_ok == 0; }

    // Calculate the cash discrepancy (0 if balanced)
    double cash_discrepancy() const { return cash_after - cash_expected; }

    // Calculate the P&L discrepancy (0 if balanced)
    double pnl_discrepancy() const { return realized_pnl - expected_pnl; }
};

// Sync callback type - called after each trade with updated state
// Parameters: (cash, realized_pnl, unrealized_pnl, commission, volume, fills, wins, losses, targets, stops)
using SyncCallback = void (*)(double, double, double, double, double, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

// Trade event callback - called after each trade with details
struct TradeEventInfo {
    uint32_t symbol;
    const char* ticker;
    double price;
    double quantity;
    double realized_pnl; // For sells only
    double commission;
    bool is_buy;
    ExitReason exit_reason; // Only valid for exits
    bool is_exit;
};
using TradeEventCallback = void (*)(const TradeEventInfo&);

/**
 * TradeRecorder - Single source of truth for trade accounting
 */
class TradeRecorder {
public:
    TradeRecorder() = default;

    // Set optional sync callback (for IPC updates)
    void set_sync_callback(SyncCallback cb) { sync_callback_ = cb; }

    // Set optional trade event callback (for event publishing)
    void set_trade_callback(TradeEventCallback cb) { trade_callback_ = cb; }

    // Connect to SharedLedger for IPC visibility (optional)
    void connect_shared_ledger(ipc::SharedLedger* ledger) { shared_ledger_ = ledger; }

    // Check if SharedLedger is connected
    bool has_shared_ledger() const { return shared_ledger_ != nullptr; }

    // Initialize with starting capital
    void init(double initial_cash) {
        cash_ = initial_cash;
        initial_cash_ = initial_cash;
        realized_pnl_ = 0;
        total_commission_ = 0;
        total_volume_ = 0;
        total_fills_ = 0;
        winning_trades_ = 0;
        losing_trades_ = 0;
        target_count_ = 0;
        stop_count_ = 0;
        total_gains_ = 0;
        total_losses_ = 0;

        // Reset ledger
        ledger_count_ = 0;
        ledger_start_ = 0;
        ledger_seq_ = 0;

        for (auto& pos : positions_) {
            pos.clear();
        }
    }

    // =========================================================================
    // CORE OPERATIONS - Single entry points for all trade recording
    // =========================================================================

    /**
     * Record a BUY trade
     * - Reduces cash by (price * qty + commission)
     * - Creates/adds to position
     * - Tracks commission and volume
     * - Creates ledger entry
     */
    void record_buy(const TradeInput& input) {
        if (input.quantity <= 0 || input.price <= 0)
            return;
        if (input.symbol >= MAX_RECORDER_SYMBOLS)
            return;

        double cash_before = cash_;
        double trade_value = input.price * input.quantity;

        // Update cash
        cash_ -= (trade_value + input.commission);

        // Update position (weighted average entry)
        auto& pos = positions_[input.symbol];
        double avg_entry_before = pos.avg_price;
        if (pos.quantity > 0) {
            // Average up/down
            double old_value = pos.quantity * pos.avg_price;
            double new_value = old_value + trade_value;
            pos.quantity += input.quantity;
            pos.avg_price = new_value / pos.quantity;
        } else {
            // New position
            pos.quantity = input.quantity;
            pos.avg_price = input.price;
        }
        pos.last_price = input.price;
        pos.active = true;

        // Track costs and volume
        total_commission_ += input.commission;
        total_volume_ += trade_value;
        total_fills_++;

        // Create ledger entry
        auto& entry = append_ledger_entry();
        entry.timestamp_ns = now_ns();
        entry.symbol = input.symbol;
        std::strncpy(entry.ticker, input.ticker, sizeof(entry.ticker) - 1);
        entry.price = input.price;
        entry.quantity = input.quantity;
        entry.commission = input.commission;
        entry.cash_before = cash_before;
        entry.cash_after = cash_;
        entry.cash_expected = cash_before - trade_value - input.commission;

        // Calculation breakdown
        entry.trade_value = trade_value;
        entry.expected_cash_change = -(trade_value + input.commission);

        // P&L (none for BUY)
        entry.realized_pnl = 0;
        entry.avg_entry = avg_entry_before;
        entry.pnl_per_unit = 0;
        entry.expected_pnl = 0;

        // Position state
        entry.position_qty = pos.quantity;
        entry.position_avg = pos.avg_price;

        // Flags
        entry.is_buy = 1;
        entry.is_exit = 0;
        entry.exit_reason = 0;
        entry.balance_ok = (std::abs(entry.cash_after - entry.cash_expected) < 0.001) ? 1 : 0;
        entry.pnl_ok = 1; // BUY has no P&L, always OK

        // Running totals
        entry.running_realized_pnl = realized_pnl_;
        entry.running_commission = total_commission_;

        // Sync to SharedLedger (if connected)
        sync_to_shared_ledger(entry);

        // Sync and notify
        sync_state();
        notify_trade(input, 0, true); // is_buy=true, no realized pnl
    }

    /**
     * Record a SELL trade
     * - Increases cash by (price * qty - commission)
     * - Calculates realized P&L from avg entry
     * - Tracks win/loss and gains/losses separately
     * - Creates ledger entry
     */
    void record_sell(const TradeInput& input) {
        if (input.quantity <= 0 || input.price <= 0)
            return;
        if (input.symbol >= MAX_RECORDER_SYMBOLS)
            return;

        auto& pos = positions_[input.symbol];
        if (pos.quantity <= 0)
            return; // Nothing to sell

        double cash_before = cash_;
        double avg_entry_before = pos.avg_price;

        // Clamp to available quantity
        double sell_qty = std::min(input.quantity, pos.quantity);
        double trade_value = input.price * sell_qty;

        // Calculate realized P&L BEFORE updating position
        double pnl = (input.price - pos.avg_price) * sell_qty;
        realized_pnl_ += pnl;
        pos.realized_pnl += pnl;

        // Track win/loss and gains/losses separately
        if (pnl >= 0) {
            winning_trades_++;
            total_gains_ += pnl;
        } else {
            losing_trades_++;
            total_losses_ += std::abs(pnl);
        }

        // Update cash
        cash_ += (trade_value - input.commission);

        // Update position
        pos.quantity -= sell_qty;
        double pos_qty_after = pos.quantity;
        double pos_avg_after = pos.avg_price;
        if (pos.quantity <= 0.0001) {
            pos.quantity = 0;
            pos.avg_price = 0;
            pos.active = false;
            pos_qty_after = 0;
            pos_avg_after = 0;
        }
        pos.last_price = input.price;

        // Track costs and volume
        total_commission_ += input.commission;
        total_volume_ += trade_value;
        total_fills_++;

        // Create ledger entry
        auto& entry = append_ledger_entry();
        entry.timestamp_ns = now_ns();
        entry.symbol = input.symbol;
        std::strncpy(entry.ticker, input.ticker, sizeof(entry.ticker) - 1);
        entry.price = input.price;
        entry.quantity = sell_qty;
        entry.commission = input.commission;
        entry.cash_before = cash_before;
        entry.cash_after = cash_;
        entry.cash_expected = cash_before + trade_value - input.commission;

        // Calculation breakdown
        entry.trade_value = trade_value;
        entry.expected_cash_change = trade_value - input.commission;

        // P&L breakdown
        double pnl_per_unit = input.price - avg_entry_before;
        double expected_pnl = pnl_per_unit * sell_qty;
        entry.realized_pnl = pnl; // + = gain, - = loss
        entry.avg_entry = avg_entry_before;
        entry.pnl_per_unit = pnl_per_unit;
        entry.expected_pnl = expected_pnl;

        // Position state
        entry.position_qty = pos_qty_after;
        entry.position_avg = pos_avg_after;

        // Flags
        entry.is_buy = 0;
        entry.is_exit = 0;
        entry.exit_reason = 0;
        entry.balance_ok = (std::abs(entry.cash_after - entry.cash_expected) < 0.001) ? 1 : 0;
        entry.pnl_ok = (std::abs(pnl - expected_pnl) < 0.001) ? 1 : 0;

        // Running totals
        entry.running_realized_pnl = realized_pnl_;
        entry.running_commission = total_commission_;

        // Sync to SharedLedger (if connected)
        sync_to_shared_ledger(entry);

        // Sync and notify
        sync_state();
        notify_trade(input, pnl, false); // is_buy=false, include realized pnl
    }

    /**
     * Record an explicit exit (target/stop/pullback/emergency)
     * - Same as sell, but also tracks exit type
     * - NOTE: win/loss already tracked by record_sell()
     */
    void record_exit(ExitReason reason, const TradeInput& input) {
        // Record the sell (handles cash, P&L, win/loss, sync)
        record_sell(input);

        // Track exit type for statistics
        switch (reason) {
        case ExitReason::TARGET:
        case ExitReason::PULLBACK:
            target_count_++;
            break;
        case ExitReason::STOP:
        case ExitReason::EMERGENCY:
            stop_count_++;
            break;
        case ExitReason::SIGNAL:
            // Already tracked by record_sell's win/loss logic
            break;
        }
    }

    /**
     * Update market price for unrealized P&L calculation
     */
    void update_market_price(uint32_t symbol, double price) {
        if (symbol >= MAX_RECORDER_SYMBOLS)
            return;
        positions_[symbol].last_price = price;
    }

    // =========================================================================
    // QUERY METHODS
    // =========================================================================

    double cash() const { return cash_; }
    double initial_cash() const { return initial_cash_; }

    double position_quantity(uint32_t symbol) const {
        if (symbol >= MAX_RECORDER_SYMBOLS)
            return 0;
        return positions_[symbol].quantity;
    }

    double position_avg_price(uint32_t symbol) const {
        if (symbol >= MAX_RECORDER_SYMBOLS)
            return 0;
        return positions_[symbol].avg_price;
    }

    double position_last_price(uint32_t symbol) const {
        if (symbol >= MAX_RECORDER_SYMBOLS)
            return 0;
        return positions_[symbol].last_price;
    }

    double realized_pnl() const { return realized_pnl_; }

    double unrealized_pnl() const {
        double total = 0;
        for (const auto& pos : positions_) {
            if (pos.active && pos.quantity > 0) {
                total += pos.unrealized_pnl();
            }
        }
        return total;
    }

    double total_commission() const { return total_commission_; }
    double total_volume() const { return total_volume_; }
    uint32_t total_fills() const { return total_fills_; }
    uint32_t winning_trades() const { return winning_trades_; }
    uint32_t losing_trades() const { return losing_trades_; }
    uint32_t target_count() const { return target_count_; }
    uint32_t stop_count() const { return stop_count_; }

    // Gains/Losses breakdown (O(1))
    double total_gains() const { return total_gains_; }
    double total_losses() const { return total_losses_; }

    /**
     * Total market value of all positions
     */
    double market_value() const {
        double total = 0;
        for (const auto& pos : positions_) {
            if (pos.active && pos.quantity > 0) {
                total += pos.market_value();
            }
        }
        return total;
    }

    /**
     * Total equity = cash + market value
     */
    double equity() const { return cash_ + market_value(); }

    /**
     * Total P&L from equity perspective
     */
    double equity_pnl() const { return equity() - initial_cash_; }

    /**
     * Verify P&L reconciliation
     * Returns the difference (should be ~0)
     */
    double pnl_difference() const {
        double equity_based = equity_pnl();
        double component_based = realized_pnl_ + unrealized_pnl() - total_commission_;
        return equity_based - component_based;
    }

    /**
     * Win rate percentage
     */
    double win_rate() const {
        uint32_t total = winning_trades_ + losing_trades_;
        if (total == 0)
            return 0;
        return 100.0 * winning_trades_ / total;
    }

    // =========================================================================
    // LEDGER - Transaction audit trail
    // =========================================================================

    /**
     * Get number of ledger entries
     */
    size_t ledger_count() const { return ledger_count_; }

    /**
     * Get ledger entry by index (0 = oldest, count-1 = newest)
     * Returns nullptr if index out of range
     */
    const LedgerEntry* ledger_entry(size_t index) const {
        if (index >= ledger_count_)
            return nullptr;
        size_t actual_idx = (ledger_start_ + index) % MAX_LEDGER_ENTRIES;
        return &ledger_[actual_idx];
    }

    /**
     * Get most recent ledger entry
     */
    const LedgerEntry* ledger_last() const {
        if (ledger_count_ == 0)
            return nullptr;
        return ledger_entry(ledger_count_ - 1);
    }

    /**
     * Check if any ledger entries have balance mismatches
     * Returns number of mismatches found
     */
    size_t ledger_check_balance() const {
        size_t mismatches = 0;
        for (size_t i = 0; i < ledger_count_; i++) {
            if (ledger_entry(i)->has_mismatch()) {
                mismatches++;
            }
        }
        return mismatches;
    }

    /**
     * Get first mismatch entry (for debugging)
     * Returns nullptr if no mismatches
     */
    const LedgerEntry* ledger_first_mismatch() const {
        for (size_t i = 0; i < ledger_count_; i++) {
            auto* entry = ledger_entry(i);
            if (entry->has_mismatch()) {
                return entry;
            }
        }
        return nullptr;
    }

    /**
     * Verify consistency: running totals == ledger sum
     * Returns true if consistent, false if mismatch found
     */
    bool verify_consistency() const {
        if (ledger_count_ == 0)
            return true;

        double calc_pnl = 0;
        double calc_commission = 0;
        double calc_gains = 0;
        double calc_losses = 0;

        for (size_t i = 0; i < ledger_count_; i++) {
            auto* e = ledger_entry(i);
            calc_pnl += e->realized_pnl;
            calc_commission += e->commission;
            if (e->realized_pnl > 0)
                calc_gains += e->realized_pnl;
            else if (e->realized_pnl < 0)
                calc_losses += std::abs(e->realized_pnl);
        }

        // Check running totals match ledger sum
        if (std::abs(calc_pnl - realized_pnl_) > 0.01)
            return false;
        if (std::abs(calc_commission - total_commission_) > 0.01)
            return false;
        if (std::abs(calc_gains - total_gains_) > 0.01)
            return false;
        if (std::abs(calc_losses - total_losses_) > 0.01)
            return false;

        return true;
    }

    /**
     * Dump ledger to stdout (for debugging)
     */
    void ledger_dump(size_t last_n = 10) const {
        std::printf("\n=== LEDGER (last %zu of %zu entries) ===\n", std::min(last_n, ledger_count_), ledger_count_);
        std::printf("%-4s %-8s %-4s %8s %8s %10s %10s %10s %8s %8s\n", "Seq", "Symbol", "Side", "Qty", "Price",
                    "TradeVal", "AvgEntry", "P&L", "Cash$", "OK?");
        std::printf("------------------------------------------------------------------------------------\n");

        size_t start = ledger_count_ > last_n ? ledger_count_ - last_n : 0;
        for (size_t i = start; i < ledger_count_; i++) {
            auto* e = ledger_entry(i);
            const char* status = (e->balance_ok && e->pnl_ok) ? "OK" : "ERR";
            std::printf("%-4u %-8s %-4s %8.3f %8.2f %10.2f %10.2f %+8.2f %8.2f %8s\n", e->sequence, e->ticker,
                        e->is_buy ? "BUY" : "SELL", e->quantity, e->price, e->trade_value, e->avg_entry,
                        e->realized_pnl, e->cash_after, status);

            // Show breakdown if there's an error
            if (!e->balance_ok || !e->pnl_ok) {
                if (!e->balance_ok) {
                    std::printf("     └─ CASH ERR: expected=%.2f actual=%.2f diff=%.4f\n", e->cash_expected,
                                e->cash_after, e->cash_discrepancy());
                }
                if (!e->pnl_ok && !e->is_buy) {
                    std::printf("     └─ P&L ERR: expected=%.2f actual=%.2f diff=%.4f (%.2f × %.3f)\n", e->expected_pnl,
                                e->realized_pnl, e->pnl_discrepancy(), e->pnl_per_unit, e->quantity);
                }
            }
        }
        std::printf("====================================================================================\n\n");
    }

private:
    // Append a new ledger entry, returns reference to the new entry
    LedgerEntry& append_ledger_entry() {
        size_t write_idx;
        if (ledger_count_ < MAX_LEDGER_ENTRIES) {
            // Not full yet, just append
            write_idx = ledger_count_;
            ledger_count_++;
        } else {
            // Full, overwrite oldest
            write_idx = ledger_start_;
            ledger_start_ = (ledger_start_ + 1) % MAX_LEDGER_ENTRIES;
        }

        auto& entry = ledger_[write_idx];
        // Clear entry
        std::memset(&entry, 0, sizeof(LedgerEntry));
        entry.sequence = ++ledger_seq_;
        return entry;
    }

    // Sync a completed ledger entry to SharedLedger (if connected)
    void sync_to_shared_ledger(const LedgerEntry& local) {
        if (!shared_ledger_)
            return;

        auto* e = shared_ledger_->append();
        if (!e)
            return;

        // Copy fields to SharedLedgerEntry (converting to fixed-point)
        e->timestamp_ns.store(local.timestamp_ns);
        // sequence already set by append()
        e->symbol.store(local.symbol);
        std::strncpy(e->ticker, local.ticker, sizeof(e->ticker) - 1);

        // Transaction details (fixed-point)
        e->price_x8.store(static_cast<int64_t>(local.price * ipc::LEDGER_FIXED_SCALE));
        e->quantity_x8.store(static_cast<int64_t>(local.quantity * ipc::LEDGER_FIXED_SCALE));
        e->commission_x8.store(static_cast<int64_t>(local.commission * ipc::LEDGER_FIXED_SCALE));

        // Cash flow
        e->cash_before_x8.store(static_cast<int64_t>(local.cash_before * ipc::LEDGER_FIXED_SCALE));
        e->cash_after_x8.store(static_cast<int64_t>(local.cash_after * ipc::LEDGER_FIXED_SCALE));
        e->cash_expected_x8.store(static_cast<int64_t>(local.cash_expected * ipc::LEDGER_FIXED_SCALE));

        // Calculation breakdown
        e->trade_value_x8.store(static_cast<int64_t>(local.trade_value * ipc::LEDGER_FIXED_SCALE));
        e->expected_cash_change_x8.store(static_cast<int64_t>(local.expected_cash_change * ipc::LEDGER_FIXED_SCALE));

        // P&L
        e->realized_pnl_x8.store(static_cast<int64_t>(local.realized_pnl * ipc::LEDGER_FIXED_SCALE));
        e->avg_entry_x8.store(static_cast<int64_t>(local.avg_entry * ipc::LEDGER_FIXED_SCALE));
        e->pnl_per_unit_x8.store(static_cast<int64_t>(local.pnl_per_unit * ipc::LEDGER_FIXED_SCALE));
        e->expected_pnl_x8.store(static_cast<int64_t>(local.expected_pnl * ipc::LEDGER_FIXED_SCALE));

        // Position state
        e->position_qty_x8.store(static_cast<int64_t>(local.position_qty * ipc::LEDGER_FIXED_SCALE));
        e->position_avg_x8.store(static_cast<int64_t>(local.position_avg * ipc::LEDGER_FIXED_SCALE));

        // Running totals
        e->running_realized_pnl_x8.store(static_cast<int64_t>(local.running_realized_pnl * ipc::LEDGER_FIXED_SCALE));
        e->running_commission_x8.store(static_cast<int64_t>(local.running_commission * ipc::LEDGER_FIXED_SCALE));

        // Flags
        e->is_buy.store(local.is_buy);
        e->is_exit.store(local.is_exit);
        e->exit_reason.store(local.exit_reason);
        e->balance_ok.store(local.balance_ok);
        e->pnl_ok.store(local.pnl_ok);
    }

    // Get current time in nanoseconds (simple implementation)
    static uint64_t now_ns() {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    }

    // Sync state to callback (if set)
    void sync_state() {
        if (sync_callback_) {
            sync_callback_(cash_, realized_pnl_, unrealized_pnl(), total_commission_, total_volume_, total_fills_,
                           winning_trades_, losing_trades_, target_count_, stop_count_);
        }
    }

    // Notify trade event (if callback set)
    void notify_trade(const TradeInput& input, double realized, bool is_buy, bool is_exit = false,
                      ExitReason reason = ExitReason::TARGET) {
        if (trade_callback_) {
            TradeEventInfo info{};
            info.symbol = input.symbol;
            info.ticker = input.ticker;
            info.price = input.price;
            info.quantity = input.quantity;
            info.realized_pnl = realized;
            info.commission = input.commission;
            info.is_buy = is_buy;
            info.is_exit = is_exit;
            info.exit_reason = reason;
            trade_callback_(info);
        }
    }

    // Callbacks
    SyncCallback sync_callback_ = nullptr;
    TradeEventCallback trade_callback_ = nullptr;

    // Optional SharedLedger for IPC visibility
    ipc::SharedLedger* shared_ledger_ = nullptr;

    // Cash and capital
    double cash_ = 0;
    double initial_cash_ = 0;

    // P&L tracking
    double realized_pnl_ = 0;
    double total_commission_ = 0;
    double total_volume_ = 0;

    // Trade counts
    uint32_t total_fills_ = 0;
    uint32_t winning_trades_ = 0;
    uint32_t losing_trades_ = 0;
    uint32_t target_count_ = 0;
    uint32_t stop_count_ = 0;

    // Gains/Losses tracking (separate from realized_pnl for breakdown)
    double total_gains_ = 0;  // Sum of positive P&L
    double total_losses_ = 0; // Sum of |negative P&L|

    // Position tracking
    std::array<RecorderPosition, MAX_RECORDER_SYMBOLS> positions_;

    // Ledger - circular buffer for audit trail
    std::array<LedgerEntry, MAX_LEDGER_ENTRIES> ledger_;
    size_t ledger_count_ = 0; // Number of entries (up to MAX_LEDGER_ENTRIES)
    size_t ledger_start_ = 0; // Start index in circular buffer
    uint32_t ledger_seq_ = 0; // Next sequence number
};

} // namespace trading
} // namespace hft
