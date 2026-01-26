# Market Microstructure

## Overview
Market microstructure is the study of how markets operate at the detailed level - how orders are processed, how prices are formed, and how information flows through the market. Understanding microstructure is essential for HFT systems.

## Order Book Basics

### Structure
```
         ASK SIDE
    ┌─────────────────┐
    │ $100.05 x 500   │  <- Best Ask (lowest sell)
    │ $100.06 x 1000  │
    │ $100.07 x 750   │
    ├─────────────────┤
    │ $100.03 x 300   │  <- Best Bid (highest buy)
    │ $100.02 x 800   │
    │ $100.01 x 1200  │
    └─────────────────┘
         BID SIDE
```

### Key Concepts
- **Best Bid**: Highest price someone will pay
- **Best Ask**: Lowest price someone will sell
- **Spread**: Best Ask - Best Bid (cost of immediacy)
- **Mid Price**: (Best Bid + Best Ask) / 2
- **Depth**: Volume available at each price level

## Bid-Ask Spread

### Components
1. **Inventory cost**: Market maker holding risk
2. **Adverse selection**: Trading against informed traders
3. **Order processing**: Fixed costs of trading
4. **Competition**: More market makers = tighter spread

### Spread Interpretation
- **Tight spread** (0.01-0.05%): High liquidity, easy to trade
- **Normal spread** (0.05-0.20%): Standard conditions
- **Wide spread** (>0.20%): Low liquidity, careful with size
- **Spread blowout**: News event or market stress

### Trading Cost Impact
```
Entry at Ask: $100.05
Exit at Bid:  $100.03
Spread cost:  $0.02 (0.02%)

For round-trip trade of 1000 units:
Cost = 1000 * $0.02 = $20
```

## Slippage

### Definition
Difference between expected execution price and actual execution price.

### Causes
1. **Market impact**: Your order moves the price
2. **Latency**: Price moved before order arrived
3. **Size**: Large orders eat through book levels
4. **Volatility**: Fast-moving markets

### Slippage Estimation
```
Small order (< 10% of best level): Minimal slippage
Medium order (10-50% of best level): 1-2 ticks slippage
Large order (> 50% of best level): Multiple levels slippage
```

### Slippage Formula (Simplified)
```
Expected Slippage = Order Size / Available Liquidity * Volatility Factor
```

## Liquidity Assessment

### Depth Analysis
- **Level 1**: Best bid/ask (immediate liquidity)
- **Level 2-5**: Near-the-touch liquidity
- **Deep book**: Total available liquidity

### Liquidity Indicators
- **Bid-ask spread**: Primary liquidity measure
- **Quote size**: Volume at best bid/ask
- **Book depth**: Total volume within X% of mid
- **Trade frequency**: How often market trades

### Liquidity Score (Example)
```
Score = (Quote_Size * 0.4) + (1/Spread * 0.3) + (Trade_Freq * 0.3)

High score (> 80): Very liquid, trade freely
Medium score (50-80): Adequate, mind size
Low score (< 50): Illiquid, trade carefully
```

## Order Types and Execution

### Market Order
- Execute immediately at best available price
- Guarantees execution, not price
- Crosses the spread (pays bid-ask)
- Use when: Speed is critical

### Limit Order
- Execute only at specified price or better
- Guarantees price, not execution
- Provides liquidity (earns spread)
- Use when: Price is more important than speed

### Execution Quality
```
Market Order Buy at Ask $100.05:
- If you get $100.05: Perfect execution
- If you get $100.06: 1 tick negative slippage
- If you get $100.04: 1 tick price improvement

Limit Order Buy at $100.03:
- If filled: You earned the spread
- If not filled: Missed the opportunity
```

## Price Formation

### Continuous Trading
- Prices set by order book matching
- New information -> New orders -> New prices
- Price = consensus of all market participants

### Price Discovery
- Information flows through trades
- Large trades signal information
- Price adjusts to reflect new information

### Efficient Price
```
Efficient Price ≈ Mid Price (under normal conditions)
                = Weighted average of recent trades (under imbalance)
```

## Market Impact

### Definition
How your own order affects the market price.

### Temporary vs. Permanent Impact
- **Temporary**: Price bounces back after your trade
- **Permanent**: Price stays at new level (your trade revealed information)

### Impact Estimation
```
Impact = k * sqrt(Order_Size / ADV) * Volatility

k = market-specific constant
ADV = Average Daily Volume
```

### Minimizing Impact
1. **Split orders**: Multiple smaller orders
2. **Use limits**: Don't cross aggressively
3. **Time spreading**: Execute over longer period
4. **Dark pools**: Trade away from lit markets

## Queue Position

### Importance
- Limit orders execute in price-time priority
- First in queue gets filled first
- Queue position = competitive advantage

### Queue Dynamics
```
At price $100.03:
  Order 1: 100 shares (first)  <- fills first
  Order 2: 200 shares (second)
  Order 3: 150 shares (third)
  Your order: 50 shares (last) <- fills last

If only 250 shares trade at $100.03:
  Order 1: Filled (100)
  Order 2: Filled (150 of 200)
  Order 3: Not filled
  Your order: Not filled
```

### Improving Queue Position
- Place orders early
- Cancel and replace loses position
- Some venues offer queue priority for certain order types

## Information Signals

### Order Flow Signals
- Large market orders: Informed trading
- Cancel activity: Sentiment change
- Quote updates: Market maker adjustment

### Imbalance Signals
```
Bid Volume >> Ask Volume: Buying pressure, price may rise
Ask Volume >> Bid Volume: Selling pressure, price may fall

Imbalance Ratio = (Bid_Vol - Ask_Vol) / (Bid_Vol + Ask_Vol)
  > +0.3: Strong buy imbalance
  < -0.3: Strong sell imbalance
```

### Trade Signals
- Trade at ask: Buyer aggressing (bullish)
- Trade at bid: Seller aggressing (bearish)
- Large trade: Information event

## Microstructure Trading Implications

### For Market Making
- Quote tighter when liquidity is high
- Widen quotes when adverse selection risk high
- Manage inventory to avoid imbalanced book

### For Momentum/Trend
- Enter when order flow supports direction
- Avoid trading against strong imbalance
- Use market orders when speed matters

### For Mean Reversion
- Enter when temporary impact detected
- Wait for imbalance to normalize
- Use limit orders to earn spread

## Parameters for AI Tuning

### Microstructure-Aware Parameters
- **spread_threshold_bps**: Max spread to trade (default: 10 bps)
- **min_depth**: Minimum book depth to trade
- **imbalance_threshold**: Order flow imbalance trigger
- **slippage_estimate_pct**: Expected slippage per trade

### Regime Adjustments
| Condition | Spread Limit | Min Depth | Slippage Est |
|-----------|-------------|-----------|--------------|
| Normal | 10 bps | 1000 | 0.02% |
| Low Liquidity | 5 bps | 2000 | 0.05% |
| High Vol | 20 bps | 500 | 0.10% |
| News Event | Pause | - | - |
