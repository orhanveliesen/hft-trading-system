# Momentum Strategy

## Overview
The Momentum Strategy identifies and follows strong price trends, entering positions in the direction of momentum and exiting when momentum fades. It profits from trend continuation rather than mean reversion.

## Core Concept

```
Price moving up strongly -> Buy and ride the trend
Price moving down strongly -> Sell/short and ride the trend
Momentum fading -> Exit position
```

## Key Indicators

### Rate of Change (ROC)
- Measures percentage price change over N periods
- Formula: `ROC = (Price_now - Price_n) / Price_n * 100`
- Positive ROC = upward momentum
- Negative ROC = downward momentum

### Moving Average Momentum
- Price distance from moving average
- Formula: `Momentum = (Price - MA) / MA * 100`
- Large positive = strong upward momentum
- Large negative = strong downward momentum

### Volume Confirmation
- Strong momentum should have volume support
- Volume increasing with price = healthy trend
- Volume decreasing with price = weakening trend

## Signal Generation

### Entry Signals

**Long Entry:**
1. ROC > threshold (e.g., +2%)
2. Price above 20-period MA
3. Volume above average
4. No resistance nearby

**Short Entry:**
1. ROC < threshold (e.g., -2%)
2. Price below 20-period MA
3. Volume above average
4. No support nearby

### Exit Signals

**Exit Long:**
1. ROC turns negative
2. Price crosses below MA
3. Momentum divergence (price up, momentum down)
4. Target profit reached

**Exit Short:**
1. ROC turns positive
2. Price crosses above MA
3. Momentum divergence (price down, momentum up)
4. Target profit reached

## Parameter Configuration

### Momentum Thresholds
- **entry_threshold_pct**: Minimum ROC to enter (default: 1.5%)
- **exit_threshold_pct**: ROC level to exit (default: 0.5%)
- **ma_period**: Moving average period (default: 20)

### Position Management
- **max_position_pct**: Maximum position size (default: 3%)
- **scale_in**: Add to winning positions (optional)
- **trail_stop_pct**: Trailing stop percentage (default: 2%)

## Regime-Specific Behavior

### Trending Up
- Optimal environment for momentum
- Lower entry threshold (easier to enter)
- Wider trailing stops (let profits run)
- Scale into winning positions

### Trending Down
- Inverse momentum (short bias)
- Same principles, opposite direction
- Tighter stops (trends can reverse quickly)

### Ranging Market
- Poor environment for momentum
- Raise entry threshold significantly
- Reduce position sizes
- Consider switching to mean-reversion

### High Volatility
- False signals more common
- Require stronger momentum (higher threshold)
- Tighter position limits
- Faster exit on momentum fade

## Risk Management

### Position Sizing
- Base size: 2% of portfolio
- Scale with momentum strength:
  - Weak momentum (1-2%): 50% of base
  - Medium momentum (2-3%): 100% of base
  - Strong momentum (>3%): 150% of base

### Stop Loss Strategy
- Initial stop: 1.5-2% below entry
- Move to breakeven after 1% profit
- Trail by 2% once in profit

### Momentum Divergence
- Watch for price making new highs while momentum weakens
- This is an early exit signal
- Reduce position or exit entirely

## Parameter Tuning Guidelines

| Condition | Entry Threshold | MA Period | Trail Stop |
|-----------|----------------|-----------|------------|
| Normal | 1.5% | 20 | 2.0% |
| Strong Trend | 1.0% | 15 | 3.0% |
| Weak Trend | 2.5% | 25 | 1.5% |
| High Volatility | 3.0% | 30 | 1.0% |

## When to Use This Strategy

✅ Good conditions:
- Clear directional trends
- High conviction moves
- Volume-supported price action
- Low noise environment

❌ Avoid when:
- Choppy, ranging markets
- News-driven volatility
- Low volume periods
- Near major support/resistance

## Common Pitfalls

1. **Chasing late**: Entering after momentum has peaked
2. **Ignoring volume**: Momentum without volume often fails
3. **Fighting the trend**: Trying to pick tops/bottoms
4. **No trailing stops**: Giving back all profits when trend reverses
