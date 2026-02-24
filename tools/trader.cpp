/**
 * Trader - Unified Trading Application
 *
 * Single entry point for all trading operations.
 * Default: Production mode (real orders)
 * Use --paper for paper trading with simulated fills.
 *
 * Symbols are fetched dynamically from Binance Exchange Info API.
 * Falls back to hardcoded list if API is unavailable.
 *
 * Usage:
 *   trader                           # Production mode, all symbols
 *   trader --paper                   # Paper trading mode
 *   trader -s BTCUSDT                # Single symbol
 *   trader -s BTCUSDT,ETHUSDT        # Multiple symbols
 *   trader --paper -d 300            # Paper trade for 5 minutes
 *   trader -h                        # Help
 */

#include "../include/exchange/binance_ws.hpp"
#include "../include/exchange/paper_exchange.hpp"
#include "../include/ipc/event_publisher.hpp"
#include "../include/ipc/execution_report.hpp"
#include "../include/ipc/shared_config.hpp"
#include "../include/ipc/shared_event_log.hpp"
#include "../include/ipc/shared_ledger.hpp"
#include "../include/ipc/shared_paper_config.hpp"
#include "../include/ipc/shared_portfolio_state.hpp"
#include "../include/ipc/shared_ring_buffer.hpp"
#include "../include/ipc/symbol_config.hpp"
#include "../include/ipc/trade_event.hpp"
#include "../include/ipc/tuner_event.hpp"
#include "../include/ipc/udp_telemetry.hpp"
#include "../include/paper/paper_order_sender.hpp"
#include "../include/paper/queue_fill_detector.hpp"
#include "../include/risk/enhanced_risk_manager.hpp"
#include "../include/strategy/market_health_monitor.hpp"
#include "../include/strategy/regime_detector.hpp"
#include "../include/strategy/rolling_sharpe.hpp"
#include "../include/strategy/technical_indicators.hpp"
#include "../include/symbol_config.hpp"
#include "../include/trading/trade_recorder.hpp"
#include "../include/trading_engine.hpp"
// Position persistence removed - paper trading restarts fresh

// New unified architecture includes
#include "../include/config/defaults.hpp"
#include "../include/exchange/binance_rest.hpp"
#include "../include/exchange/iexchange.hpp"
#include "../include/exchange/paper_exchange_adapter.hpp"
#include "../include/exchange/production_order_sender.hpp"
#include "../include/execution/execution_engine.hpp"
#include "../include/strategy/config_strategy.hpp"
#include "../include/strategy/fair_value_strategy.hpp"
#include "../include/strategy/istrategy.hpp"
#include "../include/strategy/market_maker_strategy.hpp"
#include "../include/strategy/momentum_strategy.hpp"
#include "../include/strategy/strategy_constants.hpp"
#include "../include/strategy/strategy_selector.hpp"
#include "../include/strategy/symbol_strategy.hpp"
#include "../include/strategy/technical_indicators_strategy.hpp"
#include "../include/trading/portfolio.hpp"
#include "../include/util/cli.hpp"
#include "../include/util/system.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring> // For strncpy
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <sched.h> // For CPU affinity
#include <sstream>
#include <thread>
#include <vector>

using namespace hft;
using namespace hft::exchange;
using namespace hft::strategy;
using namespace hft::execution;

// Use Portfolio constants from trading namespace
using hft::trading::MAX_POSITIONS_PER_SYMBOL;
using hft::trading::MAX_SYMBOLS;
using hft::trading::OpenPosition;
using hft::trading::Portfolio;
using hft::trading::SymbolPositions;

// ============================================================================
// EMA Deviation Thresholds (max price above EMA to allow buy)
// ============================================================================
// Strategy constants - reference strategy_constants.hpp
// These control EMA filter strictness, streak thresholds, and auto-tune multipliers
using EmaThresholds = hft::strategy::EmaThresholds;
using StreakThresholds = hft::strategy::StreakThresholds;
using AutoTuneMultipliers = hft::strategy::AutoTuneMultipliers;

// ============================================================================
// Global State
// ============================================================================

std::atomic<bool> g_running{true};
ipc::SharedConfig* g_shared_config = nullptr; // For graceful shutdown signaling

/**
 * Pre-shutdown callback for signal handler.
 * Updates shared config status before g_running is set to false.
 */
void trader_pre_shutdown() {
    if (g_shared_config) {
        g_shared_config->set_trader_status(3); // shutting_down
        g_shared_config->update_heartbeat();
    }
}

// ============================================================================
// Using declarations
// ============================================================================

using namespace hft::ipc;
using hft::ipc::EventPublisher;

// ============================================================================
// CLI Arguments (from util/cli.hpp)
// ============================================================================
using CLIArgs = hft::util::CLIArgs;
using hft::util::parse_args;
using hft::util::print_help;

/**
 * Get default trading symbols from Binance API
 *
 * Fetches active USDT spot trading pairs from Binance Exchange Info API.
 * Throws std::runtime_error if API is unavailable - we should not trade
 * without current market data.
 */

// Symbols: Use exchange::fetch_default_symbols() from binance_rest.hpp
using hft::exchange::fetch_default_symbols;

// ============================================================================
// Order Senders (from paper/paper_order_sender.hpp, exchange/production_order_sender.hpp)
// ============================================================================
using PaperOrderSender = hft::paper::PaperOrderSender;
using ProductionOrderSender = hft::exchange::ProductionOrderSender;
using hft::exchange::LocalOrderSender;

// Verify concepts
static_assert(LocalOrderSender<PaperOrderSender>, "PaperOrderSender must satisfy LocalOrderSender");
static_assert(LocalOrderSender<ProductionOrderSender>, "ProductionOrderSender must satisfy LocalOrderSender");

// ============================================================================
// Trading Application
// ============================================================================

template <typename OrderSender>
class TradingApp {
public:
    explicit TradingApp(const CLIArgs& args)
        : args_(args), sender_(), engine_(sender_), total_ticks_(0),
          publisher_(true) // Always publish events for monitoring (dashboard, observer)
    {
        portfolio_.init(args.capital);

        // Initialize TradeRecorder - single source of truth for P&L
        trade_recorder_.init(args.capital);

        // Initialize EnhancedRiskManager with capital and default limits
        {
            risk::EnhancedRiskConfig risk_cfg;
            risk_cfg.initial_capital = static_cast<int64_t>(args.capital * risk::PRICE_SCALE);
            risk_cfg.daily_loss_limit_pct = 0.03; // 3% daily loss limit
            risk_cfg.max_drawdown_pct = 0.05;     // 5% max drawdown
            risk_cfg.max_notional_pct = 2.0;      // 200% max exposure
            risk_cfg.max_order_size = 1000000;    // Large enough for crypto
            risk_manager_ = risk::EnhancedRiskManager(risk_cfg);
        }

        // Initialize shared portfolio state for dashboard/observer
        if (args.paper_mode) {
            // Always create fresh portfolio state (no crash recovery for paper trading)
            SharedPortfolioState::destroy("/trader_portfolio");
            portfolio_state_ = SharedPortfolioState::create("/trader_portfolio", args.capital);
            if (portfolio_state_) {
                std::cout << "[IPC] Portfolio state initialized "
                          << "(session=" << std::hex << std::uppercase << portfolio_state_->session_id << std::dec
                          << ", cash=$" << args.capital << ")\n";
            }

            // Initialize SharedLedger for IPC visibility
            ipc::SharedLedger::destroy("/trader_ledger");
            shared_ledger_ = ipc::SharedLedger::create("/trader_ledger");
            if (shared_ledger_) {
                trade_recorder_.connect_shared_ledger(shared_ledger_);
                std::cout << "[IPC] Ledger initialized (max entries: " << ipc::MAX_SHARED_LEDGER_ENTRIES << ")\n";
            }

            // Open shared config (dashboard can modify this)
            // Try to open existing, if version mismatch destroy and recreate
            shared_config_ = SharedConfig::open_rw("/trader_config");
            if (!shared_config_) {
                // Either doesn't exist or version mismatch - destroy and create fresh
                SharedConfig::destroy("/trader_config");
                shared_config_ = SharedConfig::create("/trader_config");
            }
            if (shared_config_) {
                last_config_seq_ = shared_config_->sequence.load();
                std::cout << "[IPC] Config loaded (spread_mult=" << shared_config_->spread_multiplier() << "x)\n";

                // Register HFT lifecycle in shared config
                shared_config_->set_trader_pid(getpid());
                shared_config_->set_trader_status(1); // starting
                shared_config_->update_heartbeat();
                g_shared_config = shared_config_; // For signal handler

                // Set config for Portfolio (reads target%, stop%, commission from config)
                portfolio_.set_config(shared_config_);
                std::cout << "[CONFIG] Portfolio: target=" << (portfolio_.target_pct() * 100) << "%, "
                          << "stop=" << (portfolio_.stop_pct() * 100) << "%, "
                          << "commission=" << (portfolio_.commission_rate() * 100) << "%, "
                          << "position=" << (portfolio_.base_position_pct() * 100) << "%\n";
            }

            // Connect to symbol-specific configs from tuner
            // Tuner sets per-symbol position sizing, targets, etc.
            symbol_configs_ = ipc::SharedSymbolConfigs::open_rw("/trader_symbol_configs");
            if (!symbol_configs_) {
                // Create if doesn't exist (tuner may not have run yet)
                symbol_configs_ = ipc::SharedSymbolConfigs::create("/trader_symbol_configs");
            }
            if (symbol_configs_) {
                portfolio_.set_symbol_configs(symbol_configs_);
                std::cout << "[IPC] Symbol configs connected (supports per-symbol tuning)\n";
            }

            // Initialize paper trading config (separate from main config for SRP)
            shared_paper_config_ = SharedPaperConfig::open_rw("/trader_paper_config");
            if (!shared_paper_config_) {
                SharedPaperConfig::destroy("/trader_paper_config");
                shared_paper_config_ = SharedPaperConfig::create("/trader_paper_config");
            }
            if (shared_paper_config_) {
                std::cout << "[IPC] Paper config loaded (slippage=" << shared_paper_config_->slippage_bps()
                          << " bps)\n";
            }

            // Initialize event log for tuner and web interface
            event_log_ = SharedEventLog::create();
            if (event_log_) {
                std::cout << "[IPC] Event log initialized (ring size: " << EVENT_LOG_RING_SIZE << " events)\n";
                // Log startup event
                TunerEvent startup;
                startup.init(TunerEventType::ProcessStart, "*");
                startup.set_reason("HFT engine started");
                event_log_->log(startup);
            }
        }

        if constexpr (std::is_same_v<OrderSender, PaperOrderSender>) {
            // Configure PaperOrderSender with slippage settings
            sender_.set_config(shared_config_);
            sender_.set_fill_callback(
                [this](Symbol s, OrderId id, Side side, double q, Price p) { on_fill(s, id, side, q, p); });
            sender_.set_slippage_callback([this](double slippage_cost) {
                if (portfolio_state_) {
                    portfolio_state_->add_slippage(slippage_cost);
                }
            });

            // Initialize new PaperExchange with config and callbacks
            paper_exchange_.set_config(shared_config_);
            paper_exchange_.set_paper_config(shared_paper_config_); // Paper-specific settings
            paper_exchange_.set_execution_callback(
                [this](const ipc::ExecutionReport& report) { on_execution_report(report); });
            paper_exchange_.set_slippage_callback([this](double slippage_cost) {
                if (portfolio_state_) {
                    portfolio_state_->add_slippage(slippage_cost);
                }
            });

            double slippage = paper_exchange_.get_slippage_bps();
            std::cout << "[PAPER] PaperExchange initialized (commission="
                      << (shared_config_ ? shared_config_->commission_rate() * 100 : 0.1) << "%, "
                      << "slippage=" << slippage << " bps)\n";
        }

        // Register strategies in unified selector
        register_strategies();

        // Initialize unified execution architecture (paper mode only for now)
        if constexpr (std::is_same_v<OrderSender, PaperOrderSender>) {
            // Create PaperExchangeAdapter with same price scale
            paper_adapter_ = std::make_unique<PaperExchangeAdapter>(risk::PRICE_SCALE);
            paper_adapter_->set_config(shared_config_);
            paper_adapter_->set_paper_config(shared_paper_config_); // Paper-specific settings

            // Set up fill callback to route through on_execution_report
            // NOTE: qty is double (not Quantity/uint32_t) for fractional crypto quantities
            paper_adapter_->set_fill_callback([this](uint64_t order_id, const char* symbol_name, Side side, double qty,
                                                     Price fill_price, double commission) {
                // Convert back to ExecutionReport format
                ipc::ExecutionReport report;
                report.clear(); // CRITICAL: Initialize all fields to zero
                report.order_id = order_id;
                report.side = side;
                report.filled_qty = qty; // Already double, no conversion needed
                report.filled_price = static_cast<double>(fill_price) / risk::PRICE_SCALE;
                report.commission = commission;
                report.status = ipc::OrderStatus::Filled;
                report.exec_type = ipc::ExecType::Trade; // CRITICAL: Set for is_fill() check

                // Use symbol name directly from callback (no ID conversion needed)
                std::strncpy(report.symbol, symbol_name, sizeof(report.symbol) - 1);
                report.symbol[sizeof(report.symbol) - 1] = '\0';

                // Route through unified handler
                on_execution_report(report);
            });

            paper_adapter_->set_slippage_callback([this](double slippage_cost) {
                if (portfolio_state_) {
                    portfolio_state_->add_slippage(slippage_cost);
                }
            });

            // Wire ExecutionEngine to use the adapter
            execution_engine_.set_exchange(paper_adapter_.get());

            // Set position callback to prevent overselling
            // This ensures we never sell more than we own
            execution_engine_.set_position_callback(
                [this](Symbol symbol) -> double { return portfolio_.positions[symbol].total_quantity(); });

            std::cout << "[EXEC] ExecutionEngine initialized with PaperExchangeAdapter\n";
        }

        // UDP Telemetry for remote monitoring
        if (telemetry_.is_valid()) {
            std::cout << "[UDP] Telemetry publisher initialized (multicast: 239.255.0.1:5555)\n";
        }
    }

    ~TradingApp() {
        // Log shutdown event before cleanup
        if (event_log_) {
            TunerEvent e;
            e.init(TunerEventType::ProcessStop, "*");
            e.set_reason("HFT engine stopped");
            event_log_->log(e);
        }


        // Normal shutdown - cleanup shared memory
        if (portfolio_state_) {
            // Mark trading as inactive
            portfolio_state_->trading_active.store(0);

            // Print final summary before cleanup
            double slippage = portfolio_state_->total_slippage();
            double total_costs = portfolio_.total_commissions + slippage;
            std::cout << "\n[CLEANUP] Final portfolio state:\n"
                      << "  Cash: $" << std::fixed << std::setprecision(2) << portfolio_state_->cash() << "\n"
                      << "  Realized P&L: $" << portfolio_state_->total_realized_pnl() << "\n"
                      << "  Commissions: $" << portfolio_.total_commissions << "\n"
                      << "  Slippage: $" << slippage << "\n"
                      << "  Total Costs: $" << total_costs << "\n"
                      << "  Net P&L: $" << (portfolio_state_->total_realized_pnl() - total_costs) << "\n"
                      << "  Fills: " << portfolio_state_->total_fills.load()
                      << ", Targets: " << portfolio_state_->total_targets.load()
                      << ", Stops: " << portfolio_state_->total_stops.load() << "\n"
                      << "  Win rate: " << std::setprecision(1) << portfolio_state_->win_rate() << "%\n";

            // Unmap and unlink shared memory
            munmap(portfolio_state_, sizeof(SharedPortfolioState));
            SharedPortfolioState::destroy("/trader_portfolio");
            std::cout << "[IPC] Portfolio state cleaned up\n";
        }

        // Cleanup shared config (mark as stopped before unmapping)
        if (shared_config_) {
            shared_config_->set_trader_status(0); // stopped
            shared_config_->update_heartbeat();
            g_shared_config = nullptr;
            munmap(shared_config_, sizeof(SharedConfig));
            std::cout << "[IPC] Config unmapped, HFT marked as stopped\n";
        }

        // Cleanup symbol-specific configs
        if (symbol_configs_) {
            munmap(symbol_configs_, sizeof(ipc::SharedSymbolConfigs));
            std::cout << "[IPC] Symbol configs unmapped\n";
        }
    }

    void add_symbol(const std::string& ticker) {
        // Called during init only, before trading starts
        if (engine_.lookup_symbol(ticker).has_value())
            return;

        SymbolConfig cfg;
        cfg.symbol = ticker;
        cfg.max_position = args_.max_position;
        cfg.max_loss = 1000 * risk::PRICE_SCALE;

        Symbol id = engine_.add_symbol(cfg);
        if (id < MAX_SYMBOLS) {
            strategies_[id].init(ticker);

            // Initialize ConfigStrategy for this symbol (used when TunerState is ON/PAUSED)
            config_strategies_[id] = std::make_unique<ConfigStrategy>(shared_config_, symbol_configs_, ticker.c_str());

            // Initialize portfolio state slot with matching index
            // This ensures update_last_price_relaxed(id, price) writes to correct slot
            if (portfolio_state_) {
                portfolio_state_->init_slot(id, ticker.c_str());
            }

            // Register symbol with unified exchange adapter at same ID as engine
            if (paper_adapter_) {
                paper_adapter_->register_symbol_at(ticker.c_str(), id);
            }
        }
    }

    void on_quote(const std::string& ticker, Price bid, Price ask, Quantity bid_size, Quantity ask_size) {
        // Hot path - no locks, O(1) array access
        auto opt = engine_.lookup_symbol(ticker);
        if (!opt)
            return;

        Symbol id = *opt;
        if (id >= MAX_SYMBOLS)
            return;

        auto* world = engine_.get_symbol_world(id);
        if (!world)
            return;

        total_ticks_.fetch_add(1, std::memory_order_relaxed);

        // Update L1 with real order book sizes from exchange
        L1Snapshot snap;
        snap.bid_price = bid;
        snap.bid_size = bid_size;
        snap.ask_price = ask;
        snap.ask_size = ask_size;
        world->apply_snapshot(snap);

        // Process paper fills (legacy + new PaperExchange)
        if constexpr (std::is_same_v<OrderSender, PaperOrderSender>) {
            sender_.process_fills(id, bid, ask);

            // New PaperExchange: check pending limit orders
            // Convert scaled prices to USD
            double bid_usd = static_cast<double>(bid) / risk::PRICE_SCALE;
            double ask_usd = static_cast<double>(ask) / risk::PRICE_SCALE;
            uint64_t ts = std::chrono::steady_clock::now().time_since_epoch().count();
            paper_exchange_.on_price_update(ticker.c_str(), bid_usd, ask_usd, ts);

            // Also update PaperExchangeAdapter for unified execution (fills limit orders)
            if (paper_adapter_) {
                paper_adapter_->on_price_update(id, bid, ask, ts);
            }
        }

        // Update regime and spread - O(1) array access
        auto& strat = strategies_[id];
        if (!strat.active)
            return;

        // Track spread for dynamic thresholds
        strat.update_spread(bid, ask);

        double mid = (bid + ask) / 2.0 / risk::PRICE_SCALE;

        // Update last price in shared state for dashboard charts
        // Ultra-low latency: relaxed memory ordering (~1 cycle vs ~15)
        if (portfolio_state_) {
            portfolio_state_->update_last_price_relaxed(static_cast<size_t>(id), static_cast<int64_t>(mid * 1e8));
        }
        strat.regime.update(mid);
        strat.indicators.update(mid); // Update technical indicators

        // Update market snapshot for AI tuner (every tick)
        if (portfolio_state_ && id < ipc::MAX_PORTFOLIO_SYMBOLS) {
            auto& snap = portfolio_state_->positions[id].snapshot;

            // Update high/low - branchless using std::max/std::min
            // max(0, pos) = pos handles first tick case for high
            // For low, use INT64_MAX sentinel on first tick to make min() work correctly
            int64_t mid_x8 = static_cast<int64_t>(mid * 1e8);
            int64_t curr_high = snap.price_high_x8.load(std::memory_order_relaxed);
            int64_t curr_low = snap.price_low_x8.load(std::memory_order_relaxed);

            // High: max(curr_high, mid_x8) - always correct since max(0, positive) = positive
            snap.price_high_x8.store(std::max(curr_high, mid_x8), std::memory_order_relaxed);

            // Low: if curr_low == 0 (first tick), treat as INT64_MAX so min() returns mid_x8
            int64_t effective_low = (curr_low == 0) ? INT64_MAX : curr_low;
            snap.price_low_x8.store(std::min(effective_low, mid_x8), std::memory_order_relaxed);

            // Set open price if first tick
            if (snap.price_open_x8.load(std::memory_order_relaxed) == 0) {
                snap.price_open_x8.store(mid_x8, std::memory_order_relaxed);
            }

            // Update EMA-20 from indicators (using slow EMA as proxy for EMA-20)
            double ema = strat.indicators.ema_slow();
            if (ema > 0) {
                snap.ema_20_x8.store(static_cast<int64_t>(ema * 1e8), std::memory_order_relaxed);
            }

            // ATR not available in current indicators, use BB width as volatility proxy
            double bb_width = strat.indicators.bb_width();
            if (bb_width > 0) {
                snap.atr_14_x8.store(static_cast<int64_t>(bb_width * 1e8), std::memory_order_relaxed);
            }

            // Update volatility from regime detector
            double vol = strat.regime.volatility() * 100.0; // Convert to %
            snap.volatility_x100.store(static_cast<int32_t>(vol * 100), std::memory_order_relaxed);

            // Update trend direction based on regime
            int8_t trend = 0;
            if (strat.current_regime == MarketRegime::TrendingUp)
                trend = 1;
            else if (strat.current_regime == MarketRegime::TrendingDown)
                trend = -1;
            snap.trend_direction.store(trend, std::memory_order_relaxed);

            // Increment tick count
            snap.tick_count.fetch_add(1, std::memory_order_relaxed);
        }

        // Update unified strategies with market snapshot
        MarketSnapshot market_snap;
        market_snap.bid = bid;
        market_snap.ask = ask;
        market_snap.bid_size = bid_size; // Real order book data from exchange
        market_snap.ask_size = ask_size;
        market_snap.last_trade = (bid + ask) / 2;
        market_snap.timestamp_ns = std::chrono::steady_clock::now().time_since_epoch().count();
        strategy_selector_.on_tick_all(market_snap);

        // Update ConfigStrategy with tick (for readiness tracking)
        if (config_strategies_[id]) {
            config_strategies_[id]->on_tick(market_snap);
        }

        MarketRegime new_regime = strat.regime.current_regime();
        if (new_regime != strat.current_regime) {
            // Regime changed - publish to observer
            if (strat.current_regime != MarketRegime::Unknown) {
                publisher_.regime_change(id, strat.ticker, static_cast<uint8_t>(new_regime));

                // Event log for tuner/web tracking
                if (event_log_) {
                    TunerEvent e =
                        TunerEvent::make_regime_change(strat.ticker, static_cast<uint8_t>(strat.current_regime),
                                                       static_cast<uint8_t>(new_regime), strat.regime.confidence());
                    event_log_->log(e);
                }
            }
            strat.current_regime = new_regime;

            // Update shared state for dashboard (~5ns)
            if (portfolio_state_) {
                portfolio_state_->update_regime(strat.ticker, static_cast<uint8_t>(new_regime));
            }
        }

        // Update market health monitor with spike state
        market_health_.update_symbol(static_cast<size_t>(id), strat.regime.is_spike());
        market_health_.tick(); // Decrement cooldown if active

        // Check for market-wide crash - emergency liquidate all positions
        if (market_health_.should_liquidate()) {
            emergency_liquidate(bid);
        }

        // Generate buy signals
        // Skip trading if market is dangerous (spike or high volatility) or in cooldown after crash
        // Note: Strategies run autonomously - they don't depend on tuner connection
        if (engine_.can_trade() && !world->is_halted() && !strat.regime.is_dangerous() &&
            !market_health_.in_cooldown()) {
            check_signal(id, world, &strat, bid, ask);
        }

        // Check target/stop-loss for this symbol - O(n), no allocation
        // IMPORTANT: Skip when tuner_mode is ON - unified system handles exits via exchange
        // This prevents double-counting cash updates
        bool use_legacy_exits =
            !shared_config_ || !(shared_config_->is_tuner_on() || shared_config_->is_tuner_paused());
        if (use_legacy_exits && portfolio_.symbol_active[id]) {
            double bid_usd = static_cast<double>(bid) / risk::PRICE_SCALE;
            const char* ticker = strat.ticker;

            portfolio_.check_and_close(
                id, bid_usd,
                // On target hit (profit)
                [&](double qty, double entry, double exit) {
                    double profit = (exit - entry) * qty;
                    double trade_value = exit * qty;
                    double commission = trade_value * portfolio_.commission_rate();

                    // Record exit in TradeRecorder (ledger + IPC)
                    trading::TradeInput exit_input{};
                    exit_input.symbol = id;
                    exit_input.price = exit;
                    exit_input.quantity = qty;
                    exit_input.commission = commission;
                    std::strncpy(exit_input.ticker, ticker, sizeof(exit_input.ticker) - 1);
                    trade_recorder_.record_exit(trading::ExitReason::TARGET, exit_input);

                    // Update shared portfolio state (~5ns)
                    if (portfolio_state_) {
                        portfolio_state_->set_cash(portfolio_.cash);
                        portfolio_state_->add_realized_pnl(profit);
                        portfolio_state_->add_commission(commission);
                        portfolio_state_->add_volume(trade_value);
                        portfolio_state_->record_target();
                        portfolio_state_->record_event();
                        // Update position (will be 0 qty if fully closed)
                        auto& pos = portfolio_.positions[id];
                        portfolio_state_->update_position(ticker, pos.total_quantity(), pos.avg_entry(), exit);
                    }

                    // Track win streak
                    record_win();

                    // Publish to observer (~5ns)
                    publisher_.target_hit(id, ticker, entry, exit, qty);

                    // UDP telemetry: P&L update
                    if (portfolio_state_) {
                        telemetry_.publish_pnl(static_cast<int64_t>(portfolio_state_->total_realized_pnl() * 1e8),
                                               static_cast<int64_t>(portfolio_state_->total_unrealized_pnl() * 1e8),
                                               static_cast<int64_t>(portfolio_state_->total_equity() * 1e8),
                                               portfolio_state_->winning_trades.load(),
                                               portfolio_state_->losing_trades.load());
                    }

                    if (args_.verbose) {
                        std::cout << "[TARGET] " << ticker << " SELL " << qty << " @ $" << std::fixed
                                  << std::setprecision(2) << exit << " (entry=$" << entry << ", profit=$"
                                  << std::setprecision(2) << profit << ")\n";
                    }
                },
                // On stop-loss hit (cut loss)
                [&](double qty, double entry, double exit) {
                    double loss = (exit - entry) * qty; // Will be negative
                    double trade_value = exit * qty;
                    double commission = trade_value * portfolio_.commission_rate();

                    // Record exit in TradeRecorder (ledger + IPC)
                    trading::TradeInput exit_input{};
                    exit_input.symbol = id;
                    exit_input.price = exit;
                    exit_input.quantity = qty;
                    exit_input.commission = commission;
                    std::strncpy(exit_input.ticker, ticker, sizeof(exit_input.ticker) - 1);
                    trade_recorder_.record_exit(trading::ExitReason::STOP, exit_input);

                    // Update shared portfolio state (~5ns)
                    if (portfolio_state_) {
                        portfolio_state_->set_cash(portfolio_.cash);
                        portfolio_state_->add_realized_pnl(loss);
                        portfolio_state_->add_commission(commission);
                        portfolio_state_->add_volume(trade_value);
                        portfolio_state_->record_stop();
                        portfolio_state_->record_event();
                        // Update position (will be 0 qty if fully closed)
                        auto& pos = portfolio_.positions[id];
                        portfolio_state_->update_position(ticker, pos.total_quantity(), pos.avg_entry(), exit);
                    }

                    // Track loss streak
                    record_loss();

                    // Publish to observer (~5ns)
                    publisher_.stop_loss(id, ticker, entry, exit, qty);

                    // UDP telemetry: P&L update
                    if (portfolio_state_) {
                        telemetry_.publish_pnl(static_cast<int64_t>(portfolio_state_->total_realized_pnl() * 1e8),
                                               static_cast<int64_t>(portfolio_state_->total_unrealized_pnl() * 1e8),
                                               static_cast<int64_t>(portfolio_state_->total_equity() * 1e8),
                                               portfolio_state_->winning_trades.load(),
                                               portfolio_state_->losing_trades.load());
                    }

                    if (args_.verbose) {
                        std::cout << "[STOP] " << ticker << " SELL " << qty << " @ $" << std::fixed
                                  << std::setprecision(2) << exit << " (entry=$" << entry << ", loss=$"
                                  << std::setprecision(2) << -loss << ")\n";
                    }
                },
                // On trend exit (profit taking on pullback from peak)
                [&](double qty, double entry, double exit, double peak) {
                    double profit = (exit - entry) * qty;
                    double trade_value = exit * qty;
                    double commission = trade_value * portfolio_.commission_rate();

                    // Record exit in TradeRecorder (ledger + IPC)
                    trading::TradeInput exit_input{};
                    exit_input.symbol = id;
                    exit_input.price = exit;
                    exit_input.quantity = qty;
                    exit_input.commission = commission;
                    std::strncpy(exit_input.ticker, ticker, sizeof(exit_input.ticker) - 1);
                    trade_recorder_.record_exit(trading::ExitReason::PULLBACK, exit_input);

                    // Update shared portfolio state
                    if (portfolio_state_) {
                        portfolio_state_->set_cash(portfolio_.cash);
                        portfolio_state_->add_realized_pnl(profit);
                        portfolio_state_->add_commission(commission);
                        portfolio_state_->add_volume(trade_value);
                        portfolio_state_->record_target(); // Count as target hit
                        portfolio_state_->record_event();
                        auto& pos = portfolio_.positions[id];
                        portfolio_state_->update_position(ticker, pos.total_quantity(), pos.avg_entry(), exit);
                    }

                    // Track win streak
                    record_win();

                    // Publish to observer as target hit (since it's profitable)
                    publisher_.target_hit(id, ticker, entry, exit, qty);

                    // UDP telemetry
                    if (portfolio_state_) {
                        telemetry_.publish_pnl(static_cast<int64_t>(portfolio_state_->total_realized_pnl() * 1e8),
                                               static_cast<int64_t>(portfolio_state_->total_unrealized_pnl() * 1e8),
                                               static_cast<int64_t>(portfolio_state_->total_equity() * 1e8),
                                               portfolio_state_->winning_trades.load(),
                                               portfolio_state_->losing_trades.load());
                    }

                    if (args_.verbose) {
                        std::cout << "[TREND] " << ticker << " SELL " << qty << " @ $" << std::fixed
                                  << std::setprecision(2) << exit << " (entry=$" << entry << ", peak=$" << peak
                                  << ", profit=$" << std::setprecision(2) << profit << ")\n";
                    }
                },
                shared_config_ ? shared_config_->pullback_pct() : 0.005 // From config or default 0.5%
            );
        }
    }

    // Stats for final summary (called after trading stops, not on hot path)
    struct Stats {
        size_t symbols = 0;
        uint64_t ticks = 0;
        uint64_t orders = 0;
        uint64_t fills = 0;
        double cash = 0;
        double holdings_value = 0;
        double equity = 0;
        double pnl = 0;
        int positions = 0;
        bool halted = false;
    };

    Stats get_stats() {
        // Called after trading stops - not performance critical
        Stats s;
        s.symbols = engine_.symbol_count();
        s.ticks = total_ticks_.load(std::memory_order_relaxed);
        s.halted = !engine_.can_trade();
        s.cash = portfolio_.cash;

        if constexpr (std::is_same_v<OrderSender, PaperOrderSender>) {
            s.orders = sender_.total_orders();
            s.fills = sender_.total_fills();
        }

        // Calculate holdings value using fixed array (no std::map)
        std::array<double, MAX_SYMBOLS> prices{};
        engine_.for_each_symbol([&](const SymbolWorld& w) {
            Price mid = w.top().mid_price();
            if (mid > 0 && w.id() < MAX_SYMBOLS) {
                prices[w.id()] = static_cast<double>(mid) / risk::PRICE_SCALE;
            }
        });

        s.holdings_value = 0;
        s.positions = 0;
        for (size_t sym = 0; sym < MAX_SYMBOLS; sym++) {
            if (!portfolio_.symbol_active[sym] || prices[sym] <= 0)
                continue;

            double sym_qty = portfolio_.positions[sym].total_quantity();
            if (sym_qty > 0) {
                s.holdings_value += sym_qty * prices[sym];
                s.positions++;
            }
        }

        s.equity = s.cash + s.holdings_value;
        s.pnl = s.equity - args_.capital;
        return s;
    }

    bool is_halted() const { return !engine_.can_trade(); }

    // Called periodically from main loop for UDP telemetry heartbeat
    void publish_telemetry_heartbeat() { telemetry_.publish_heartbeat(); }

    // Called periodically from main loop for IPC heartbeat
    void publish_heartbeat() { publisher_.heartbeat(); }

private:
    CLIArgs args_;
    OrderSender sender_;
    TradingEngine<OrderSender> engine_;
    std::array<SymbolStrategy, MAX_SYMBOLS> strategies_; // Fixed array, O(1) access
    std::atomic<uint64_t> total_ticks_;
    // No mutex - single-threaded hot path, lock-free design
    Portfolio portfolio_;
    EventPublisher publisher_;                           // Lock-free event publishing to observer
    ipc::TelemetryPublisher telemetry_;                  // UDP multicast for remote monitoring
    SharedPortfolioState* portfolio_state_ = nullptr;    // Shared state for dashboard
    SharedConfig* shared_config_ = nullptr;              // Shared config from dashboard
    ipc::SharedSymbolConfigs* symbol_configs_ = nullptr; // Symbol-specific tuning from tuner
    trading::TradeRecorder trade_recorder_;              // Single source of truth for P&L
    ipc::SharedLedger* shared_ledger_ = nullptr;         // IPC ledger for dashboard
    SharedPaperConfig* shared_paper_config_ = nullptr;   // Paper trading settings
    SharedEventLog* event_log_ = nullptr;                // Event log for tuner/web
    uint32_t last_config_seq_ = 0;                       // Track config changes

    // Paper exchange (only used in paper mode)
    hft::exchange::PaperExchange paper_exchange_;

    // Unified strategy architecture
    StrategySelector strategy_selector_;
    execution::ExecutionEngine execution_engine_;
    std::unique_ptr<PaperExchangeAdapter> paper_adapter_; // Owned adapter for IExchange

    // Per-symbol ConfigStrategy instances (used when TunerState is ON or PAUSED)
    std::array<std::unique_ptr<ConfigStrategy>, MAX_SYMBOLS> config_strategies_;

    // Market health monitor for crash detection
    MarketHealthMonitor market_health_{MAX_SYMBOLS, 0.5, 60}; // 50% threshold, 60 tick cooldown

    // Strategy mode tracking
    int32_t consecutive_wins_ = 0;
    int32_t consecutive_losses_ = 0;
    uint8_t active_mode_ = 2; // NORMAL by default

    // Enhanced risk manager for position/notional limits and P&L tracking
    risk::EnhancedRiskManager risk_manager_;

    void update_active_mode() {
        if (!shared_config_)
            return;

        // Determine mode based on performance and config
        uint8_t force = shared_config_->get_force_mode();
        if (force > 0) {
            // Manual override
            active_mode_ = force;
        } else {
            // Auto mode - adjust based on performance
            int32_t loss_limit = shared_config_->loss_streak();
            if (consecutive_losses_ >= loss_limit) {
                active_mode_ = 3; // CAUTIOUS
            } else if (consecutive_losses_ >= loss_limit + 2) {
                active_mode_ = 4; // DEFENSIVE
            } else if (consecutive_wins_ >= 3) {
                active_mode_ = 1; // AGGRESSIVE
            } else {
                active_mode_ = 2; // NORMAL
            }
        }

        // Update shared config for dashboard
        shared_config_->set_active_mode(active_mode_);
        shared_config_->set_consecutive_wins(consecutive_wins_);
        shared_config_->set_consecutive_losses(consecutive_losses_);

        // Apply auto-tuning if enabled
        auto_tune_params();
    }

    // Base values for auto-tune (saved when auto-tune first activates)
    int32_t base_cooldown_ms_ = 0;
    double base_min_trade_value_ = 0;
    bool auto_tune_base_saved_ = false;

    /**
     * Auto-tune parameters based on win/loss streaks
     *
     * Rules:
     *   2 losses  -> cooldown +50%
     *   3 losses  -> signal_strength = Strong (2)
     *   4 losses  -> min_trade_value +50%
     *   5+ losses -> TRADING PAUSED
     *   3 wins    -> gradually relax parameters back to base
     */
    void auto_tune_params() {
        if (!shared_config_)
            return;
        if (!shared_config_->is_tuner_off())
            return;

        // Save base values on first call (so we can relax back to them)
        if (!auto_tune_base_saved_) {
            base_cooldown_ms_ = shared_config_->get_cooldown_ms();
            base_min_trade_value_ = shared_config_->min_trade_value();
            auto_tune_base_saved_ = true;
        }

        // ===== LOSS STREAK: Tighten parameters =====
        // Thresholds read from SharedConfig for runtime configurability
        const int losses_to_pause = shared_config_->get_losses_to_pause();
        const int losses_to_defensive = shared_config_->get_losses_to_defensive();
        const int losses_to_tighten = shared_config_->get_losses_to_tighten_signal();
        const int losses_to_cautious = shared_config_->get_losses_to_cautious();

        if (consecutive_losses_ >= losses_to_pause) {
            // 5+ losses: PAUSE TRADING
            if (shared_config_->trading_enabled.load()) {
                shared_config_->set_trading_enabled(false);
                publisher_.status(0, "ALL", StatusCode::AutoTunePaused, 0, static_cast<uint8_t>(consecutive_losses_));
                if (args_.verbose) {
                    std::cout << "[AUTO-TUNE] " << losses_to_pause << "+ consecutive losses - TRADING PAUSED\n";
                }
            }
        } else if (consecutive_losses_ >= losses_to_defensive) {
            // 4 losses: min_trade_value +50%
            double new_min = base_min_trade_value_ * AutoTuneMultipliers::TIGHTEN_FACTOR;
            if (shared_config_->min_trade_value() < new_min) {
                shared_config_->set_min_trade_value(new_min);
                publisher_.status(0, "ALL", StatusCode::AutoTuneMinTrade, new_min,
                                  static_cast<uint8_t>(consecutive_losses_));
                if (args_.verbose) {
                    std::cout << "[AUTO-TUNE] " << losses_to_defensive << " losses - min_trade_value -> $" << new_min
                              << "\n";
                }
            }
        } else if (consecutive_losses_ >= losses_to_tighten) {
            // 3 losses: signal_strength = Strong
            if (shared_config_->get_signal_strength() < 2) {
                shared_config_->set_signal_strength(2);
                publisher_.status(0, "ALL", StatusCode::AutoTuneSignal, 2, static_cast<uint8_t>(consecutive_losses_));
                if (args_.verbose) {
                    std::cout << "[AUTO-TUNE] " << losses_to_tighten << " losses - signal_strength -> Strong\n";
                }
            }
        } else if (consecutive_losses_ >= losses_to_cautious) {
            // 2 losses: cooldown +50%
            int32_t new_cooldown = static_cast<int32_t>(base_cooldown_ms_ * AutoTuneMultipliers::TIGHTEN_FACTOR);
            if (shared_config_->get_cooldown_ms() < new_cooldown) {
                shared_config_->set_cooldown_ms(new_cooldown);
                publisher_.status(0, "ALL", StatusCode::AutoTuneCooldown, new_cooldown,
                                  static_cast<uint8_t>(consecutive_losses_));
                if (args_.verbose) {
                    std::cout << "[AUTO-TUNE] " << losses_to_cautious << " losses - cooldown_ms -> " << new_cooldown
                              << "\n";
                }
            }
        }

        // ===== WIN STREAK: Relax parameters gradually =====
        const int wins_to_aggressive = shared_config_->get_wins_to_aggressive();

        if (consecutive_wins_ >= wins_to_aggressive) {
            bool relaxed = false;

            // Re-enable trading if it was paused
            if (!shared_config_->trading_enabled.load()) {
                shared_config_->set_trading_enabled(true);
                relaxed = true;
                if (args_.verbose) {
                    std::cout << "[AUTO-TUNE] " << wins_to_aggressive << " wins - TRADING RE-ENABLED\n";
                }
            }

            // Relax min_trade_value back toward base
            double current_min = shared_config_->min_trade_value();
            if (current_min > base_min_trade_value_) {
                double new_min = std::max(base_min_trade_value_, current_min * AutoTuneMultipliers::RELAX_FACTOR);
                shared_config_->set_min_trade_value(new_min);
                relaxed = true;
                if (args_.verbose) {
                    std::cout << "[AUTO-TUNE] " << wins_to_aggressive << " wins - min_trade_value -> $" << new_min
                              << "\n";
                }
            }

            // Relax cooldown back toward base
            int32_t current_cooldown = shared_config_->get_cooldown_ms();
            if (current_cooldown > base_cooldown_ms_) {
                int32_t new_cooldown = std::max(
                    base_cooldown_ms_, static_cast<int32_t>(current_cooldown * AutoTuneMultipliers::RELAX_FACTOR));
                shared_config_->set_cooldown_ms(new_cooldown);
                relaxed = true;
                if (args_.verbose) {
                    std::cout << "[AUTO-TUNE] " << wins_to_aggressive << " wins - cooldown_ms -> " << new_cooldown
                              << "\n";
                }
            }

            // Publish relaxed event once if anything changed
            if (relaxed) {
                publisher_.status(0, "ALL", StatusCode::AutoTuneRelaxed, 0, static_cast<uint8_t>(consecutive_wins_));
            }

            // Note: signal_strength stays at Strong (conservative)
            // User can manually lower it if desired
        }
    }

    /**
     * Get order type preference from shared config
     *
     * @return true for market order, false for limit order
     */
    bool should_use_market_order() const {
        if (!shared_config_)
            return true; // Default to market

        uint8_t pref = shared_config_->get_order_type_default();
        switch (pref) {
        case 1: // MarketOnly
            return true;
        case 2: // LimitOnly
        case 3: // Adaptive (start with limit)
            return false;
        default: // Auto (0) - default to market for now
            return true;
        }
    }

    /**
     * Calculate limit price for buy order based on config
     *
     * Places limit slightly inside the spread to increase fill probability
     * while still getting better price than market order.
     *
     * @param bid Current best bid
     * @param ask Current best ask
     * @return Limit price for buy order
     */
    Price calculate_buy_limit_price(Price bid, Price ask) const {
        // Get offset from config (default 2 bps = 0.02%)
        double offset_bps = 2.0;
        if (shared_config_) {
            offset_bps = shared_config_->get_limit_offset_bps();
        }

        // Place limit above bid but below ask
        // offset_bps = how far from bid toward ask
        double spread = static_cast<double>(ask - bid);
        double offset = spread * (offset_bps / 100.0);

        // Limit = bid + offset (closer to ask = more aggressive fill)
        return bid + static_cast<Price>(offset);
    }

    /**
     * Calculate limit price for sell order based on config
     *
     * @param bid Current best bid
     * @param ask Current best ask
     * @return Limit price for sell order
     */
    Price calculate_sell_limit_price(Price bid, Price ask) const {
        double offset_bps = 2.0;
        if (shared_config_) {
            offset_bps = shared_config_->get_limit_offset_bps();
        }

        double spread = static_cast<double>(ask - bid);
        double offset = spread * (offset_bps / 100.0);

        // Limit = ask - offset (closer to bid = more aggressive fill)
        return ask - static_cast<Price>(offset);
    }

    /**
     * Register all available strategies with the unified selector
     *
     * Strategies available:
     * - TechnicalIndicatorsStrategy: RSI + EMA crossover + Bollinger Bands
     * - MarketMakerStrategy: Two-sided quoting with inventory skew
     */
    void register_strategies() {
        // TechnicalIndicatorsStrategy config
        TechnicalIndicatorsStrategy::Config ti_config;
        ti_config.base_position_pct = portfolio_.base_position_pct();
        ti_config.max_position_pct = portfolio_.max_position_pct();
        ti_config.price_scale = risk::PRICE_SCALE;

        auto ti_strategy = std::make_unique<TechnicalIndicatorsStrategy>(ti_config);
        strategy_selector_.register_default(std::move(ti_strategy));

        // MarketMakerStrategy config
        MarketMakerStrategy::Config mm_config;
        mm_config.price_scale = risk::PRICE_SCALE;
        mm_config.min_spread_bps = 5.0;      // Don't quote if spread < 5 bps
        mm_config.mm_config.spread_bps = 10; // 10 bps spread
        mm_config.mm_config.max_position = args_.max_position;

        auto mm_strategy = std::make_unique<MarketMakerStrategy>(mm_config);
        strategy_selector_.register_strategy(std::move(mm_strategy));

        // MomentumStrategy config
        MomentumStrategy::Config mom_config;
        mom_config.price_scale = risk::PRICE_SCALE;
        mom_config.base_position_pct = 0.15; // More aggressive for momentum
        mom_config.max_position_pct = 0.4;
        mom_config.roc_period = 10;
        mom_config.momentum_ema_period = 5;

        auto mom_strategy = std::make_unique<MomentumStrategy>(mom_config);
        strategy_selector_.register_strategy(std::move(mom_strategy));

        // FairValueStrategy config
        FairValueStrategy::Config fv_config;
        fv_config.price_scale = risk::PRICE_SCALE;
        fv_config.base_position_pct = 0.1; // Conservative for mean reversion
        fv_config.max_position_pct = 0.25;
        fv_config.fair_value_period = 20;
        fv_config.std_dev_period = 20;

        auto fv_strategy = std::make_unique<FairValueStrategy>(fv_config);
        strategy_selector_.register_strategy(std::move(fv_strategy));

        std::cout << "[STRATEGY] Registered " << strategy_selector_.count() << " strategies: ";
        for (auto name : strategy_selector_.strategy_names()) {
            std::cout << name << " ";
        }
        std::cout << "\n";
    }

    void record_win() {
        consecutive_wins_++;
        consecutive_losses_ = 0;
        update_active_mode();
    }

    void record_loss() {
        consecutive_losses_++;
        consecutive_wins_ = 0;
        update_active_mode();
    }

    /**
     * Emergency liquidation - sell all positions at market price
     * Called when market-wide crash is detected
     */
    void emergency_liquidate(Price current_bid) {
        std::cout << "\n[EMERGENCY] MARKET CRASH DETECTED - Liquidating all positions!\n";
        std::cout << "[EMERGENCY] Spike ratio: " << std::fixed << std::setprecision(1)
                  << (market_health_.spike_ratio() * 100) << "% of symbols spiking\n";

        int liquidated = 0;
        double total_value = 0;
        double total_pnl = 0;

        for (size_t s = 0; s < MAX_SYMBOLS; s++) {
            if (!portfolio_.symbol_active[s])
                continue;

            double qty = portfolio_.positions[s].total_quantity();
            if (qty <= 0)
                continue;

            auto* world = engine_.get_symbol_world(static_cast<Symbol>(s));
            if (!world)
                continue;

            // Get current bid for this symbol
            double bid_usd = world->best_bid() > 0 ? static_cast<double>(world->best_bid()) / risk::PRICE_SCALE
                                                   : static_cast<double>(current_bid) / risk::PRICE_SCALE;

            double entry = portfolio_.avg_entry_price(static_cast<Symbol>(s));
            double pnl = (bid_usd - entry) * qty;
            double value = bid_usd * qty;

            // Execute market sell
            // NOTE: sell() returns actual_commission which may be scaled if overselling protection triggered
            double trade_value = bid_usd * qty;
            double commission = trade_value * portfolio_.commission_rate();
            double actual_commission = portfolio_.sell(static_cast<Symbol>(s), bid_usd, qty, 0, commission);

            // Record exit in TradeRecorder (ledger + IPC)
            // IMPORTANT: Use actual_commission for accurate accounting
            trading::TradeInput exit_input{};
            exit_input.symbol = static_cast<uint32_t>(s);
            exit_input.price = bid_usd;
            exit_input.quantity = qty;
            exit_input.commission = actual_commission;
            std::strncpy(exit_input.ticker, strategies_[s].ticker, sizeof(exit_input.ticker) - 1);
            trade_recorder_.record_exit(trading::ExitReason::EMERGENCY, exit_input);

            // Update shared state
            if (portfolio_state_) {
                portfolio_state_->set_cash(portfolio_.cash);
                portfolio_state_->add_realized_pnl(pnl);
                portfolio_state_->add_commission(actual_commission); // Use actual commission
                portfolio_state_->add_volume(trade_value);
                portfolio_state_->record_stop(); // Count as emergency stop
                portfolio_state_->record_event();
                portfolio_state_->update_position(strategies_[s].ticker,
                                                  0, // Fully liquidated
                                                  0, bid_usd);
            }

            // Track
            if (pnl > 0)
                record_win();
            else
                record_loss();

            // Publish event
            publisher_.stop_loss(static_cast<Symbol>(s), strategies_[s].ticker, entry, bid_usd, qty);

            std::cout << "[EMERGENCY] SOLD " << strategies_[s].ticker << " qty=" << std::setprecision(4) << qty
                      << " @ $" << std::setprecision(2) << bid_usd << " P&L=$" << pnl << "\n";

            liquidated++;
            total_value += value;
            total_pnl += pnl;
        }

        std::cout << "[EMERGENCY] Liquidation complete: " << liquidated << " positions, "
                  << "$" << std::setprecision(2) << total_value << " value, "
                  << "$" << total_pnl << " P&L\n";
        std::cout << "[EMERGENCY] Cooldown active for " << market_health_.cooldown_remaining() << " ticks\n\n";
    }

    void on_fill(Symbol symbol, OrderId id, Side side, double qty, Price price) {
        auto* world = engine_.get_symbol_world(symbol);
        if (!world)
            return;

        double price_usd = static_cast<double>(price) / risk::PRICE_SCALE;
        double qty_d = qty; // Already double, no cast needed
        double trade_value = price_usd * qty_d;

        // Calculate spread cost (half spread paid per trade)
        // Spread cost = (ask - bid) / 2 * qty
        double bid_usd = world->best_bid() > 0 ? static_cast<double>(world->best_bid()) / risk::PRICE_SCALE : price_usd;
        double ask_usd = world->best_ask() > 0 ? static_cast<double>(world->best_ask()) / risk::PRICE_SCALE : price_usd;
        double spread = ask_usd - bid_usd;
        double spread_cost = (spread / 2.0) * qty_d; // Half spread per trade

        // For SELL fills, capture avg entry BEFORE the sell to calculate P&L
        double avg_entry_before_sell = 0.0;
        double qty_before_sell = 0.0;
        if (side == Side::Sell) {
            avg_entry_before_sell = portfolio_.positions[symbol].avg_entry();
            qty_before_sell = portfolio_.positions[symbol].total_quantity();
        }

        // Commission for this trade (calculate BEFORE updating portfolio)
        double commission = trade_value * portfolio_.commission_rate();

        // Update portfolio (spot trading: no leverage, no shorting)
        // NOTE: buy()/sell() return actual_commission which may be 0 if position limit reached or scaled
        double actual_commission = 0;
        if (side == Side::Buy) {
            // Release reserved cash (was reserved when order was sent)
            portfolio_.release_reserved_cash(price_usd * qty_d);
            actual_commission = portfolio_.buy(symbol, price_usd, qty_d, spread_cost, commission);
        } else {
            actual_commission = portfolio_.sell(symbol, price_usd, qty_d, spread_cost, commission);
        }

        // Update risk manager position tracking
        risk_manager_.on_fill(world->ticker(), side, static_cast<int64_t>(qty_d * 1e8), price);

        // Record in TradeRecorder (ledger + IPC)
        // IMPORTANT: Use actual_commission for accurate accounting
        trading::TradeInput fill_input{};
        fill_input.symbol = symbol;
        fill_input.price = price_usd;
        fill_input.quantity = qty_d;
        fill_input.commission = actual_commission;
        fill_input.spread_cost = spread_cost;
        std::strncpy(fill_input.ticker, world->ticker().c_str(), sizeof(fill_input.ticker) - 1);
        if (side == Side::Buy) {
            trade_recorder_.record_buy(fill_input);
        } else {
            trade_recorder_.record_sell(fill_input);
        }

        // Update shared portfolio state for dashboard (~5ns)
        if (portfolio_state_) {
            portfolio_state_->set_cash(portfolio_.cash);
            portfolio_state_->record_fill();
            portfolio_state_->record_event();

            // Track trading costs - use ACTUAL commission for accurate accounting
            portfolio_state_->add_commission(actual_commission);
            portfolio_state_->add_spread_cost(spread_cost);
            portfolio_state_->add_volume(trade_value);

            auto& pos = portfolio_.positions[symbol];
            portfolio_state_->update_position(world->ticker().c_str(), pos.total_quantity(), pos.avg_entry(),
                                              price_usd);

            if (side == Side::Buy) {
                portfolio_state_->record_buy(world->ticker().c_str());
            } else {
                portfolio_state_->record_sell(world->ticker().c_str());

                // Track realized P&L for SELL fills (BUG FIX: was missing before!)
                if (avg_entry_before_sell > 0 && qty_before_sell > 0) {
                    double realized_pnl = (price_usd - avg_entry_before_sell) * qty_d;
                    portfolio_state_->add_realized_pnl(realized_pnl);

                    // Update risk manager P&L for daily loss limit / drawdown tracking
                    int64_t total_pnl_scaled =
                        static_cast<int64_t>(portfolio_state_->total_realized_pnl() * risk::PRICE_SCALE);
                    risk_manager_.update_pnl(total_pnl_scaled);

                    // Update ConfigStrategy with trade result (for mode transitions)
                    bool was_win = (realized_pnl >= 0);
                    double pnl_pct = (realized_pnl / (avg_entry_before_sell * qty_d)) * 100.0;
                    if (config_strategies_[symbol]) {
                        config_strategies_[symbol]->record_trade_result(pnl_pct, was_win);
                    }
                }
            }
        }

        // Publish fill event to observer (~5ns, lock-free)
        publisher_.fill(symbol, world->ticker().c_str(), side == Side::Buy ? 0 : 1, price_usd, qty_d, id);

        // UDP telemetry for remote monitoring (~10µs, fire-and-forget)
        telemetry_.publish_fill(symbol, side == Side::Buy, static_cast<uint32_t>(qty),
                                static_cast<int64_t>(price_usd * 1e8));

        // Event log for tuner/web tracking
        if (event_log_) {
            TunerEvent e =
                TunerEvent::make_fill(world->ticker().c_str(), side == Side::Buy ? TradeSide::Buy : TradeSide::Sell,
                                      price_usd, qty_d, 0); // P&L calculated on position close
            event_log_->log(e);
        }

        // Debug: log fill details
        if (args_.verbose) {
            std::cout << "[FILL] " << world->ticker() << " " << (side == Side::Buy ? "BUY" : "SELL") << " "
                      << std::fixed << std::setprecision(6) << qty << " @ $" << std::setprecision(2) << price_usd
                      << " (cash=$" << portfolio_.cash << ")\n";
        }

        world->on_fill(side, qty, price);
        world->on_our_fill(id, qty);
    }

    /**
     * on_execution_report - Unified handler for all execution reports
     *
     * This is the single entry point for processing ExecutionReport messages
     * from any exchange (paper or production). Commission is included in the
     * report, not calculated here.
     *
     * @param report ExecutionReport from exchange (paper or real)
     */
    void on_execution_report(const ipc::ExecutionReport& report) {
        // Only process fills
        if (!report.is_fill())
            return;

        // Lookup symbol
        auto opt = engine_.lookup_symbol(report.symbol);
        if (!opt)
            return;

        Symbol symbol = *opt;
        if (symbol >= MAX_SYMBOLS)
            return;

        auto* world = engine_.get_symbol_world(symbol);
        if (!world)
            return;

        double price_usd = report.filled_price;
        double qty = report.filled_qty;
        double commission = report.commission; // From exchange, not calculated!
        double trade_value = price_usd * qty;

        // Calculate spread cost (half spread paid per trade)
        double bid_usd = world->best_bid() > 0 ? static_cast<double>(world->best_bid()) / risk::PRICE_SCALE : price_usd;
        double ask_usd = world->best_ask() > 0 ? static_cast<double>(world->best_ask()) / risk::PRICE_SCALE : price_usd;
        double spread = ask_usd - bid_usd;
        double spread_cost = (spread / 2.0) * qty;

        // Determine side
        bool is_buy = report.is_buy();

        // For SELL fills, capture avg entry BEFORE the sell to calculate P&L
        double avg_entry_before_sell = 0.0;
        double qty_before_sell = 0.0;
        if (!is_buy) {
            avg_entry_before_sell = portfolio_.positions[symbol].avg_entry();
            qty_before_sell = portfolio_.positions[symbol].total_quantity();
        }

        // Update portfolio with commission from report
        // NOTE: buy()/sell() return actual_commission which may be 0 if position limit reached or scaled
        double actual_commission = 0;
        if (is_buy) {
            portfolio_.release_reserved_cash(price_usd * qty);
            actual_commission = portfolio_.buy(symbol, price_usd, qty, spread_cost, commission);
        } else {
            actual_commission = portfolio_.sell(symbol, price_usd, qty, spread_cost, commission);
        }

        // Record in TradeRecorder (ledger + IPC)
        // IMPORTANT: Use actual_commission for accurate accounting
        trading::TradeInput exec_input{};
        exec_input.symbol = symbol;
        exec_input.price = price_usd;
        exec_input.quantity = qty;
        exec_input.commission = actual_commission;
        exec_input.spread_cost = spread_cost;
        std::strncpy(exec_input.ticker, report.symbol, sizeof(exec_input.ticker) - 1);
        if (is_buy) {
            trade_recorder_.record_buy(exec_input);
        } else {
            trade_recorder_.record_sell(exec_input);
        }

        // Update shared portfolio state for dashboard
        if (portfolio_state_) {
            portfolio_state_->set_cash(portfolio_.cash);
            portfolio_state_->record_fill();
            portfolio_state_->record_event();

            // Track trading costs - use ACTUAL commission for accurate accounting
            portfolio_state_->add_commission(actual_commission);
            portfolio_state_->add_spread_cost(spread_cost);
            portfolio_state_->add_volume(trade_value);

            auto& pos = portfolio_.positions[symbol];
            portfolio_state_->update_position(report.symbol, pos.total_quantity(), pos.avg_entry(), price_usd);

            if (is_buy) {
                portfolio_state_->record_buy(report.symbol);
            } else {
                portfolio_state_->record_sell(report.symbol);

                // Track win/loss for SELL fills (closing positions)
                if (avg_entry_before_sell > 0 && qty_before_sell > 0) {
                    double realized_pnl = (price_usd - avg_entry_before_sell) * qty;

                    // Track realized P&L (also increments winning_trades or losing_trades)
                    portfolio_state_->add_realized_pnl(realized_pnl);

                    // Update ConfigStrategy with trade result (for mode transitions)
                    bool was_win = (realized_pnl >= 0);
                    double pnl_pct = (realized_pnl / (avg_entry_before_sell * qty)) * 100.0;
                    if (config_strategies_[symbol]) {
                        config_strategies_[symbol]->record_trade_result(pnl_pct, was_win);
                    }

                    if (realized_pnl >= 0) {
                        portfolio_state_->record_target();
                        // Publish target_hit event to observer
                        publisher_.target_hit(symbol, report.symbol, avg_entry_before_sell, price_usd, qty);
                    } else {
                        portfolio_state_->record_stop();
                        // Publish stop_loss event to observer
                        publisher_.stop_loss(symbol, report.symbol, avg_entry_before_sell, price_usd, qty);
                    }
                }
            }
        }

        // Publish fill event to observer
        publisher_.fill(symbol, report.symbol, is_buy ? 0 : 1, price_usd, qty, report.order_id);

        // UDP telemetry
        telemetry_.publish_fill(symbol, is_buy, static_cast<uint32_t>(qty), static_cast<int64_t>(price_usd * 1e8));

        // Debug output
        if (args_.verbose) {
            std::cout << "[EXEC] " << report.symbol << " " << (is_buy ? "BUY" : "SELL") << " " << qty << " @ $"
                      << std::fixed << std::setprecision(2) << price_usd << " (comm=$" << std::setprecision(4)
                      << commission << ", cash=$" << std::setprecision(2) << portfolio_.cash << ")\n";
        }

        // Update SymbolWorld state
        Side side = is_buy ? Side::Buy : Side::Sell;
        Price price_scaled = static_cast<Price>(price_usd * risk::PRICE_SCALE);
        Quantity qty_scaled = static_cast<Quantity>(qty);
        world->on_fill(side, qty_scaled, price_scaled);
        world->on_our_fill(report.order_id, qty_scaled);
    }

    // Note: check_targets_and_stops removed - now handled inline in on_quote

    /**
     * Generate and execute signal using unified strategy architecture
     *
     * This method demonstrates the full unified architecture workflow:
     * 1. Build MarketSnapshot from current market data
     * 2. Build StrategyPosition from portfolio state
     * 3. Select appropriate strategy based on market regime
     * 4. Generate signal using IStrategy::generate()
     * 5. Execute signal using ExecutionEngine
     *
     * Returns true if a signal was executed, false otherwise.
     */
    bool execute_unified_signal(Symbol id, SymbolWorld* world, SymbolStrategy* strat, Price bid, Price ask) {
        // Skip if execution engine not configured
        if (!paper_adapter_)
            return false;

        // 1. Build MarketSnapshot with real order book sizes
        MarketSnapshot market;
        market.bid = bid;
        market.ask = ask;
        market.bid_size = world->top().best_bid_size();
        market.ask_size = world->top().best_ask_size();
        market.last_trade = (bid + ask) / 2;
        market.timestamp_ns = std::chrono::steady_clock::now().time_since_epoch().count();

        if (!market.valid())
            return false;

        // 2. Build StrategyPosition from portfolio
        double holding = portfolio_.get_holding(id);
        double mid_usd = market.mid_usd(risk::PRICE_SCALE);

        StrategyPosition position;
        position.quantity = holding;
        position.avg_entry_price = portfolio_.avg_entry_price(id);
        position.unrealized_pnl = (mid_usd - position.avg_entry_price) * holding;
        position.realized_pnl = 0; // Would need to track per-symbol
        position.cash_available = portfolio_.cash - portfolio_.pending_cash;
        // max_position: Use config to determine sizing mode
        // Mode 0 (percentage-based): Use portfolio cash value for percentage calculations
        // Mode 1 (unit-based): Use max_position_units directly
        if (shared_config_ && shared_config_->is_unit_based_sizing()) {
            position.max_position = shared_config_->get_max_position_units();
        } else {
            position.max_position = portfolio_.cash; // Percentage-based (default)
        }

        // 3. Get current regime
        MarketRegime regime = strat->current_regime;

        // 4. Select strategy and generate signal based on TunerState
        Signal signal;
        const char* strategy_name = "Unknown";
        bool use_config_strategy =
            shared_config_ && (shared_config_->is_tuner_on() || shared_config_->is_tuner_paused());
        if (use_config_strategy) {
            // TunerState ON/PAUSED: Use ConfigStrategy (config-driven)
            auto& cfg_strat = config_strategies_[id];
            if (!cfg_strat || !cfg_strat->ready())
                return false;
            signal = cfg_strat->generate(id, market, position, regime);
            strategy_name = "Config";
        } else {
            // TunerState OFF: Use traditional regime-based strategy selection
            IStrategy* strategy = strategy_selector_.select_for_regime(regime);
            if (!strategy || !strategy->ready())
                return false;
            signal = strategy->generate(id, market, position, regime);
            strategy_name = strategy->name().data();
        }
        if (!signal.is_actionable())
            return false;

        // 4b. Check minimum trade value
        // IMPORTANT: Return true even on skip to trigger cooldown and prevent signal spam
        {
            double min_trade = shared_config_->min_trade_value(); // Default: $100
            double price_usd = signal.is_buy() ? market.ask_usd(risk::PRICE_SCALE) : market.bid_usd(risk::PRICE_SCALE);
            double order_value = signal.suggested_qty * price_usd;

            if (order_value < min_trade) {
                // Skip silently - signal was generated but order too small
                // Return true to trigger cooldown and prevent signal spam
                return true;
            }
        }

        // 5. Apply order type preference from config (overrides strategy default)
        if (shared_config_) {
            uint8_t pref = shared_config_->get_order_type_default();
            switch (pref) {
            case 1: // MarketOnly
                signal.order_pref = OrderPreference::Market;
                break;
            case 2: // LimitOnly
                signal.order_pref = OrderPreference::Limit;
                // Calculate limit price if not set
                if (signal.limit_price == 0) {
                    if (signal.is_buy()) {
                        signal.limit_price = calculate_buy_limit_price(bid, ask);
                    } else {
                        signal.limit_price = calculate_sell_limit_price(bid, ask);
                    }
                }
                break;
            case 3: // Adaptive (start with limit)
                signal.order_pref = OrderPreference::Limit;
                if (signal.limit_price == 0) {
                    if (signal.is_buy()) {
                        signal.limit_price = calculate_buy_limit_price(bid, ask);
                    } else {
                        signal.limit_price = calculate_sell_limit_price(bid, ask);
                    }
                }
                break;
            // case 0 (Auto): let ExecutionEngine decide based on signal/regime/spread
            default:
                break;
            }
        }

        // 5b. Risk manager pre-trade check
        {
            Side side = signal.is_buy() ? Side::Buy : Side::Sell;
            Quantity qty = static_cast<Quantity>(signal.suggested_qty);
            Price price = signal.is_buy() ? ask : bid;

            if (!risk_manager_.check_order(strat->ticker, side, qty, price)) {
                if (args_.verbose) {
                    std::cout << "[RISK] " << strat->ticker << " " << (signal.is_buy() ? "BUY" : "SELL")
                              << " BLOCKED - ";
                    if (risk_manager_.is_halted()) {
                        if (risk_manager_.is_daily_limit_breached()) {
                            std::cout << "daily loss limit breached\n";
                        } else if (risk_manager_.is_drawdown_breached()) {
                            std::cout << "max drawdown breached\n";
                        } else {
                            std::cout << "trading halted\n";
                        }
                    } else {
                        std::cout << "order size/position limit\n";
                    }
                }
                return false;
            }
        }

        // 6. Execute signal using ExecutionEngine
        // The engine decides limit vs market based on signal.order_pref, strength, regime, spread
        uint64_t order_id = execution_engine_.execute(id, signal, market, regime);

        if (order_id > 0) {
            // Reserve cash for buy orders
            if (signal.is_buy()) {
                double order_value = signal.suggested_qty * market.ask_usd(risk::PRICE_SCALE);
                portfolio_.reserve_cash(order_value);
            }

            if (args_.verbose) {
                std::cout << "[UNIFIED] " << strat->ticker << " " << signal_type_str(signal.type)
                          << " qty=" << signal.suggested_qty << " (strategy=" << strategy_name
                          << ", strength=" << signal_strength_str(signal.strength) << ", reason=" << signal.reason
                          << ")\n";
            }

            // Publish signal event
            publisher_.signal(id, strat->ticker, signal.is_buy() ? 0 : 1, static_cast<uint8_t>(signal.strength),
                              mid_usd);

            // Event log for tuner/web tracking
            if (event_log_) {
                TunerEvent e =
                    TunerEvent::make_signal(strat->ticker, signal.is_buy() ? TradeSide::Buy : TradeSide::Sell, mid_usd,
                                            signal.suggested_qty, signal.reason);
                event_log_->log(e);
            }

            return true;
        }

        return false;
    }

    void check_signal(Symbol id, SymbolWorld* world, SymbolStrategy* strat, Price bid, Price ask) {
        uint64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
        double mid_usd = static_cast<double>((bid + ask) / 2) / risk::PRICE_SCALE;

        // Cooldown from config (default 2000ms = 2 billion ns)
        int64_t cooldown_ns = (shared_config_ ? shared_config_->get_cooldown_ms() : 2000) * 1'000'000LL;
        if (now - strat->last_signal_time < static_cast<uint64_t>(cooldown_ns)) {
            return; // Silent cooldown - no status spam
        }

        Price mid = (bid + ask) / 2;
        if (strat->last_mid == 0) {
            strat->last_mid = mid;
            return;
        }

        strat->last_mid = mid;

        // Wait for indicators to have enough data
        if (!strat->indicators.ready()) {
            // Publish warmup status occasionally
            static uint32_t warmup_counter = 0;
            if (++warmup_counter % 100 == 0) {
                publisher_.status(id, strat->ticker, StatusCode::IndicatorsWarmup, mid_usd);
            }
            return;
        }

        double ask_usd = static_cast<double>(ask) / risk::PRICE_SCALE;
        double bid_usd = static_cast<double>(bid) / risk::PRICE_SCALE;
        double holding = portfolio_.get_holding(id);

        // =====================================================================
        // TREND-BASED EXIT: Sell when trend reverses (don't wait for target)
        // =====================================================================
        // IMPORTANT: Skip when tuner_mode is ON - unified system handles ALL exits via exchange
        // to prevent double-counting (this code directly updates portfolio, unified uses callbacks)
        bool use_legacy_exits =
            !shared_config_ || !(shared_config_->is_tuner_on() || shared_config_->is_tuner_paused());
        if (use_legacy_exits && holding > 0) {
            bool should_exit = false;
            const char* exit_reason = "";

            auto sell_strength = strat->indicators.sell_signal();
            using SS = TechnicalIndicators::SignalStrength;

            // Exit condition 1: Trend turned down
            if (strat->current_regime == MarketRegime::TrendingDown) {
                should_exit = true;
                exit_reason = "TREND_DOWN";
            }
            // Exit condition 2: Strong sell signal in any regime
            else if (sell_strength >= SS::Strong) {
                should_exit = true;
                exit_reason = "STRONG_SELL";
            }
            // Exit condition 3: Medium sell signal + high volatility (risky)
            else if (sell_strength >= SS::Medium && strat->current_regime == MarketRegime::HighVolatility) {
                should_exit = true;
                exit_reason = "VOLATILE_SELL";
            }

            if (should_exit && world->can_trade(Side::Sell, 1)) {
                // Market sell entire position
                // NOTE: sell() returns actual_commission which may be scaled if overselling protection triggered
                double qty = holding;
                double entry = portfolio_.avg_entry_price(id);
                double pnl = (bid_usd - entry) * qty;
                double trade_value = bid_usd * qty;
                double commission = trade_value * portfolio_.commission_rate();

                double actual_commission = portfolio_.sell(id, bid_usd, qty, 0, commission);

                // Record exit in TradeRecorder (ledger + IPC)
                // IMPORTANT: Use actual_commission for accurate accounting
                trading::TradeInput exit_input{};
                exit_input.symbol = id;
                exit_input.price = bid_usd;
                exit_input.quantity = qty;
                exit_input.commission = actual_commission;
                std::strncpy(exit_input.ticker, strat->ticker, sizeof(exit_input.ticker) - 1);
                trade_recorder_.record_exit(trading::ExitReason::SIGNAL, exit_input);

                // Update shared portfolio state
                if (portfolio_state_) {
                    portfolio_state_->set_cash(portfolio_.cash);
                    portfolio_state_->add_realized_pnl(pnl);
                    portfolio_state_->add_commission(actual_commission); // Use actual commission
                    portfolio_state_->add_volume(trade_value);           // BUG FIX: was missing!
                    if (pnl > 0) {
                        portfolio_state_->record_target(); // Count as win
                    } else {
                        portfolio_state_->record_stop(); // Count as loss
                    }
                    portfolio_state_->record_event();
                    // Read actual position state (may not be fully closed)
                    auto& pos = portfolio_.positions[id];
                    portfolio_state_->update_position(strat->ticker, pos.total_quantity(), pos.avg_entry(), bid_usd);
                }

                // Track win/loss
                if (pnl > 0)
                    record_win();
                else
                    record_loss();

                // Publish to observer (use target/stop event, NOT fill - to avoid double counting)
                if (pnl > 0) {
                    publisher_.target_hit(id, strat->ticker, entry, bid_usd, qty);
                } else {
                    publisher_.stop_loss(id, strat->ticker, entry, bid_usd, qty);
                }

                if (args_.verbose) {
                    std::cout << "[EXIT:" << exit_reason << "] " << strat->ticker << " SELL " << std::fixed
                              << std::setprecision(4) << qty << " @ $" << std::setprecision(2) << bid_usd << " (entry=$"
                              << entry << ", P&L=$" << std::setprecision(2) << pnl << ")\n";
                }

                strat->last_signal_time = now;
                return; // Don't check buy after selling
            }
        }

        // =====================================================================
        // BUY LOGIC: Buy based on regime + indicators
        // =====================================================================

        // Option 1: Use unified strategy architecture (--unified flag OR tuner_mode ON)
        // When tuner_mode is ON, we use the unified architecture with AI-tuned parameters
        bool use_unified = args_.unified_strategy ||
                           (shared_config_ && (shared_config_->is_tuner_on() || shared_config_->is_tuner_paused()));

        if (use_unified) {
            if (execute_unified_signal(id, world, strat, bid, ask)) {
                strat->last_signal_time = now;
            }
            return; // Skip legacy logic when unified mode is enabled
        }

        // Option 2: Legacy direct indicator logic (default)
        bool should_buy = false;

        auto buy_strength = strat->indicators.buy_signal();
        using SS = TechnicalIndicators::SignalStrength;

        // Get minimum signal strength from config (1=Medium, 2=Strong)
        int min_strength = shared_config_ ? shared_config_->get_signal_strength() : 2;
        SS required_strength = (min_strength >= 2) ? SS::Strong : SS::Medium;

        switch (strat->current_regime) {
        case MarketRegime::TrendingUp:
            // Uptrend: Buy based on configured signal strength
            if (buy_strength >= required_strength && holding < args_.max_position) {
                should_buy = true;
            }
            break;

        case MarketRegime::TrendingDown:
            // Downtrend: DON'T BUY! Stop-loss will handle exits
            // Just wait for trend reversal
            break;

        case MarketRegime::Ranging:
        case MarketRegime::LowVolatility:
            // Mean reversion: Buy on dips based on configured strength
            if (buy_strength >= required_strength && holding < args_.max_position) {
                should_buy = true;
            }
            break;

        case MarketRegime::HighVolatility:
            // High vol: Always require Strong signals (regardless of config)
            if (buy_strength >= SS::Strong && holding < args_.max_position) {
                should_buy = true;
            }
            break;

        default:
            break;
        }

        // Price check: Only buy if price is reasonably close to EMA
        // Relaxed filter - crypto markets trend up, strict filter blocks too much
        double ema = strat->indicators.ema_slow();
        if (should_buy && ema > 0) {
            double deviation = (ask_usd - ema) / ema;
            // Allow buying above EMA in uptrends, more restrictive in other regimes
            // Get EMA deviation thresholds from config (or use defaults)
            double dev_trending = shared_config_ ? shared_config_->ema_dev_trending() : EmaThresholds::TRENDING_UP;
            double dev_ranging = shared_config_ ? shared_config_->ema_dev_ranging() : EmaThresholds::RANGING;
            double dev_highvol = shared_config_ ? shared_config_->ema_dev_highvol() : EmaThresholds::HIGH_VOL;

            double max_deviation;
            switch (strat->current_regime) {
            case MarketRegime::TrendingUp:
                max_deviation = dev_trending;
                break;
            case MarketRegime::Ranging:
            case MarketRegime::LowVolatility:
                max_deviation = dev_ranging;
                break;
            case MarketRegime::HighVolatility:
                max_deviation = dev_highvol;
                break;
            default:
                max_deviation = dev_ranging; // Default to ranging threshold
            }
            if (deviation > max_deviation) {
                should_buy = false; // Price too high relative to EMA
            }
        }

        // Calculate position size based on config
        // Uses symbol-specific position sizing if tuner has set it
        double available_cash = portfolio_.cash - portfolio_.pending_cash;
        double qty = portfolio_.calculate_qty(ask_usd, available_cash, strat->ticker);

        // Portfolio constraint - need enough cash for calculated qty
        if (should_buy && (qty <= 0 || !portfolio_.can_buy(ask_usd, qty))) {
            should_buy = false;
            // Rate limited: ~once per minute at 100 ticks/sec
            static uint32_t cash_low_counter = 0;
            if (++cash_low_counter % 5000 == 0) {
                publisher_.status(id, strat->ticker, StatusCode::CashLow, ask_usd, static_cast<uint8_t>(buy_strength),
                                  static_cast<uint8_t>(strat->current_regime));
            }
        }

        auto signal_str = [](TechnicalIndicators::SignalStrength s) {
            switch (s) {
            case SS::Strong:
                return "STRONG";
            case SS::Medium:
                return "MEDIUM";
            case SS::Weak:
                return "WEAK";
            default:
                return "NONE";
            }
        };

        // Execute buy if conditions met
        // Quantity is now double - supports fractional units (e.g., 0.021 BTC)
        double order_value = ask_usd * qty;
        double min_trade = shared_config_ ? shared_config_->min_trade_value() : 100.0;

        // Check minimum trade value to avoid overtrading with tiny positions
        if (should_buy && order_value < min_trade) {
            should_buy = false;
            strat->last_signal_time = now; // Trigger cooldown to prevent signal spam
        }

        // Check position capacity - avoid sending orders that will be rejected
        // BUG FIX: Previously orders were sent even when position limit was reached,
        // causing cash to get stuck (reserved but never deducted when buy() failed)
        if (should_buy && !portfolio_.can_add_position(id)) {
            should_buy = false; // Position limit reached for this symbol
        }

        if (should_buy && qty > 1e-8 && world->can_trade(Side::Buy, qty)) {
            portfolio_.reserve_cash(order_value);

            // Determine order type from config
            bool is_market = should_use_market_order();
            Price order_price;
            const char* order_type_str;

            if (is_market) {
                order_price = ask; // Market buy at ask
                order_type_str = "MKT";
            } else {
                order_price = calculate_buy_limit_price(bid, ask); // Limit inside spread
                order_type_str = "LMT";
            }

            if (args_.verbose) {
                double order_price_usd = static_cast<double>(order_price) / risk::PRICE_SCALE;
                std::cout << "[BUY:" << order_type_str << "] " << strat->ticker << " " << std::fixed
                          << std::setprecision(6) << qty << " @ $" << std::setprecision(2) << order_price_usd << " (=$"
                          << std::setprecision(2) << order_value << ", signal=" << signal_str(buy_strength)
                          << ", RSI=" << std::setprecision(0) << strat->indicators.rsi() << ", target=$"
                          << std::setprecision(2) << ask_usd * (1.0 + portfolio_.target_pct()) << ", stop=$"
                          << ask_usd * (1.0 - portfolio_.stop_pct()) << ")\n";
            }
            sender_.send_order(id, Side::Buy, qty, order_price, is_market);
            strat->last_signal_time = now;
        }
        // NOTE: Selling is handled by check_targets_and_stops(), not here!
    }
};

// Note: Dashboard removed - use trader_observer for real-time monitoring
// This keeps HFT process lean with zero display overhead

// ============================================================================
// Main
// ============================================================================

template <typename OrderSender>
int run(const CLIArgs& args) {
    // Pin to CPU core if requested (reduces latency variance)
    hft::util::set_cpu_affinity(args.cpu_affinity);

    std::cout << "\nHFT Trading System - " << (args.paper_mode ? "PAPER" : "PRODUCTION") << " MODE\n";
    std::cout << "================================================================\n\n";

    if (!args.paper_mode) {
        std::cout << "WARNING: Production mode - real orders will be sent!\n";
        std::cout << "Press Ctrl+C within 5 seconds to abort...\n\n";
        for (int i = 5; i > 0 && g_running; --i) {
            std::cout << "  " << i << "...\n";
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!g_running)
            return 0;
    }

    TradingApp<OrderSender> app(args);

    auto symbols = args.symbols.empty() ? fetch_default_symbols() : args.symbols;
    std::cout << "Registering " << symbols.size() << " symbols...\n";
    for (auto& s : symbols)
        app.add_symbol(s);

    BinanceWs ws(false);

    ws.set_connect_callback([](bool c) {
        if (c) {
            std::cout << "[OK] Connected to Binance\n\n";
            if (g_shared_config) {
                g_shared_config->set_ws_market_status(2); // healthy
                g_shared_config->update_ws_last_message();
            }
        } else {
            std::cout << "[DISCONNECTED] from Binance\n";
            if (g_shared_config) {
                g_shared_config->set_ws_market_status(0); // disconnected
            }
        }
    });

    ws.set_error_callback([](const std::string& err) { std::cerr << "[WS ERROR] " << err << "\n"; });

    // Enable auto-reconnect with status updates
    ws.enable_auto_reconnect(true);
    ws.set_reconnect_callback([](uint32_t retry_count, bool success) {
        if (success) {
            std::cout << "[RECONNECTED] After " << retry_count << " attempt(s)\n";
            if (g_shared_config) {
                g_shared_config->increment_ws_reconnect_count();
                g_shared_config->set_ws_market_status(2); // healthy
            }
        } else {
            std::cout << "[RECONNECTING] Attempt " << retry_count << "...\n";
            if (g_shared_config) {
                g_shared_config->set_ws_market_status(0); // disconnected during retry
            }
        }
    });

    ws.set_book_ticker_callback([&](const BookTicker& bt) {
        // Convert double quantities to scaled Quantity (uint32_t)
        Quantity bid_size = static_cast<Quantity>(bt.bid_qty * config::scaling::QUANTITY_SCALE);
        Quantity ask_size = static_cast<Quantity>(bt.ask_qty * config::scaling::QUANTITY_SCALE);
        app.on_quote(bt.symbol, bt.bid_price, bt.ask_price, bid_size, ask_size);
    });

    for (auto& s : symbols)
        ws.subscribe_book_ticker(s);

    std::cout << "Connecting...\n";
    if (!ws.connect()) {
        std::cerr << "Connection failed\n";
        return 1;
    }

    for (int i = 0; i < 50 && !ws.is_connected() && g_running; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!ws.is_connected()) {
        std::cerr << "Connection timeout\n";
        return 1;
    }

    // Mark as running now that we're connected
    if (g_shared_config) {
        g_shared_config->set_trader_status(2);    // running
        g_shared_config->set_trader_start_time(); // Record start time for restart detection
        g_shared_config->set_ws_market_status(2); // healthy - we just connected
        g_shared_config->update_ws_last_message();
        g_shared_config->update_heartbeat();
    }

    auto start = std::chrono::steady_clock::now();
    auto last_heartbeat = start;

    while (g_running) {
        auto elapsed =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();

        if (args.duration > 0 && elapsed >= args.duration)
            break;

        if (app.is_halted()) {
            std::cout << "\n  TRADING HALTED - Risk limit breached\n";
            break;
        }

        // Update heartbeat every second
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_heartbeat).count() >= 1) {
            if (g_shared_config) {
                g_shared_config->update_heartbeat();

                // Connection health monitoring with auto-recovery
                static int unhealthy_count = 0;
                constexpr int FORCE_RECONNECT_THRESHOLD = 30; // Force reconnect after 30s unhealthy

                if (!ws.is_connected()) {
                    g_shared_config->set_ws_market_status(0); // disconnected
                    unhealthy_count = 0;                      // Reset - already handling reconnection
                } else if (!ws.is_healthy(10)) {
                    g_shared_config->set_ws_market_status(1); // degraded - connected but no data
                    ++unhealthy_count;

                    // Force reconnect after prolonged unhealthy state
                    if (unhealthy_count >= FORCE_RECONNECT_THRESHOLD) {
                        std::cout << "[HEALTH] Connection unhealthy for " << unhealthy_count
                                  << "s, forcing reconnect...\n";
                        ws.force_reconnect();
                        unhealthy_count = 0;
                    }
                } else {
                    g_shared_config->set_ws_market_status(2); // healthy
                    g_shared_config->update_ws_last_message();
                    unhealthy_count = 0; // Reset on healthy
                }
            }
            app.publish_telemetry_heartbeat(); // UDP multicast heartbeat
            app.publish_heartbeat();           // IPC heartbeat for observer/dashboard
            last_heartbeat = now;
        }

        // No dashboard here - use trader_observer for real-time monitoring
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ws.disconnect();

    // Final summary
    auto stats = app.get_stats();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();

    std::cout << "\n[DONE] " << elapsed << "s | " << stats.ticks << " ticks | " << stats.fills << " fills | P&L: $"
              << std::fixed << std::setprecision(2) << (stats.pnl >= 0 ? "+" : "") << stats.pnl << "\n";

    return 0;
}

int main(int argc, char* argv[]) {
    hft::util::install_shutdown_handler(g_running, trader_pre_shutdown);

    CLIArgs args;
    if (!parse_args(argc, argv, args))
        return 1;

    if (args.help) {
        print_help();
        return 0;
    }

    if (args.paper_mode) {
        return run<PaperOrderSender>(args);
    } else {
        return run<ProductionOrderSender>(args);
    }
}
