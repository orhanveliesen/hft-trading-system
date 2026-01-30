#pragma once

#include "iexchange.hpp"
#include "paper_exchange.hpp"
#include <array>
#include <cstring>
#include <chrono>

namespace hft {
namespace exchange {

/**
 * PaperExchangeAdapter - Adapts PaperExchange to IExchange interface
 *
 * Handles type conversions between:
 * - IExchange: Symbol (uint32_t), Price (uint32_t), Quantity (double)
 * - PaperExchange: const char*, double, double
 *
 * Features:
 * - Symbol table for ID-to-string mapping
 * - Price scale conversion (Price is fixed-point)
 * - Forwards all operations to underlying PaperExchange
 * - Tracks statistics required by IExchange
 */
class PaperExchangeAdapter : public IExchange {
public:
    static constexpr size_t MAX_SYMBOLS = 64;  // Support more trading pairs
    static constexpr size_t MAX_SYMBOL_LEN = 16;
    static constexpr double DEFAULT_PRICE_SCALE = 1e4;  // 4 decimal places

    struct SymbolEntry {
        char name[MAX_SYMBOL_LEN];
        bool active;

        void clear() {
            std::memset(name, 0, sizeof(name));
            active = false;
        }
    };

    PaperExchangeAdapter(double price_scale = DEFAULT_PRICE_SCALE)
        : price_scale_(price_scale)
        , total_orders_(0)
        , total_fills_(0)
        , total_commission_(0)
    {
        for (auto& entry : symbol_table_) {
            entry.clear();
        }

        // Set up internal callback from PaperExchange
        paper_.set_execution_callback([this](const ipc::ExecutionReport& report) {
            handle_execution_report(report);
        });

        paper_.set_slippage_callback([this](double slippage) {
            if (on_slippage_) {
                on_slippage_(slippage);
            }
        });
    }

    // =========================================================================
    // Symbol Management
    // =========================================================================

    /// Register a symbol and get its numeric ID
    Symbol register_symbol(const char* name) {
        // Check if already registered
        for (size_t i = 0; i < MAX_SYMBOLS; ++i) {
            if (symbol_table_[i].active &&
                std::strcmp(symbol_table_[i].name, name) == 0) {
                return static_cast<Symbol>(i);
            }
        }

        // Find free slot
        for (size_t i = 0; i < MAX_SYMBOLS; ++i) {
            if (!symbol_table_[i].active) {
                std::strncpy(symbol_table_[i].name, name, MAX_SYMBOL_LEN - 1);
                symbol_table_[i].name[MAX_SYMBOL_LEN - 1] = '\0';
                symbol_table_[i].active = true;
                return static_cast<Symbol>(i);
            }
        }

        return static_cast<Symbol>(-1);  // No room
    }

    /// Register a symbol at a specific ID (to match engine's ID)
    bool register_symbol_at(const char* name, Symbol id) {
        if (id >= MAX_SYMBOLS) {
            return false;  // ID out of range
        }

        // Check if slot is already taken with a different symbol
        if (symbol_table_[id].active &&
            std::strcmp(symbol_table_[id].name, name) != 0) {
            return false;  // Conflict - different symbol already at this ID
        }

        // Register at the specified ID
        std::strncpy(symbol_table_[id].name, name, MAX_SYMBOL_LEN - 1);
        symbol_table_[id].name[MAX_SYMBOL_LEN - 1] = '\0';
        symbol_table_[id].active = true;
        return true;
    }

    /// Get symbol name from ID
    const char* symbol_name(Symbol id) const {
        if (id < MAX_SYMBOLS && symbol_table_[id].active) {
            return symbol_table_[id].name;
        }
        return "UNKNOWN";
    }

    // =========================================================================
    // IExchange Implementation - Order Operations
    // =========================================================================

    uint64_t send_market_order(
        Symbol symbol, Side side, double qty, Price expected_price
    ) override {
        const char* sym_name = symbol_name(symbol);
        double price_dbl = price_to_double(expected_price);
        uint64_t ts = now_ns();

        // For market orders, we need current bid/ask
        // Use expected_price as both bid and ask (caller should pass the right one)
        double bid = price_dbl;
        double ask = price_dbl;

        auto report = paper_.send_market_order(sym_name, side, qty, bid, ask, ts);
        total_orders_++;

        return report.order_id;
    }

    uint64_t send_limit_order(
        Symbol symbol, Side side, double qty, Price limit_price
    ) override {
        const char* sym_name = symbol_name(symbol);
        double price_dbl = price_to_double(limit_price);
        uint64_t ts = now_ns();

        auto report = paper_.send_limit_order(sym_name, side, qty, price_dbl, ts);
        total_orders_++;

        if (report.status == ipc::OrderStatus::Rejected) {
            return 0;  // Indicate failure
        }

        return report.order_id;
    }

    bool cancel_order(uint64_t order_id) override {
        return paper_.cancel_order(order_id, now_ns());
    }

    bool is_order_pending(uint64_t order_id) const override {
        return paper_.find_order(order_id) != nullptr;
    }

    // =========================================================================
    // IExchange Implementation - Price Updates
    // =========================================================================

    void on_price_update(
        Symbol symbol,
        Price bid,
        Price ask,
        uint64_t timestamp_ns
    ) override {
        const char* sym_name = symbol_name(symbol);
        double bid_dbl = price_to_double(bid);
        double ask_dbl = price_to_double(ask);

        paper_.on_price_update(sym_name, bid_dbl, ask_dbl, timestamp_ns);
    }

    // =========================================================================
    // IExchange Implementation - Callbacks
    // =========================================================================

    void set_fill_callback(FillCallback cb) override {
        on_fill_ = std::move(cb);
    }

    void set_slippage_callback(SlippageCallback cb) override {
        on_slippage_ = std::move(cb);
    }

    // =========================================================================
    // IExchange Implementation - Configuration
    // =========================================================================

    void set_commission_rate(double rate) override {
        commission_rate_ = rate;
        // Note: PaperExchange reads from SharedConfig, we could add a setter there
    }

    void set_slippage_bps(double bps) override {
        slippage_bps_ = bps;
        // Note: PaperExchange reads from SharedConfig
    }

    // =========================================================================
    // IExchange Implementation - Statistics
    // =========================================================================

    size_t pending_order_count() const override {
        return paper_.pending_count();
    }

    uint64_t total_orders() const override {
        return total_orders_;
    }

    uint64_t total_fills() const override {
        return total_fills_;
    }

    double total_slippage() const override {
        return paper_.total_slippage();
    }

    double total_commission() const override {
        return total_commission_;
    }

    // =========================================================================
    // IExchangeAdapter Implementation
    // =========================================================================

    bool is_paper() const override {
        return true;
    }

    // =========================================================================
    // Additional Methods
    // =========================================================================

    /// Set SharedConfig for the underlying PaperExchange
    void set_config(const ipc::SharedConfig* config) {
        paper_.set_config(config);
    }

    /// Set SharedPaperConfig for paper-trading specific settings
    void set_paper_config(const ipc::SharedPaperConfig* paper_config) {
        paper_.set_paper_config(paper_config);
    }

    /// Get underlying PaperExchange (for advanced use)
    PaperExchange& paper() { return paper_; }
    const PaperExchange& paper() const { return paper_; }

    /// Price conversion helpers
    double price_to_double(Price p) const {
        return static_cast<double>(p) / price_scale_;
    }

    Price double_to_price(double d) const {
        return static_cast<Price>(d * price_scale_);
    }

private:
    PaperExchange paper_;
    std::array<SymbolEntry, MAX_SYMBOLS> symbol_table_;
    double price_scale_;

    // Configuration
    double commission_rate_ = 0.001;
    double slippage_bps_ = 5.0;

    // Statistics
    uint64_t total_orders_;
    uint64_t total_fills_;
    double total_commission_;

    // Callbacks
    FillCallback on_fill_;
    SlippageCallback on_slippage_;

    void handle_execution_report(const ipc::ExecutionReport& report) {
        if (report.is_fill()) {
            total_fills_++;
            total_commission_ += report.commission;

            // Forward to callback with symbol name directly (no ID conversion)
            if (on_fill_) {
                Price fill_price = double_to_price(report.filled_price);

                on_fill_(
                    report.order_id,
                    report.symbol,  // Use name directly from ExecutionReport
                    report.side,
                    report.filled_qty,
                    fill_price,
                    report.commission
                );
            }
        }
    }

    Symbol find_symbol_id(const char* name) const {
        for (size_t i = 0; i < MAX_SYMBOLS; ++i) {
            if (symbol_table_[i].active &&
                std::strcmp(symbol_table_[i].name, name) == 0) {
                return static_cast<Symbol>(i);
            }
        }
        // Return invalid ID - caller should check
        return static_cast<Symbol>(-1);
    }

    uint64_t now_ns() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()
        ).count();
    }
};

}  // namespace exchange
}  // namespace hft
