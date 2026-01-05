#pragma once

#include "../types.hpp"
#include <atomic>
#include <string>
#include <functional>

namespace hft {
namespace account {

/**
 * AccountInfo - Current account state from broker/exchange
 *
 * All monetary values in smallest unit (cents for USD)
 * Example: $1,000,000.00 = 100000000
 */
struct AccountInfo {
    int64_t cash_balance = 0;       // Available cash (not in positions)
    int64_t buying_power = 0;       // Max can buy (includes margin)
    int64_t margin_used = 0;        // Margin currently in use
    int64_t margin_available = 0;   // Margin still available
    int64_t unrealized_pnl = 0;     // Open position P&L
    int64_t realized_pnl = 0;       // Closed position P&L (today)
    uint64_t sequence = 0;          // Update sequence number
    Timestamp last_update = 0;      // Last update timestamp

    // Equity = cash + unrealized P&L
    int64_t equity() const {
        return cash_balance + unrealized_pnl;
    }

    // Net liquidation value
    int64_t net_liq() const {
        return equity();
    }
};

/**
 * MarginRequirement - Per-symbol margin rules
 */
struct MarginRequirement {
    double initial_margin = 0.25;    // 25% = 4x leverage
    double maintenance_margin = 0.20; // 20% maintenance
    int64_t min_equity = 2500000;    // $25,000 minimum (PDT rule)
};

/**
 * OrderCost - Pre-trade cost calculation
 */
struct OrderCost {
    int64_t notional = 0;           // Price * Quantity
    int64_t margin_required = 0;    // Initial margin needed
    int64_t commission = 0;         // Estimated commission
    int64_t total_cost = 0;         // Total buying power needed
    bool can_afford = false;        // Have enough buying power?
    std::string reject_reason;      // Why rejected (if any)
};

// Callback for account updates from broker
using AccountUpdateCallback = std::function<void(const AccountInfo&)>;

/**
 * AccountManager - Manages account state and pre-trade checks
 *
 * Responsibilities:
 * 1. Track account balance and margin
 * 2. Calculate order costs before sending
 * 3. Reserve buying power for pending orders
 * 4. Block orders that exceed limits
 */
class AccountManager {
public:
    AccountManager() = default;

    explicit AccountManager(const MarginRequirement& margin_req)
        : margin_req_(margin_req) {}

    // ========================================
    // Account State Updates (from broker)
    // ========================================

    // Full account snapshot
    void update(const AccountInfo& info) {
        account_ = info;
        if (on_update_) {
            on_update_(account_);
        }
    }

    // Incremental updates
    void update_cash(int64_t cash) {
        account_.cash_balance = cash;
        account_.sequence++;
    }

    void update_buying_power(int64_t bp) {
        account_.buying_power = bp;
        account_.sequence++;
    }

    void update_margin(int64_t used, int64_t available) {
        account_.margin_used = used;
        account_.margin_available = available;
        account_.sequence++;
    }

    void update_pnl(int64_t unrealized, int64_t realized) {
        account_.unrealized_pnl = unrealized;
        account_.realized_pnl = realized;
        account_.sequence++;
    }

    // Set update callback
    void set_update_callback(AccountUpdateCallback cb) {
        on_update_ = std::move(cb);
    }

    // ========================================
    // Account State Queries
    // ========================================

    const AccountInfo& info() const { return account_; }

    int64_t cash_balance() const { return account_.cash_balance; }
    int64_t buying_power() const { return account_.buying_power - reserved_bp_; }
    int64_t margin_available() const { return account_.margin_available; }
    int64_t equity() const { return account_.equity(); }

    // Check if account meets minimum equity requirement
    bool meets_minimum_equity() const {
        return account_.equity() >= margin_req_.min_equity;
    }

    // ========================================
    // Pre-Trade Checks
    // ========================================

    // Calculate cost of an order before sending
    OrderCost calculate_order_cost(Side side, Quantity qty, Price price) const {
        OrderCost cost;

        // Calculate notional value
        cost.notional = static_cast<int64_t>(qty) * price;

        // Calculate margin required (for buys and short sells)
        cost.margin_required = static_cast<int64_t>(
            cost.notional * margin_req_.initial_margin);

        // Estimate commission (example: $0.005 per share, min $1)
        int64_t comm = static_cast<int64_t>(qty) * 50;  // 50 cents per 100 shares
        cost.commission = (comm > 100) ? comm : 100;    // min $1

        // Total cost
        if (side == Side::Buy) {
            // Buying: need margin + commission
            cost.total_cost = cost.margin_required + cost.commission;
        } else {
            // Selling/shorting: need margin + commission
            cost.total_cost = cost.margin_required + cost.commission;
        }

        // Check if we can afford it
        int64_t available_bp = account_.buying_power - reserved_bp_;
        cost.can_afford = (cost.total_cost <= available_bp);

        if (!cost.can_afford) {
            cost.reject_reason = "Insufficient buying power: need " +
                std::to_string(cost.total_cost / 100) + " have " +
                std::to_string(available_bp / 100);
        }

        // Check minimum equity (PDT rule)
        if (!meets_minimum_equity()) {
            cost.can_afford = false;
            cost.reject_reason = "Below minimum equity requirement ($25,000)";
        }

        return cost;
    }

    // Quick check without full calculation
    bool can_afford(Quantity qty, Price price) const {
        int64_t notional = static_cast<int64_t>(qty) * price;
        int64_t margin_needed = static_cast<int64_t>(
            notional * margin_req_.initial_margin);
        return margin_needed <= (account_.buying_power - reserved_bp_);
    }

    // ========================================
    // Buying Power Reservation
    // ========================================

    // Reserve buying power when order is sent (not yet filled)
    bool reserve_buying_power(int64_t amount) {
        if (amount > (account_.buying_power - reserved_bp_)) {
            return false;
        }
        reserved_bp_ += amount;
        return true;
    }

    // Release reserved buying power (order cancelled or filled)
    void release_buying_power(int64_t amount) {
        int64_t result = reserved_bp_ - amount;
        reserved_bp_ = (result > 0) ? result : 0;
    }

    // Get total reserved
    int64_t reserved_buying_power() const { return reserved_bp_; }

    // ========================================
    // Margin Configuration
    // ========================================

    void set_margin_requirement(const MarginRequirement& req) {
        margin_req_ = req;
    }

    const MarginRequirement& margin_requirement() const {
        return margin_req_;
    }

private:
    AccountInfo account_;
    MarginRequirement margin_req_;
    int64_t reserved_bp_ = 0;  // Reserved for pending orders
    AccountUpdateCallback on_update_;
};

}  // namespace account
}  // namespace hft
