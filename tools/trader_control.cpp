/**
 * HFT Control Tool
 *
 * Shared memory üzerinden config değiştirme aracı
 *
 * Kullanım:
 *   ./trader_control status                    # Tüm config'i göster
 *   ./trader_control list                      # Parametreleri listele
 *   ./trader_control get target_pct            # Tek değer oku
 *   ./trader_control set target_pct 3.0        # 3% profit target
 *   ./trader_control set stop_pct 1.0          # 1% stop loss
 *   ./trader_control set commission 0.1        # 0.1% commission
 *   ./trader_control disable                   # Trading kapat
 *   ./trader_control enable                    # Trading aç
 */

#include "../include/ipc/shared_config.hpp"
#include "../include/ipc/shared_paper_config.hpp"
#include <iostream>
#include <iomanip>
#include <string>
#include <cstring>
#include <chrono>

using namespace hft::ipc;

// Basis points to percentage conversion: 1 bps = 0.01% = 1/100 of a percent
static constexpr double BPS_TO_PCT = 100.0;
// Percentage to decimal conversion: 1% = 0.01
static constexpr double PCT_TO_DECIMAL = 100.0;

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <command> [args...]\n\n"
              << "Commands:\n"
              << "  status                 Show all config values\n"
              << "  list                   List all settable parameters\n"
              << "  get <param>            Get a specific value\n"
              << "  set <param> <value>    Set a specific value\n"
              << "  disable                Disable new trades\n"
              << "  enable                 Enable trading\n"
              << "\nTrading Cost Parameters:\n"
              << "  target_pct             Profit target (%, e.g., 3.0 = 3%)\n"
              << "  stop_pct               Stop loss (%, e.g., 1.0 = 1%)\n"
              << "  pullback_pct           Trend exit pullback (%, e.g., 0.5)\n"
              << "  commission             Commission rate (%, e.g., 0.1 = 0.1%)\n"
              << "  slippage_bps           Slippage simulation (bps, paper only, e.g., 5 = 0.05%)\n"
              << "\nTrade Filtering (anti-overtrading):\n"
              << "  min_trade_value        Minimum trade size ($, e.g., 100)\n"
              << "  cooldown_ms            Cooldown between trades (ms, e.g., 2000)\n"
              << "  signal_strength        Signal requirement (1=Medium, 2=Strong)\n"
              << "\nPosition Parameters:\n"
              << "  sizing_mode            Position sizing mode (0=Percentage, 1=Units)\n"
              << "  base_position_pct      Base position size (%)\n"
              << "  max_position_pct       Max position size (%)\n"
              << "  max_position_units     Max units when unit-based mode (default: 10)\n"
              << "\nRisk Parameters:\n"
              << "  drawdown_threshold     Max drawdown before defensive (%)\n"
              << "  loss_streak            Consecutive losses before cautious\n"
              << "\nSmartStrategy Streak Thresholds:\n"
              << "  losses_to_cautious     Consecutive losses -> CAUTIOUS mode (default: 2)\n"
              << "  losses_to_tighten      Consecutive losses -> require stronger signals (default: 3)\n"
              << "  losses_to_defensive    Consecutive losses -> DEFENSIVE mode (default: 4)\n"
              << "  losses_to_pause        Consecutive losses -> PAUSE trading (default: 5)\n"
              << "  losses_to_exit_only    Consecutive losses -> EXIT_ONLY mode (default: 6)\n"
              << "  wins_to_aggressive     Consecutive wins -> can be AGGRESSIVE (default: 3)\n"
              << "  wins_max_aggressive    Cap on aggression bonus (default: 5)\n"
              << "\nSmartStrategy Thresholds:\n"
              << "  min_confidence         Minimum confidence for signal (0-1, default: 0.3)\n"
              << "  min_position_pct       Minimum position size (%)\n"
              << "  min_risk_reward        Minimum risk/reward ratio (default: 0.6)\n"
              << "  drawdown_to_defensive  Drawdown % -> DEFENSIVE mode (default: 3%)\n"
              << "  drawdown_to_exit       Drawdown % -> EXIT_ONLY mode (default: 5%)\n"
              << "  win_rate_aggressive    Win rate to allow AGGRESSIVE (default: 0.6 = 60%)\n"
              << "  win_rate_cautious      Win rate below triggers CAUTIOUS (default: 0.4 = 40%)\n"
              << "  sharpe_aggressive      Sharpe ratio for AGGRESSIVE (default: 1.0)\n"
              << "  sharpe_cautious        Sharpe ratio below triggers CAUTIOUS (default: 0.3)\n"
              << "  sharpe_defensive       Sharpe ratio below triggers DEFENSIVE (default: 0.0)\n"
              << "  signal_aggressive      Signal threshold in AGGRESSIVE mode (default: 0.3)\n"
              << "  signal_normal          Signal threshold in NORMAL mode (default: 0.5)\n"
              << "  signal_cautious        Signal threshold in CAUTIOUS mode (default: 0.7)\n"
              << "\nEMA Filter (buy entry filter):\n"
              << "  ema_dev_trending       Max % above EMA in uptrend (e.g., 1.0 = 1%)\n"
              << "  ema_dev_ranging        Max % above EMA in ranging (e.g., 0.5 = 0.5%)\n"
              << "  ema_dev_highvol        Max % above EMA in high vol (e.g., 0.2 = 0.2%)\n"
              << "\nSpike Detection (regime detector):\n"
              << "  spike_threshold        Standard deviations for spike (e.g., 3.0 = 3σ)\n"
              << "  spike_lookback         Bars for average calculation (e.g., 10)\n"
              << "  spike_min_move         Minimum % move filter (e.g., 0.5 = 0.5%)\n"
              << "  spike_cooldown         Bars between detections (e.g., 5)\n"
              << "\nAI Tuner & Order Execution:\n"
              << "  tuner_mode             AI tuner mode (0=OFF, 1=ON unified strategy)\n"
              << "  order_type             Order type (0=Auto, 1=MarketOnly, 2=LimitOnly, 3=Adaptive)\n"
              << "  limit_offset_bps       Limit order offset inside spread (bps, e.g., 3.0)\n"
              << "  limit_timeout_ms       Adaptive mode: limit->market timeout (ms, e.g., 500)\n"
              << "\nRegime Strategy Mapping (regime=0-6, strategy=0-6):\n"
              << "  regime_strategy <regime> <strategy>  Set strategy for regime\n"
              << "  Regimes: 0=Unknown, 1=TrendingUp, 2=TrendingDown, 3=Ranging, 4=HighVol, 5=LowVol, 6=Spike\n"
              << "  Strategies: 0=NONE, 1=MOMENTUM, 2=MEAN_REV, 3=MKT_MAKER, 4=DEFENSIVE, 5=CAUTIOUS, 6=SMART\n"
              << "\nExamples:\n"
              << "  " << prog << " status\n"
              << "  " << prog << " set target_pct 3.0      # 3% profit target\n"
              << "  " << prog << " set commission 0.05    # 0.05% commission (5 bps)\n";
}

void print_params(const SharedConfig* config, const char* shm_name) {
    std::cout << "=== Settable Parameters ===\n";
    std::cout << "Config: /dev/shm" << shm_name << "\n\n";
    std::cout << std::fixed << std::setprecision(2);

    std::cout << "[ Trading Costs ]\n";
    std::cout << "  target_pct:        " << std::setw(8) << config->target_pct() << "%   Profit target\n";
    std::cout << "  stop_pct:          " << std::setw(8) << config->stop_pct() << "%   Stop loss\n";
    std::cout << "  pullback_pct:      " << std::setw(8) << config->pullback_pct() << "%   Trend exit pullback\n";
    std::cout << "  commission:        " << std::setw(8) << (config->commission_rate() * PCT_TO_DECIMAL) << "%   Commission rate\n";

    std::cout << "\n[ Trade Filtering ]\n";
    std::cout << "  min_trade_value:   " << std::setw(8) << config->min_trade_value() << "$   Minimum trade\n";
    std::cout << "  cooldown_ms:       " << std::setw(8) << config->get_cooldown_ms() << "ms  Trade cooldown\n";
    std::cout << "  signal_strength:   " << std::setw(8) << config->get_signal_strength() << "    (1=Med, 2=Strong)\n";
    std::cout << "  auto_tune:         " << std::setw(8) << (config->is_auto_tune_enabled() ? "ON" : "OFF") << "    Adaptive tuning\n";

    std::cout << "\n[ Position Sizing ]\n";
    std::cout << "  sizing_mode:       " << std::setw(8) << (config->is_percentage_based_sizing() ? "Percent" : "Units") << "    (0=%, 1=units)\n";
    std::cout << "  base_position_pct: " << std::setw(8) << config->base_position_pct() << "%   Base position\n";
    std::cout << "  max_position_pct:  " << std::setw(8) << config->max_position_pct() << "%   Max position\n";
    std::cout << "  max_position_units:" << std::setw(8) << config->get_max_position_units() << "    (unit mode only)\n";

    std::cout << "\n[ Risk Management ]\n";
    std::cout << "  drawdown_threshold:" << std::setw(8) << config->drawdown_threshold() << "%   Drawdown limit\n";
    std::cout << "  loss_streak:       " << std::setw(8) << config->loss_streak() << "    Losses before cautious\n";
    std::cout << "  spread_multiplier: " << std::setw(8) << config->spread_multiplier() << "x   Spread threshold\n";

    std::cout << "\n[ Status ]\n";
    std::cout << "  trading_enabled:   " << std::setw(8) << (config->trading_enabled.load() ? "YES" : "NO") << "\n";
    std::cout << "  sequence:          " << std::setw(8) << config->sequence.load() << "    Config version\n";
}

void print_status(const SharedConfig* config, const SharedPaperConfig* paper_config, const char* shm_name) {
    std::cout << "=== Trader Config Status ===\n";
    std::cout << "Config: /dev/shm" << shm_name << "\n";
    std::cout << "Build: " << config->get_build_hash() << "\n\n";
    std::cout << std::fixed << std::setprecision(2);

    // Trading status
    std::cout << "[ Status ]\n";
    std::cout << "  trading_enabled: " << (config->trading_enabled.load() ? "YES" : "NO") << "\n";
    std::cout << "  paper_trading:   " << (config->is_paper_trading() ? "YES (simulation)" : "NO (live)") << "\n";
    std::cout << "  trader_status:   " << (int)config->get_trader_status()
              << (config->is_trader_alive() ? " (alive)" : " (stale)") << "\n";
    std::cout << "  mode:            " << (int)config->get_active_mode() << "\n\n";

    // Trading costs
    double slippage_bps = paper_config ? paper_config->slippage_bps() : config->slippage_bps();
    std::cout << "[ Trading Costs ]\n";
    std::cout << "  target_pct:      " << config->target_pct() << "% (profit target)\n";
    std::cout << "  stop_pct:        " << config->stop_pct() << "% (stop loss)\n";
    std::cout << "  pullback_pct:    " << config->pullback_pct() << "% (trend exit)\n";
    std::cout << "  commission:      " << (config->commission_rate() * PCT_TO_DECIMAL) << "% (per trade)\n";
    std::cout << "  slippage_bps:    " << slippage_bps << " bps (paper only)\n\n";

    // Round-trip cost calculation
    double commission_pct = config->commission_rate() * 100;
    double slippage_pct = slippage_bps / BPS_TO_PCT;
    double round_trip = (commission_pct * 2) + (slippage_pct * 2);
    std::cout << "  Round-trip cost: ~" << round_trip << "% (commission + slippage)\n";
    std::cout << "  Breakeven:       target > " << round_trip << "%\n\n";

    // Trade filtering
    std::cout << "[ Trade Filtering ]\n";
    std::cout << "  min_trade_value: $" << config->min_trade_value() << "\n";
    std::cout << "  cooldown_ms:     " << config->get_cooldown_ms() << "ms\n";
    std::cout << "  signal_strength: " << config->get_signal_strength()
              << " (" << (config->get_signal_strength() >= 2 ? "Strong" : "Medium") << " required)\n";
    std::cout << "  auto_tune:       " << (config->is_auto_tune_enabled() ? "ON" : "OFF") << "\n\n";

    if (config->is_auto_tune_enabled()) {
        std::cout << "[ Auto-Tune Rules (configurable) ]\n";
        std::cout << "  " << config->get_losses_to_cautious() << " losses  -> cooldown +50%\n";
        std::cout << "  " << config->get_losses_to_tighten_signal() << " losses  -> signal_strength = Strong\n";
        std::cout << "  " << config->get_losses_to_defensive() << " losses  -> min_trade_value +50%\n";
        std::cout << "  " << config->get_losses_to_pause() << "+ losses -> TRADING PAUSED\n";
        std::cout << "  " << config->get_wins_to_aggressive() << " wins    -> gradually relax params\n\n";
    }

    // SmartStrategy thresholds
    std::cout << "[ SmartStrategy Thresholds ]\n";
    std::cout << "  min_confidence:      " << std::setprecision(2) << config->min_confidence() << " (min for signal)\n";
    std::cout << "  min_position_pct:    " << config->min_position_pct() << "% (min position)\n";
    std::cout << "  min_risk_reward:     " << config->min_risk_reward() << " (risk/reward ratio)\n";
    std::cout << "  drawdown_to_def:     " << (config->drawdown_to_defensive() * 100) << "% -> DEFENSIVE\n";
    std::cout << "  drawdown_to_exit:    " << (config->drawdown_to_exit() * 100) << "% -> EXIT_ONLY\n";
    std::cout << "  win_rate_aggressive: " << config->win_rate_aggressive() << " (>= for AGGRESSIVE)\n";
    std::cout << "  win_rate_cautious:   " << config->win_rate_cautious() << " (< for CAUTIOUS)\n";
    std::cout << "  sharpe_aggressive:   " << config->sharpe_aggressive() << " (>= for AGGRESSIVE)\n";
    std::cout << "  sharpe_cautious:     " << config->sharpe_cautious() << " (< for CAUTIOUS)\n";
    std::cout << "  sharpe_defensive:    " << config->sharpe_defensive() << " (< for DEFENSIVE)\n";
    std::cout << "  signal_aggressive:   " << config->signal_threshold_aggressive() << " (threshold in AGGRESSIVE)\n";
    std::cout << "  signal_normal:       " << config->signal_threshold_normal() << " (threshold in NORMAL)\n";
    std::cout << "  signal_cautious:     " << config->signal_threshold_cautious() << " (threshold in CAUTIOUS)\n\n";

    // Position sizing
    std::cout << "[ Position Sizing ]\n";
    std::cout << "  sizing_mode:     " << (config->is_percentage_based_sizing() ? "Percentage" : "Units") << "\n";
    std::cout << "  base_position:   " << config->base_position_pct() << "%\n";
    std::cout << "  max_position:    " << config->max_position_pct() << "%\n";
    std::cout << "  max_units:       " << config->get_max_position_units() << " (unit mode only)\n\n";

    // Risk
    std::cout << "[ Risk Management ]\n";
    std::cout << "  drawdown_limit:  " << config->drawdown_threshold() << "%\n";
    std::cout << "  loss_streak:     " << config->loss_streak() << " (before cautious)\n";
    std::cout << "  spread_mult:     " << config->spread_multiplier() << "x\n\n";

    // EMA deviation thresholds
    std::cout << "[ EMA Filter ]\n";
    std::cout << "  ema_dev_trending:   " << (config->ema_dev_trending() * PCT_TO_DECIMAL) << "% (uptrend)\n";
    std::cout << "  ema_dev_ranging:    " << (config->ema_dev_ranging() * PCT_TO_DECIMAL) << "% (ranging/lowvol)\n";
    std::cout << "  ema_dev_highvol:    " << (config->ema_dev_highvol() * PCT_TO_DECIMAL) << "% (high volatility)\n\n";

    // Spike detection thresholds
    std::cout << "[ Spike Detection ]\n";
    std::cout << "  spike_threshold:    " << config->spike_threshold() << "σ (standard deviations)\n";
    std::cout << "  spike_lookback:     " << config->get_spike_lookback() << " bars\n";
    std::cout << "  spike_min_move:     " << (config->spike_min_move() * PCT_TO_DECIMAL) << "% (minimum move)\n";
    std::cout << "  spike_cooldown:     " << config->get_spike_cooldown() << " bars\n\n";

    // AI Tuner & Order Type
    std::cout << "[ AI Tuner & Order Execution ]\n";
    std::cout << "  tuner_mode:       " << (config->is_tuner_mode() ? "ON (AI unified)" : "OFF (traditional)") << "\n";
    const char* order_type_names[] = {"Auto", "MarketOnly", "LimitOnly", "Adaptive"};
    uint8_t ot = config->get_order_type_default();
    std::cout << "  order_type:       " << (ot <= 3 ? order_type_names[ot] : "Unknown")
              << " (" << static_cast<int>(ot) << ")\n";
    std::cout << "  limit_offset_bps: " << config->get_limit_offset_bps() << " bps\n";
    std::cout << "  limit_timeout_ms: " << config->get_limit_timeout_ms() << " ms\n\n";

    // Regime → Strategy Mapping
    std::cout << "[ Regime Strategy Mapping ]\n";
    const char* regime_names[] = {"Unknown", "TrendingUp", "TrendingDown", "Ranging", "HighVol", "LowVol", "Spike"};
    const char* strategy_names[] = {"NONE", "MOMENTUM", "MEAN_REV", "MKT_MAKER", "DEFENSIVE", "CAUTIOUS", "SMART"};
    for (int i = 0; i <= 6; ++i) {
        uint8_t st = config->get_strategy_for_regime(i);
        std::cout << "  " << std::setw(12) << std::left << regime_names[i]
                  << " -> " << (st <= 6 ? strategy_names[st] : "?") << "\n";
    }
    std::cout << "\n";

    // Win/loss tracking
    std::cout << "[ Performance ]\n";
    std::cout << "  consecutive_wins:   " << config->get_consecutive_wins() << "\n";
    std::cout << "  consecutive_losses: " << config->get_consecutive_losses() << "\n\n";

    // WebSocket status
    std::cout << "[ WebSocket Status ]\n";
    const char* ws_status_names[] = {"DISCONNECTED", "DEGRADED", "HEALTHY"};
    uint8_t ws_status = config->get_ws_market_status();
    std::cout << "  ws_market_status:   " << (ws_status <= 2 ? ws_status_names[ws_status] : "UNKNOWN")
              << " (" << static_cast<int>(ws_status) << ")\n";
    std::cout << "  ws_reconnect_count: " << config->get_ws_reconnect_count() << "\n";

    // Calculate time since last message
    int64_t last_msg_ns = config->get_ws_last_message_ns();
    if (last_msg_ns > 0) {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        double secs_ago = static_cast<double>(now_ns - last_msg_ns) / 1'000'000'000.0;
        std::cout << "  last_message:       " << std::fixed << std::setprecision(1) << secs_ago << "s ago";
        if (secs_ago > 10.0) {
            std::cout << " [STALE!]";
        }
        std::cout << "\n";
    } else {
        std::cout << "  last_message:       no data yet\n";
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char* shm_name = "/trader_config";
    const char* paper_shm_name = "/trader_paper_config";
    std::string cmd = argv[1];

    // Open shared config (read-write)
    SharedConfig* config = SharedConfig::open_rw(shm_name);
    if (!config) {
        std::cerr << "Error: Cannot open shared config at /dev/shm" << shm_name << "\n";
        std::cerr << "Is the Trader application running?\n";
        return 1;
    }

    // Open paper config (optional, for slippage settings)
    SharedPaperConfig* paper_config = SharedPaperConfig::open_rw(paper_shm_name);

    // Commands
    if (cmd == "status") {
        print_status(config, paper_config, shm_name);
    }
    else if (cmd == "list") {
        print_params(config, shm_name);
    }
    else if (cmd == "get" && argc > 2) {
        std::string param = argv[2];
        std::cout << std::fixed << std::setprecision(4);

        if (param == "target_pct") {
            std::cout << config->target_pct() << "\n";
        } else if (param == "stop_pct") {
            std::cout << config->stop_pct() << "\n";
        } else if (param == "pullback_pct") {
            std::cout << config->pullback_pct() << "\n";
        } else if (param == "commission") {
            std::cout << (config->commission_rate() * PCT_TO_DECIMAL) << "\n";
        } else if (param == "slippage_bps" || param == "slippage") {
            if (paper_config) {
                std::cout << paper_config->slippage_bps() << "\n";
            } else {
                std::cout << config->slippage_bps() << " (fallback to SharedConfig)\n";
            }
        } else if (param == "min_trade_value") {
            std::cout << config->min_trade_value() << "\n";
        } else if (param == "cooldown_ms" || param == "cooldown") {
            std::cout << config->get_cooldown_ms() << "\n";
        } else if (param == "signal_strength") {
            std::cout << config->get_signal_strength() << "\n";
        } else if (param == "auto_tune") {
            std::cout << (config->is_auto_tune_enabled() ? "on" : "off") << "\n";
        } else if (param == "base_position_pct") {
            std::cout << config->base_position_pct() << "\n";
        } else if (param == "max_position_pct") {
            std::cout << config->max_position_pct() << "\n";
        } else if (param == "sizing_mode") {
            std::cout << (int)config->get_position_sizing_mode() << " ("
                      << (config->is_percentage_based_sizing() ? "Percentage" : "Units") << ")\n";
        } else if (param == "max_position_units") {
            std::cout << config->get_max_position_units() << "\n";
        } else if (param == "drawdown_threshold") {
            std::cout << config->drawdown_threshold() << "\n";
        } else if (param == "loss_streak") {
            std::cout << config->loss_streak() << "\n";
        } else if (param == "trading_enabled") {
            std::cout << (config->trading_enabled.load() ? "true" : "false") << "\n";
        } else if (param == "paper_trading") {
            std::cout << (config->is_paper_trading() ? "true" : "false") << "\n";
        } else if (param == "ema_dev_trending") {
            std::cout << (config->ema_dev_trending() * PCT_TO_DECIMAL) << "\n";
        } else if (param == "ema_dev_ranging") {
            std::cout << (config->ema_dev_ranging() * PCT_TO_DECIMAL) << "\n";
        } else if (param == "ema_dev_highvol") {
            std::cout << (config->ema_dev_highvol() * PCT_TO_DECIMAL) << "\n";
        } else if (param == "spike_threshold") {
            std::cout << config->spike_threshold() << "\n";
        } else if (param == "spike_lookback") {
            std::cout << config->get_spike_lookback() << "\n";
        } else if (param == "spike_min_move") {
            std::cout << (config->spike_min_move() * PCT_TO_DECIMAL) << "\n";
        } else if (param == "spike_cooldown") {
            std::cout << config->get_spike_cooldown() << "\n";
        } else if (param == "tuner_mode") {
            std::cout << (config->is_tuner_mode() ? "1" : "0") << "\n";
        } else if (param == "order_type") {
            std::cout << static_cast<int>(config->get_order_type_default()) << "\n";
        } else if (param == "limit_offset_bps") {
            std::cout << config->get_limit_offset_bps() << "\n";
        } else if (param == "limit_timeout_ms") {
            std::cout << config->get_limit_timeout_ms() << "\n";
        // SmartStrategy streak thresholds
        } else if (param == "losses_to_cautious") {
            std::cout << config->get_losses_to_cautious() << "\n";
        } else if (param == "losses_to_tighten") {
            std::cout << config->get_losses_to_tighten_signal() << "\n";
        } else if (param == "losses_to_defensive") {
            std::cout << config->get_losses_to_defensive() << "\n";
        } else if (param == "losses_to_pause") {
            std::cout << config->get_losses_to_pause() << "\n";
        } else if (param == "losses_to_exit_only") {
            std::cout << config->get_losses_to_exit_only() << "\n";
        } else if (param == "wins_to_aggressive") {
            std::cout << config->get_wins_to_aggressive() << "\n";
        } else if (param == "wins_max_aggressive") {
            std::cout << config->get_wins_max_aggressive() << "\n";
        // SmartStrategy thresholds
        } else if (param == "min_confidence") {
            std::cout << config->min_confidence() << "\n";
        } else if (param == "min_position_pct") {
            std::cout << config->min_position_pct() << "\n";
        } else if (param == "min_risk_reward") {
            std::cout << config->min_risk_reward() << "\n";
        } else if (param == "drawdown_to_defensive") {
            std::cout << (config->drawdown_to_defensive() * 100) << "\n";
        } else if (param == "drawdown_to_exit") {
            std::cout << (config->drawdown_to_exit() * 100) << "\n";
        } else if (param == "win_rate_aggressive") {
            std::cout << config->win_rate_aggressive() << "\n";
        } else if (param == "win_rate_cautious") {
            std::cout << config->win_rate_cautious() << "\n";
        } else if (param == "sharpe_aggressive") {
            std::cout << config->sharpe_aggressive() << "\n";
        } else if (param == "sharpe_cautious") {
            std::cout << config->sharpe_cautious() << "\n";
        } else if (param == "sharpe_defensive") {
            std::cout << config->sharpe_defensive() << "\n";
        } else if (param == "signal_aggressive") {
            std::cout << config->signal_threshold_aggressive() << "\n";
        } else if (param == "signal_normal") {
            std::cout << config->signal_threshold_normal() << "\n";
        } else if (param == "signal_cautious") {
            std::cout << config->signal_threshold_cautious() << "\n";
        } else {
            std::cerr << "Unknown parameter: " << param << "\n";
            munmap(config, sizeof(SharedConfig));
            return 1;
        }
    }
    else if (cmd == "set" && argc > 3) {
        std::string param = argv[2];
        double value = std::stod(argv[3]);

        if (param == "target_pct") {
            config->set_target_pct(value);
            std::cout << "target_pct = " << value << "% (profit target)\n";
        } else if (param == "stop_pct") {
            config->set_stop_pct(value);
            std::cout << "stop_pct = " << value << "% (stop loss)\n";
        } else if (param == "pullback_pct") {
            config->set_pullback_pct(value);
            std::cout << "pullback_pct = " << value << "% (trend exit)\n";
        } else if (param == "commission") {
            config->set_commission_rate(value / PCT_TO_DECIMAL);  // Convert % to decimal
            std::cout << "commission = " << value << "% (" << (value * 10) << " bps)\n";
        } else if (param == "slippage_bps" || param == "slippage") {
            if (paper_config) {
                paper_config->set_slippage_bps(value);
                std::cout << "slippage_bps = " << value << " bps (" << (value / BPS_TO_PCT) << "%, paper only)\n";
            } else {
                // Fallback to SharedConfig (deprecated)
                config->set_slippage_bps(value);
                std::cout << "slippage_bps = " << value << " bps (SharedConfig fallback, deprecated)\n";
            }
        } else if (param == "min_trade_value") {
            config->set_min_trade_value(value);
            std::cout << "min_trade_value = $" << value << " (minimum trade size)\n";
        } else if (param == "cooldown_ms" || param == "cooldown") {
            config->set_cooldown_ms(static_cast<int32_t>(value));
            std::cout << "cooldown_ms = " << static_cast<int>(value) << "ms\n";
        } else if (param == "signal_strength") {
            config->set_signal_strength(static_cast<int32_t>(value));
            std::cout << "signal_strength = " << static_cast<int>(value)
                      << " (" << (value >= 2 ? "Strong" : "Medium") << " signals required)\n";
        } else if (param == "auto_tune") {
            bool enabled = (value > 0);
            config->set_auto_tune_enabled(enabled);
            std::cout << "auto_tune = " << (enabled ? "ON" : "OFF") << "\n";
            if (enabled) {
                std::cout << "  (Adaptive parameter tuning based on win/loss streaks)\n";
            }
        } else if (param == "base_position_pct") {
            config->set_base_position_pct(value);
            std::cout << "base_position_pct = " << value << "%\n";
        } else if (param == "max_position_pct") {
            config->set_max_position_pct(value);
            std::cout << "max_position_pct = " << value << "%\n";
        } else if (param == "sizing_mode") {
            int mode = static_cast<int>(value);
            config->set_position_sizing_mode(static_cast<uint8_t>(mode));
            std::cout << "sizing_mode = " << mode << " (" << (mode == 0 ? "Percentage" : "Units") << ")\n";
        } else if (param == "max_position_units") {
            config->set_max_position_units(static_cast<int32_t>(value));
            std::cout << "max_position_units = " << static_cast<int32_t>(value) << "\n";
        } else if (param == "drawdown_threshold") {
            config->set_drawdown_threshold(value);
            std::cout << "drawdown_threshold = " << value << "%\n";
        } else if (param == "loss_streak") {
            config->set_loss_streak(static_cast<int>(value));
            std::cout << "loss_streak = " << static_cast<int>(value) << "\n";
        } else if (param == "spread_multiplier") {
            config->set_spread_multiplier(value);
            std::cout << "spread_multiplier = " << value << "x\n";
        } else if (param == "paper_trading") {
            bool enabled = (value > 0);
            config->set_paper_trading(enabled);
            std::cout << "paper_trading = " << (enabled ? "ON (simulation)" : "OFF (live)") << "\n";
            if (!enabled) {
                std::cout << "  WARNING: Commission/slippage settings ignored in live mode\n";
            }
        } else if (param == "ema_dev_trending") {
            config->set_ema_dev_trending(value);
            std::cout << "ema_dev_trending = " << value << "% (max above EMA in uptrend)\n";
        } else if (param == "ema_dev_ranging") {
            config->set_ema_dev_ranging(value);
            std::cout << "ema_dev_ranging = " << value << "% (max above EMA in ranging)\n";
        } else if (param == "ema_dev_highvol") {
            config->set_ema_dev_highvol(value);
            std::cout << "ema_dev_highvol = " << value << "% (max above EMA in high vol)\n";
        } else if (param == "spike_threshold") {
            config->set_spike_threshold(value);
            std::cout << "spike_threshold = " << value << "σ (standard deviations)\n";
        } else if (param == "spike_lookback") {
            config->set_spike_lookback(static_cast<int32_t>(value));
            std::cout << "spike_lookback = " << static_cast<int32_t>(value) << " bars\n";
        } else if (param == "spike_min_move") {
            config->set_spike_min_move(value / PCT_TO_DECIMAL);  // Convert % to decimal
            std::cout << "spike_min_move = " << value << "% (minimum move filter)\n";
        } else if (param == "spike_cooldown") {
            config->set_spike_cooldown(static_cast<int32_t>(value));
            std::cout << "spike_cooldown = " << static_cast<int32_t>(value) << " bars\n";
        } else if (param == "tuner_mode") {
            bool enabled = (value > 0);
            config->set_tuner_mode(enabled);
            std::cout << "tuner_mode = " << (enabled ? "ON (AI unified strategy)" : "OFF (traditional strategies)") << "\n";
        } else if (param == "order_type") {
            uint8_t type = static_cast<uint8_t>(value);
            config->set_order_type_default(type);
            const char* names[] = {"Auto", "MarketOnly", "LimitOnly", "Adaptive"};
            std::cout << "order_type = " << static_cast<int>(type)
                      << " (" << (type <= 3 ? names[type] : "Unknown") << ")\n";
        } else if (param == "limit_offset_bps") {
            config->set_limit_offset_bps(value);
            std::cout << "limit_offset_bps = " << value << " bps (limit order offset)\n";
        } else if (param == "limit_timeout_ms") {
            config->set_limit_timeout_ms(static_cast<int32_t>(value));
            std::cout << "limit_timeout_ms = " << static_cast<int>(value) << "ms (adaptive timeout)\n";
        // SmartStrategy streak thresholds
        } else if (param == "losses_to_cautious") {
            config->set_losses_to_cautious(static_cast<int32_t>(value));
            std::cout << "losses_to_cautious = " << static_cast<int>(value) << " (losses -> CAUTIOUS)\n";
        } else if (param == "losses_to_tighten") {
            config->set_losses_to_tighten_signal(static_cast<int32_t>(value));
            std::cout << "losses_to_tighten = " << static_cast<int>(value) << " (losses -> stronger signals)\n";
        } else if (param == "losses_to_defensive") {
            config->set_losses_to_defensive(static_cast<int32_t>(value));
            std::cout << "losses_to_defensive = " << static_cast<int>(value) << " (losses -> DEFENSIVE)\n";
        } else if (param == "losses_to_pause") {
            config->set_losses_to_pause(static_cast<int32_t>(value));
            std::cout << "losses_to_pause = " << static_cast<int>(value) << " (losses -> PAUSE trading)\n";
        } else if (param == "losses_to_exit_only") {
            config->set_losses_to_exit_only(static_cast<int32_t>(value));
            std::cout << "losses_to_exit_only = " << static_cast<int>(value) << " (losses -> EXIT_ONLY)\n";
        } else if (param == "wins_to_aggressive") {
            config->set_wins_to_aggressive(static_cast<int32_t>(value));
            std::cout << "wins_to_aggressive = " << static_cast<int>(value) << " (wins -> can be AGGRESSIVE)\n";
        } else if (param == "wins_max_aggressive") {
            config->set_wins_max_aggressive(static_cast<int32_t>(value));
            std::cout << "wins_max_aggressive = " << static_cast<int>(value) << " (cap on aggression)\n";
        // SmartStrategy thresholds
        } else if (param == "min_confidence") {
            config->set_min_confidence(value);
            std::cout << "min_confidence = " << value << " (minimum for signal)\n";
        } else if (param == "min_position_pct") {
            config->set_min_position_pct(value);
            std::cout << "min_position_pct = " << value << "% (minimum position)\n";
        } else if (param == "min_risk_reward") {
            config->set_min_risk_reward(value);
            std::cout << "min_risk_reward = " << value << " (risk/reward ratio)\n";
        } else if (param == "drawdown_to_defensive") {
            config->set_drawdown_to_defensive(value / 100.0);  // Convert % to decimal
            std::cout << "drawdown_to_defensive = " << value << "% -> DEFENSIVE mode\n";
        } else if (param == "drawdown_to_exit") {
            config->set_drawdown_to_exit(value / 100.0);  // Convert % to decimal
            std::cout << "drawdown_to_exit = " << value << "% -> EXIT_ONLY mode\n";
        } else if (param == "win_rate_aggressive") {
            config->set_win_rate_aggressive(value);
            std::cout << "win_rate_aggressive = " << value << " (>= for AGGRESSIVE)\n";
        } else if (param == "win_rate_cautious") {
            config->set_win_rate_cautious(value);
            std::cout << "win_rate_cautious = " << value << " (< for CAUTIOUS)\n";
        } else if (param == "sharpe_aggressive") {
            config->set_sharpe_aggressive(value);
            std::cout << "sharpe_aggressive = " << value << " (>= for AGGRESSIVE)\n";
        } else if (param == "sharpe_cautious") {
            config->set_sharpe_cautious(value);
            std::cout << "sharpe_cautious = " << value << " (< for CAUTIOUS)\n";
        } else if (param == "sharpe_defensive") {
            config->set_sharpe_defensive(value);
            std::cout << "sharpe_defensive = " << value << " (< for DEFENSIVE)\n";
        } else if (param == "signal_aggressive") {
            config->set_signal_aggressive(value);
            std::cout << "signal_aggressive = " << value << " (threshold in AGGRESSIVE)\n";
        } else if (param == "signal_normal") {
            config->set_signal_normal(value);
            std::cout << "signal_normal = " << value << " (threshold in NORMAL)\n";
        } else if (param == "signal_cautious") {
            config->set_signal_cautious(value);
            std::cout << "signal_cautious = " << value << " (threshold in CAUTIOUS)\n";
        } else {
            std::cerr << "Unknown parameter: " << param << "\n";
            munmap(config, sizeof(SharedConfig));
            return 1;
        }
        std::cout << "Config updated (sequence=" << config->sequence.load() << ")\n";
    }
    else if (cmd == "disable") {
        config->set_trading_enabled(false);
        std::cout << "Trading DISABLED\n";
    }
    else if (cmd == "enable") {
        config->set_trading_enabled(true);
        std::cout << "Trading enabled\n";
    }
    else if (cmd == "regime_strategy" && argc > 3) {
        int regime = std::stoi(argv[2]);
        int strategy = std::stoi(argv[3]);

        if (regime < 0 || regime > 6) {
            std::cerr << "Error: regime must be 0-6\n";
            std::cerr << "  0=Unknown, 1=TrendingUp, 2=TrendingDown, 3=Ranging, 4=HighVol, 5=LowVol, 6=Spike\n";
            munmap(config, sizeof(SharedConfig));
            return 1;
        }
        if (strategy < 0 || strategy > 6) {
            std::cerr << "Error: strategy must be 0-6\n";
            std::cerr << "  0=NONE, 1=MOMENTUM, 2=MEAN_REV, 3=MKT_MAKER, 4=DEFENSIVE, 5=CAUTIOUS, 6=SMART\n";
            munmap(config, sizeof(SharedConfig));
            return 1;
        }

        config->set_strategy_for_regime(regime, static_cast<uint8_t>(strategy));

        const char* regime_names[] = {"Unknown", "TrendingUp", "TrendingDown", "Ranging", "HighVol", "LowVol", "Spike"};
        const char* strategy_names[] = {"NONE", "MOMENTUM", "MEAN_REV", "MKT_MAKER", "DEFENSIVE", "CAUTIOUS", "SMART"};

        std::cout << regime_names[regime] << " -> " << strategy_names[strategy] << "\n";
        std::cout << "Regime strategy mapping updated (sequence=" << config->sequence.load() << ")\n";
    }
    else {
        print_usage(argv[0]);
        munmap(config, sizeof(SharedConfig));
        return 1;
    }

    munmap(config, sizeof(SharedConfig));
    return 0;
}
