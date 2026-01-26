# HFT Trading Configuration Parameters

## Critical Parameters That Control Trade Execution

### 1. Signal Strength (signal_strength)
- **Values**: 1 = Medium, 2 = Strong
- **Default**: 2 (Strong)
- **Effect**: Controls how strict signal requirements are
- **Problem**: If set to 2 (Strong), requires score >= 5 from indicators
  - EMA crossover: +2 points
  - RSI oversold: +2 points  
  - Below Bollinger band: +2 points
  - Max score: 6 points
- **Recommendation**: 
  - Set to 1 (Medium) for active trading (score >= 3)
  - Set to 2 (Strong) only in high-risk environments
- **Tuning Command**: `./hft_control set signal_strength 1`

### 2. Minimum Trade Value (min_trade_value)
- **Default**: $100
- **Effect**: Trades smaller than this value are rejected silently
- **Problem**: If position_pct is 1% and portfolio is $30,000:
  - Trade value = $300 (OK if min_trade_value = $100)
  - But if position_pct is 0.1%, trade = $30 (REJECTED!)
- **Auto-Tune**: After 4 consecutive losses, increases by 50%
- **Recommendation**: Keep at $10-$50 to allow small positions
- **Tuning Command**: `./hft_control set min_trade_value 10`

### 3. Cooldown (cooldown_ms)
- **Default**: 2000ms (2 seconds)
- **Effect**: Minimum time between trades on same symbol
- **Auto-Tune**: After 2 consecutive losses, increases by 50%
- **Recommendation**: 2000-5000ms depending on market conditions
- **Tuning Command**: `./hft_control set cooldown_ms 2000`

### 4. Slippage (slippage_bps)
- **Default**: 0 bps (paper mode)
- **Effect**: Simulates price slippage in paper trading
- **Live Impact**: 5 bps = 0.05% per trade
- **Recommendation**: 
  - Paper mode: 0-5 bps for realistic simulation
  - If set too high, all trades become unprofitable
- **Tuning Command**: `./hft_control set slippage_bps 5`

## Why Trades Might Not Execute

1. **No Strong Signals**: signal_strength=2 requires all indicators aligned
2. **Trade Too Small**: position_pct too low + high min_trade_value
3. **Cooldown Active**: Recent trade within cooldown period
4. **Max Position Reached**: Already at maximum position for symbol
5. **Trading Disabled**: Check trading_enabled status
6. **5+ Consecutive Losses**: Auto-tune pauses trading

## Recommended Configuration for Active Trading

```
signal_strength: 1      # Medium signals (more trades)
min_trade_value: $10    # Allow small positions
cooldown_ms: 2000       # 2 second cooldown
base_position_pct: 2%   # 2% per trade
slippage_bps: 5         # 5 bps for realistic simulation
```

## Auto-Tune Rules

| Losses | Action |
|--------|--------|
| 2 | cooldown +50% |
| 3 | signal_strength → Strong |
| 4 | min_trade_value +50% |
| 5+ | TRADING PAUSED |

After 3 consecutive wins, parameters gradually relax back to base values.
