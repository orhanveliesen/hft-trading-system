# HFT Trading Strategy Overview

## Strategy Type: Technical Indicators Based Mean-Reversion

The HFT system uses a **TechnicalIndicatorsStrategy** that combines multiple technical indicators for signal generation. It works best in **ranging/mean-reverting markets** and should avoid high volatility periods.

## Core Indicators Used

### 1. RSI (Relative Strength Index)
- Period: 14 (default)
- Oversold: RSI < 30 → BUY signal
- Overbought: RSI > 70 → SELL signal
- Best in ranging markets

### 2. EMA Crossover
- Fast EMA: 9 periods
- Slow EMA: 21 periods
- Fast > Slow → Bullish
- Fast < Slow → Bearish

### 3. Bollinger Bands
- Period: 20, StdDev: 2
- Price below lower band → BUY
- Price above upper band → SELL

## Signal Strength Levels

| Strength | Description | Order Type |
|----------|-------------|------------|
| Strong | Multiple indicators agree | MARKET order |
| Medium | Some indicators agree | LIMIT order |
| Weak | Mixed signals | LIMIT or skip |
| None | No clear signal | No action |

## When to Trade

### Good Conditions (Trade Aggressively)
- Market Regime: RANGING or LOW_VOL
- RSI showing clear oversold/overbought
- Price near Bollinger Band extremes
- Win rate > 50%

### Caution Conditions (Trade Conservatively)
- Market Regime: TRENDING (go with trend only)
- Mixed indicator signals
- Recent losses (consecutive_losses > 2)

### Avoid Trading
- Market Regime: HIGH_VOL or SPIKE
- RSI in neutral zone (40-60)
- After 3+ consecutive losses
- During major news events

## Key Parameters and Their Effects

### EMA Deviation Filter (ema_dev_trending, ema_dev_ranging)
**Purpose**: Prevents buying when price is too far above the 20-period EMA.

| Parameter | Effect of Increase | Effect of Decrease |
|-----------|-------------------|-------------------|
| ema_dev | More trades allowed | Fewer, safer trades |

**Recommended Values by Regime**:
- Trending Up: 1.0-2.0% (allow buying momentum)
- Ranging: 0.3-0.8% (tight filter for mean reversion)
- High Vol: 0.1-0.3% (very tight, avoid chasing)

### Position Size (base_position_pct)
**Purpose**: Controls how much capital to use per trade.

| Value | Risk Level | Use When |
|-------|------------|----------|
| 1.0-1.5% | Conservative | After losses, uncertain market |
| 2.0-2.5% | Moderate | Normal conditions, positive P&L |
| 3.0%+ | Aggressive | Strong conviction, winning streak |

### Cooldown (cooldown_ms)
**Purpose**: Minimum time between trades to prevent overtrading.

| Value | Description | Use When |
|-------|-------------|----------|
| 1000-2000ms | Fast | Strong trending market, high conviction |
| 2500-3500ms | Normal | Ranging market, moderate confidence |
| 4000-6000ms | Slow | After losses, choppy market |
| 8000ms+ | Very Slow | Recovery mode after drawdown |

### Target and Stop Loss (target_pct, stop_pct)
**Purpose**: Define profit target and maximum loss per trade.

**Recommended Ratios**:
- Minimum 2:1 reward/risk (target = 2x stop)
- Example: target=4%, stop=2%

| Market Condition | Target | Stop |
|-----------------|--------|------|
| Ranging | 2-3% | 1% |
| Trending | 4-6% | 1.5-2% |
| High Vol | 1.5-2% | 0.5-1% (tight) |

## Recovery from Losses

### After 2 Consecutive Losses
- Increase cooldown by 50%
- Keep other parameters same

### After 3 Consecutive Losses
- Require STRONG signals only (signal_strength = 2)
- Tighten EMA filter by 30%
- Reduce position size to minimum

### After 4+ Consecutive Losses
- Increase min_trade_value by 50%
- Consider pausing trading temporarily
- Wait for market regime change

### After 5+ Consecutive Losses
- PAUSE TRADING
- Wait for manual review or clear market shift
