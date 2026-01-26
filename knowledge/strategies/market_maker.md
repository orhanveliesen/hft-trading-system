# Market Maker Strategy

## Overview
The Market Maker Strategy provides liquidity by placing two-sided quotes (bid and ask) around the current market price. It profits from the bid-ask spread while managing inventory risk.

## Core Concept

```
Market Price: $100.00
Our Bid: $99.90 (buy lower)
Our Ask: $100.10 (sell higher)
Spread Capture: $0.20 per round-trip
```

## Key Parameters

### Spread Configuration
- **base_spread_bps**: Base spread in basis points (default: 10 bps = 0.1%)
- **spread_multiplier**: Multiplier based on volatility (1.0-3.0)
- **min_spread_bps**: Minimum spread to maintain profitability

### Inventory Management
- **max_inventory**: Maximum position allowed
- **inventory_skew**: How much to skew quotes based on position

### Quote Adjustment
- **quote_refresh_ms**: How often to update quotes
- **cancel_on_fill**: Cancel opposing side on fill

## Inventory Skew Logic

When holding long inventory, we want to sell more than buy:
```
Long 100 units -> Skew asks lower (more attractive)
Short 100 units -> Skew bids higher (more attractive)
```

Skew formula:
```
bid_adjustment = -inventory * skew_factor
ask_adjustment = +inventory * skew_factor
```

## Signal Generation

### When to Quote
1. Spread is profitable (> min_spread + costs)
2. Volatility is manageable
3. Inventory within limits

### When NOT to Quote
1. Extreme volatility (spread collapses)
2. News events (high adverse selection)
3. Inventory at max limits

## Regime-Specific Behavior

### Trending Up
- Skew quotes to favor buying
- Widen spread on ask side
- Reduce inventory skew

### Trending Down
- Skew quotes to favor selling
- Widen spread on bid side
- Increase inventory reduction speed

### Ranging/Low Volatility
- Optimal for market making
- Tighter spreads possible
- Normal inventory management

### High Volatility
- Widen spreads significantly (2-3x)
- Reduce quote sizes
- Consider pausing if spread collapses

## Risk Management

### Inventory Limits
- Hard limit: Never exceed max_inventory
- Soft limit: Start skewing at 50% of max

### Spread Limits
- If realized spread < costs: Pause quoting
- If spread widening fails: Exit position

### Stop Loss
- Per-symbol stop loss at 2-3% of position value
- Daily stop loss at 5% of capital

## Parameter Tuning Guidelines

| Market Condition | Spread Mult | Inventory Skew | Quote Size |
|-----------------|-------------|----------------|------------|
| Normal | 1.0x | 0.5 | 100% |
| Trending | 1.5x | 0.3 | 80% |
| High Vol | 2.0-3.0x | 1.0 | 50% |
| News Event | Pause | - | 0% |

## When to Use This Strategy

✅ Good conditions:
- Ranging markets with stable spreads
- Low-medium volatility
- Good liquidity (tight spreads)

❌ Avoid when:
- Strong trends (adverse selection)
- High volatility spikes
- Major news events
- Wide spreads (low liquidity)
