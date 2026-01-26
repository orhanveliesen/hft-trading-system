# Trader AI Tuner Architecture

## Overview

The AI Tuner (`trader_tuner`) is an autonomous parameter optimization system that uses Claude AI to dynamically adjust trading parameters based on market conditions and performance metrics.

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                            TRADER SYSTEM                                     │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   ┌─────────────┐     Shared Memory      ┌─────────────┐                    │
│   │  Trader    │ ◄──────────────────► │   Tuner     │                    │
│   │   Engine    │   /trader_config       │   (AI)      │                    │
│   │             │   /trader_portfolio    │             │                    │
│   │  Binance WS │   /trader_events       │  Claude API │                    │
│   └──────┬──────┘   /trader_symbol_cfgs  └──────┬──────┘                    │
│          │                                       │                          │
│          │         ┌───────────────────┐        │                          │
│          └────────►│    Dashboard      │◄───────┘                          │
│                    │  (ImGui/Web)      │                                   │
│                    └───────────────────┘                                   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Data Flow

### 1. Market Data → Trader
- Binance WebSocket streams real-time price data
- Trader updates `SharedPortfolioState` with:
  - Current prices
  - Position P&L
  - Market snapshot (high/low/EMA/volatility)

### 2. Trader → Tuner (Read)
- Tuner reads from shared memory every interval (default: 300s)
- Data collected:
  - Current portfolio state
  - Per-symbol performance metrics
  - Market regime (trending/ranging/volatile)
  - Market snapshot (last 60s)

### 3. Tuner → Claude AI
- Tuner sends prompt with:
  - Current config values
  - Performance metrics (P&L, win rate, drawdown)
  - Market data (price, EMA, volatility, trend)
  - RAG context (strategy knowledge base)

### 4. Claude AI → Tuner (Response)
- Claude returns JSON with:
  - Action: UPDATE_CONFIG / PAUSE / RESUME / EMERGENCY_EXIT
  - Symbol-specific config changes
  - Confidence level (0-100%)
  - Reasoning

### 5. Tuner → Trader (Write)
- Tuner writes to shared memory:
  - `SharedConfig` - global parameters
  - `SharedSymbolConfigs` - per-symbol configs
- Trader picks up changes on next tick

## Configuration Parameters

### Parameters AI Can Tune

| Parameter | Range | Description |
|-----------|-------|-------------|
| `ema_dev_trending` | 0.1-2.0% | Max deviation from EMA in uptrend |
| `ema_dev_ranging` | 0.1-1.0% | Max deviation from EMA in range |
| `ema_dev_highvol` | 0.05-0.5% | Max deviation from EMA in high vol |
| `base_position_pct` | 1-5% | Base position size (% of portfolio) |
| `max_position_pct` | 1-10% | Max position size |
| `cooldown_ms` | 1000-10000 | Cooldown between trades |
| `signal_strength` | 1-3 | Required signal strength (1=weak, 3=strong) |
| `target_pct` | 1-10% | Take-profit target |
| `stop_pct` | 0.5-5% | Stop-loss threshold |
| `pullback_pct` | 0.2-2% | Trend exit pullback threshold |

### AI Decision Actions

| Action | Effect |
|--------|--------|
| `UPDATE_CONFIG` | Update parameters for symbol |
| `PAUSE` | Stop trading for symbol |
| `RESUME` | Resume trading for symbol |
| `EMERGENCY_EXIT` | Close all positions for symbol |
| `HOLD` | No changes (wait and observe) |

## AI Prompt Structure

```
MARKET DATA:
- Symbol: BTCUSDT
- Price: $104,500
- Regime: RANGING
- Volatility: 0.15%
- Trend: FLAT
- P&L: -$45.23
- Win Rate: 45%
- Trades: 12

CURRENT CONFIG:
- EMA Dev (Trending): 1.0%
- EMA Dev (Ranging): 0.5%
- Position Size: 2%
- Cooldown: 2000ms
- Target: 1.5%
- Stop: 1.0%

RAG CONTEXT:
[Relevant strategy documentation from knowledge base]

TASK:
Analyze market conditions and suggest parameter adjustments.
Return JSON with action, parameters, confidence, and reasoning.
```

## Event System

### TradeEvent (Dashboard)
- `TunerConfig` event type for AI decisions
- Status codes:
  - `TunerConfigUpdate` - Parameters updated
  - `TunerPauseSymbol` - Symbol paused
  - `TunerResumeSymbol` - Symbol resumed
  - `TunerEmergencyExit` - Emergency exit triggered

### TunerEvent (Event Log)
- More detailed logging for analysis
- Includes full config changes
- Used by `trader_events` CLI tool

## Dashboard Integration

### Tuner Mode Toggle
- Dashboard shows [AI TUNER] status when active
- Toggle in STRATEGY CONFIG panel
- Strategy column shows "SMART" when tuner is active

### Event Panel
- Purple events for AI decisions
- Shows confidence level
- Visible in LIVE EVENTS panel

## RAG Knowledge Base

Located in `knowledge/` directory:

| File | Content |
|------|---------|
| `market_regimes.md` | Regime detection logic |
| `parameter_tuning.md` | Parameter tuning guidelines |
| `strategy_overview.md` | Strategy descriptions |
| `trading_config_parameters.md` | Config documentation |
| `strategies/technical_indicators.md` | TI strategy details |
| `strategies/market_maker.md` | MM strategy details |
| `strategies/momentum.md` | Momentum strategy |
| `strategies/fair_value.md` | Fair value strategy |
| `risk_management.md` | Risk management rules |
| `market_microstructure.md` | Order book dynamics |

## Usage

### Start Tuner
```bash
# Basic usage (default 300s interval)
./trader_tuner

# Custom interval
./trader_tuner --interval 60

# Dry run (no changes applied)
./trader_tuner --dry-run

# Verbose logging
./trader_tuner --verbose
```

### Environment Variables
```bash
export CLAUDE_API_KEY="sk-ant-..."
export TRADER_TUNER_MODEL="claude-3-haiku-20240307"  # Optional, default: haiku
```

### Dashboard Control
- Enable/disable tuner from dashboard
- Manual Override disables tuner
- View AI decisions in event panel

## Logging

### Tuner Logs
Location: `logs/tuner_*.log`

Contains:
- AI decisions with timestamps
- Config changes applied
- API call statistics (latency, tokens)
- Error messages

### Structured Format
```
[2026-01-19 21:04:30.421] [TUNING] Starting (trigger: 1)...
[2026-01-19 21:04:30.450] [RAG] Using config symbol: BTCUSDT
[2026-01-19 21:04:30.610] [RAG] Retrieved 1220 bytes of context
[2026-01-19 21:04:31.021] [NEWS] Fetched 0 news items
[2026-01-19 21:04:31.021] [API] Calling Claude (claude-3-haiku-20240307)...

╔══════════════════════════════════════════════════════════════════════════════╗
║ TUNING DECISION @ 2026-01-19 21:04:34.842                                    ║
║ Action:     UPDATE_CONFIG                                                    ║
║ Symbol:     BTCUSDT                                                          ║
║ Confidence: 80%                                                              ║
║ API Stats:  HTTP 200 | Latency: 3821ms | Tokens: 1967/500                    ║
║ Status:     ✓ APPLIED                                                        ║
╚══════════════════════════════════════════════════════════════════════════════╝
```

## Cost Considerations

### Token Usage
- Input: ~1500-2000 tokens per call
- Output: ~400-600 tokens per call
- Cost: ~$0.001 per call with Haiku

### Rate Limiting
- Default interval: 300s (5 min)
- Can reduce to 10s for testing
- API latency: 2-4 seconds

## Troubleshooting

### Tuner Not Connecting
```bash
# Check shared memory files
ls -la /dev/shm/trader_*

# Restart Trader first, then tuner
./trader --paper -v &
./trader_tuner --verbose
```

### No Config Changes Applied
- Check Manual Override is OFF in dashboard
- Verify CLAUDE_API_KEY is set
- Check RAG service is running

### High Slippage
- Paper trading uses default 5 bps slippage
- Adjust in dashboard: Config → Paper Trading Costs
- This simulates realistic market conditions

## Best Practices

1. **Start with dry-run** to observe AI decisions
2. **Use longer intervals** (300s) for production
3. **Monitor dashboard** for AI events
4. **Review logs** after each session
5. **Tune RAG knowledge** based on market conditions
