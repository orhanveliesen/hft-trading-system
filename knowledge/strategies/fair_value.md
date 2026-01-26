# Fair Value Strategy

## Overview
The Fair Value Strategy calculates a theoretical "fair" price for an asset based on various inputs, then trades when the market price deviates significantly from this fair value. It profits from mean reversion back to fair value.

## Core Concept

```
Fair Value: $100.00
Market Price: $98.00 (2% below fair value)
Action: BUY (expect reversion to $100)

Fair Value: $100.00
Market Price: $103.00 (3% above fair value)
Action: SELL (expect reversion to $100)
```

## Fair Value Calculation Methods

### Method 1: VWAP-Based
- Volume Weighted Average Price over N periods
- Formula: `FV = Σ(Price * Volume) / Σ(Volume)`
- Good for intraday trading
- Updates continuously

### Method 2: EMA-Based
- Exponential Moving Average as fair value
- Formula: `FV = EMA(Price, period)`
- Smoother, less reactive
- Good for trend-following adjustment

### Method 3: Mid-Price Based
- Average of best bid and best ask
- Formula: `FV = (Best_Bid + Best_Ask) / 2`
- Reflects current market consensus
- Very short-term oriented

### Method 4: Composite
- Weighted average of multiple methods
- Example: `FV = 0.5*VWAP + 0.3*EMA + 0.2*Mid`
- More robust, less prone to single-method failure

## Signal Generation

### Entry Signals

**Long Entry (Buy):**
1. Market price < Fair Value - deviation_threshold
2. Deviation is not due to fundamental change
3. Spread is acceptable (not too wide)
4. Volume supports the move

**Short Entry (Sell):**
1. Market price > Fair Value + deviation_threshold
2. Deviation is not due to fundamental change
3. Spread is acceptable
4. Volume supports the move

### Exit Signals

**Exit Long:**
1. Price returns to Fair Value (profit target)
2. Price falls further (stop loss)
3. Fair Value itself moves lower (fundamental change)

**Exit Short:**
1. Price returns to Fair Value (profit target)
2. Price rises further (stop loss)
3. Fair Value itself moves higher (fundamental change)

## Key Parameters

### Deviation Thresholds
- **entry_deviation_pct**: Minimum deviation to enter (default: 0.5%)
- **exit_deviation_pct**: Close when within this range of FV (default: 0.1%)
- **max_deviation_pct**: Don't trade if deviation too large (might be news)

### Fair Value Settings
- **fv_method**: Calculation method (vwap, ema, mid, composite)
- **fv_period**: Lookback period for calculation
- **fv_update_interval**: How often to recalculate

## Regime-Specific Behavior

### Trending Up
- Fair value should trend up
- Only take long entries
- Increase deviation threshold (trend = normal deviation)
- Use trailing fair value, not static

### Trending Down
- Fair value should trend down
- Only take short entries
- Increase deviation threshold
- Use trailing fair value

### Ranging Market
- Optimal for fair value strategy
- Tighter deviation thresholds (0.3-0.5%)
- Both long and short entries allowed
- Mean reversion more reliable

### High Volatility
- Fair value less reliable
- Wider deviation thresholds (1-2%)
- Reduce position sizes
- Consider pausing if volatility extreme

## Risk Management

### Position Sizing
- Base position: 2% of portfolio
- Scale inversely with deviation:
  - Small deviation (0.5%): Full size
  - Large deviation (1%+): Half size (higher risk of fundamental change)

### Stop Loss
- Hard stop at 2x entry deviation
- Example: Enter at 0.5% below FV, stop at 1.5% below FV
- Consider time-based stops (if no reversion in N minutes)

### Fundamental Change Detection
- If price stays deviated for extended period, fair value might be wrong
- Update fair value calculation
- Don't fight the market

## Parameter Tuning Guidelines

| Condition | Entry Dev | Exit Dev | FV Method |
|-----------|----------|---------|-----------|
| Ranging | 0.3% | 0.1% | VWAP |
| Trending | 0.8% | 0.2% | EMA |
| High Vol | 1.5% | 0.3% | Composite |
| News Event | Pause | - | - |

## When to Use This Strategy

✅ Good conditions:
- Stable, ranging markets
- High liquidity (tight spreads)
- No major news expected
- Clear fair value consensus

❌ Avoid when:
- Strong directional trends
- News events or earnings
- Low liquidity periods
- Market structure changes

## Integration with Other Strategies

Fair Value works well combined with:
- **Market Making**: Use FV as mid-point for quotes
- **Technical Indicators**: Confirm FV deviation with RSI/BB
- **Momentum**: Exit FV trades when momentum shifts
