# Adaptive Trading Configuration

## Philosophy

The trading system starts with **conservative defaults** and adapts to observed market conditions. The AI tuner monitors volatility, win rate, and other metrics to adjust parameters over time.

## Warmup Period Strategy

### Initial Configuration (First 20-50 trades)
During the warmup period, the system should:
1. Use wide stops (5%+) to minimize stop-outs
2. Use small position sizes (1-2% of capital)
3. Observe market volatility and price patterns
4. NOT make aggressive parameter changes

### Why Conservative Start?
- Unknown market conditions at startup
- Need data to understand volatility patterns
- Frequent stop-outs during learning = lost capital + bad data
- Better to miss some profit than lose capital learning

## Adaptive Parameter Guidelines

### When to WIDEN Stops
- Multiple stop losses hit in short period (>3 in 10 trades)
- High volatility detected (regime = HIGH_VOL)
- Win rate < 30% with current settings
- During news events or market uncertainty

### When to TIGHTEN Stops
- Market trending strongly in one direction
- Low volatility period (regime = LOW_VOL)
- High win rate (> 60%) sustained over 20+ trades
- After warmup period with stable performance

### Observation Windows
| Metric | Minimum Trades | Before Adjusting |
|--------|----------------|------------------|
| Stop loss % | 20 | Need enough data |
| Target % | 20 | Need enough data |
| Position size | 30 | Need confidence |
| Aggressiveness | 50 | Need track record |

## Market-Specific Considerations

The system doesn't assume a specific market type. Instead, it observes:

### Volatility Metrics
- Average price movement per timeframe
- Standard deviation of returns
- Regime detection (trending vs ranging)

### Adjust Stops Based on Observed Volatility
- If observed volatility > 3%: Keep stops at 5%+
- If observed volatility 1-3%: Stops at 3-5%
- If observed volatility < 1%: Stops at 2-3%

## Tuner Guidelines

### DO
- Wait for warmup period before major changes
- Make one parameter change at a time
- Observe results over 10-20 trades before next change
- Err on the side of caution

### DO NOT
- Tighten stops immediately at startup
- React to single trade outcomes
- Make multiple parameter changes simultaneously
- Assume market type without observing data

## Summary

**Key Principle**: Start conservative, observe, then adapt. It's better to have wider stops and miss some trades than to get stopped out constantly while learning market characteristics. The tuner should let the market data guide parameter adjustments, not assumptions about market type.
