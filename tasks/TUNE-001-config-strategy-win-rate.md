# TUNE-001: ConfigStrategy Low Win Rate

## Priority: MEDIUM

## Location
- `include/strategy/config_strategy.hpp`
- `include/config/defaults.hpp`

## Description
During 2-hour paper trading session, ConfigStrategy achieved only 22.2% win rate with 8 target hits vs 28 stop losses. This suggests suboptimal parameter configuration.

## Session Statistics
| Metric | Value |
|--------|-------|
| Total Fills | 540 |
| Target Hits | 8 |
| Stop Losses | 28 |
| Win Rate | 22.2% |
| Net P&L | -$239.07 |

## Current Parameters (defaults.hpp)
```cpp
TARGET_PCT = 0.015    // 1.5% profit target
STOP_PCT = 0.03       // 3% stop loss
```

## Potential Issues
1. **Asymmetric risk/reward**: Stop (3%) is 2x the target (1.5%), so need >66% win rate to break even
2. **Signal oscillation**: Technical indicators (EMA, RSI, BB) cause rapid signal changes leading to frequent position flips
3. **Regime misalignment**: Strategy may not be adapting well to current market conditions

## Suggested Investigations
1. Analyze trade-by-trade log to identify patterns in losing trades
2. Test with tighter stops (e.g., 1.5% stop, 1.5% target for 1:1 ratio)
3. Test with wider targets (e.g., 1.5% stop, 3% target)
4. Add minimum hold time to prevent rapid signal flips
5. Review signal strength thresholds - possibly require STRONG signals for entry

## Current Signal Weights (config_strategy.hpp)
```cpp
EMA_CROSSOVER_WEIGHT = 0.6
RSI_EXTREME_WEIGHT = 0.4
BB_OUTSIDE_WEIGHT = 0.3
OB_IMBALANCE_WEIGHT = 0.2
```

## Found
- Date: 2026-02-18
- Session: Paper trading 2h session
