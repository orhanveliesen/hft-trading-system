# Trading Strategies Encyclopedia

A comprehensive reference of trading strategies. Each strategy has specific market conditions where it performs best and worst. The AI should understand these concepts and apply them based on current market data.

---

## 1. Momentum Strategies

### Trend Following
**Concept**: Buy assets that are going up, sell assets that are going down. "The trend is your friend."

**How it works**:
- Identify direction using moving averages, price channels, or breakouts
- Enter in the direction of the trend
- Exit when trend shows signs of reversal

**Best conditions**: Strong directional markets with persistent moves
**Worst conditions**: Choppy, ranging markets (whipsaw losses)

**Variations**:
- Moving average crossovers
- Channel breakouts (Donchian, Keltner)
- ADX-based trend strength

### Momentum/Relative Strength
**Concept**: Assets that have outperformed recently will continue to outperform in the near term.

**How it works**:
- Measure recent returns over a lookback period
- Buy winners, sell losers
- Rebalance periodically

**Best conditions**: Markets with persistence in returns
**Worst conditions**: Mean-reverting markets, momentum crashes

### Breakout Trading
**Concept**: Enter when price breaks out of a range or pattern, expecting continuation.

**How it works**:
- Identify consolidation ranges, triangles, or support/resistance
- Enter when price breaks with volume confirmation
- Set stops below/above breakout point

**Best conditions**: After periods of low volatility compression
**Worst conditions**: When many breakouts fail (range-bound markets)

---

## 2. Mean Reversion Strategies

### Statistical Mean Reversion
**Concept**: Prices tend to return to their mean over time. Buy low, sell high relative to average.

**How it works**:
- Measure deviation from moving average or fair value
- Buy when significantly below mean
- Sell when significantly above mean

**Best conditions**: Ranging, oscillating markets
**Worst conditions**: Strong trending markets

### RSI/Oscillator Mean Reversion
**Concept**: Overbought conditions lead to pullbacks, oversold leads to bounces.

**How it works**:
- Use RSI, Stochastics, or similar oscillators
- Buy when oversold, sell when overbought
- Combine with support/resistance levels

**Best conditions**: Range-bound markets with clear oscillations
**Worst conditions**: Strong trends (oscillator stays extreme)

### Bollinger Band Reversion
**Concept**: Price tends to stay within statistical bands.

**How it works**:
- Buy when price touches lower band
- Sell when price touches upper band
- Band width indicates volatility regime

**Best conditions**: Low-volatility ranging markets
**Worst conditions**: Trending markets, band walking

### Pairs Trading / Statistical Arbitrage
**Concept**: Trade the relationship between correlated assets.

**How it works**:
- Identify historically correlated pairs
- When spread widens, buy underperformer, sell outperformer
- Exit when spread normalizes

**Best conditions**: Stable correlations, temporary divergences
**Worst conditions**: Structural correlation breakdown

---

## 3. Market Making Strategies

### Bid-Ask Spread Capture
**Concept**: Provide liquidity on both sides, profit from spread.

**How it works**:
- Quote bid and ask prices around fair value
- Capture spread when both sides fill
- Manage inventory to avoid directional risk

**Best conditions**: Stable markets, high volume, predictable flow
**Worst conditions**: One-sided markets, adverse selection, news events

### Inventory Management
**Concept**: Skew quotes to reduce inventory risk.

**How it works**:
- When long inventory, skew quotes to encourage sells
- When short inventory, skew quotes to encourage buys
- Maintain neutral position over time

**Key relationships**:
- Larger inventory = more aggressive skew
- Volatile markets = wider spreads
- Low volume = more inventory risk

### Passive Rebate Capture
**Concept**: Earn exchange rebates by providing liquidity.

**How it works**:
- Post limit orders that provide liquidity
- Earn maker rebates when filled
- Avoid taking liquidity (taker fees)

**Best conditions**: Exchanges with maker rebates, stable prices
**Worst conditions**: Fast-moving markets, adverse selection

---

## 4. Arbitrage Strategies

### Exchange Arbitrage
**Concept**: Same asset trades at different prices on different exchanges.

**How it works**:
- Monitor prices across multiple venues
- Buy on cheaper exchange, sell on more expensive
- Profit from price discrepancy minus fees

**Best conditions**: Fragmented markets, slow price propagation
**Worst conditions**: Efficient markets, high latency

### Triangular Arbitrage
**Concept**: Price inconsistencies in currency/asset triangles.

**How it works**:
- A/B * B/C * C/A should equal 1
- When it doesn't, execute all three legs
- Lock in risk-free profit

**Best conditions**: Temporary mispricing in related pairs
**Worst conditions**: Efficient automated markets

### Basis Trading
**Concept**: Trade the spread between spot and futures.

**How it works**:
- When futures premium is high, sell futures, buy spot
- When futures discount is high, buy futures, sell spot
- Converge at expiration

**Best conditions**: Predictable funding/basis, stable markets
**Worst conditions**: Extreme market moves, liquidation cascades

---

## 5. Volatility Strategies

### Volatility Mean Reversion
**Concept**: Volatility tends to revert to long-term average.

**How it works**:
- When volatility is extremely high, expect decline
- When volatility is extremely low, expect increase
- Trade options or adjust position sizing accordingly

**Best conditions**: Extreme volatility readings
**Worst conditions**: Volatility regime changes

### Volatility Breakout
**Concept**: Low volatility precedes high volatility moves.

**How it works**:
- Identify periods of compression (low ATR, tight Bollinger)
- Position for breakout in either direction
- Use options or stop orders

**Best conditions**: After extended consolidation
**Worst conditions**: Continued low volatility drift

### Gamma Scalping
**Concept**: Trade underlying to hedge options gamma exposure.

**How it works**:
- Hold options position with gamma
- As underlying moves, delta changes
- Rebalance by trading underlying

**Best conditions**: High actual volatility, low implied volatility
**Worst conditions**: Low actual volatility, high implied volatility (theta decay)

---

## 6. Order Flow Strategies

### Order Flow Imbalance
**Concept**: Large buy/sell imbalances predict short-term price movement.

**How it works**:
- Monitor order book depth and flow
- Detect aggressive buyers/sellers
- Trade in direction of imbalance

**Best conditions**: Markets with visible order flow, institutional activity
**Worst conditions**: Spoofing, hidden orders, fragmented flow

### VWAP Trading
**Concept**: Execute at or better than Volume Weighted Average Price.

**How it works**:
- Spread execution over time weighted by expected volume
- Minimize market impact
- Track performance vs VWAP benchmark

**Best conditions**: Liquid markets, predictable volume patterns
**Worst conditions**: Unusual volume, news-driven markets

### TWAP Trading
**Concept**: Execute evenly over time regardless of volume.

**How it works**:
- Split order into equal time intervals
- Execute fixed amount each interval
- Simpler than VWAP, more predictable

**Best conditions**: When execution timing is more important than price
**Worst conditions**: Highly variable intraday patterns

---

## 7. Technical Analysis Strategies

### Support/Resistance Trading
**Concept**: Prices react at historical levels.

**How it works**:
- Identify key price levels from history
- Buy at support, sell at resistance
- Use breakout confirmation for trend trades

**Best conditions**: Clear technical levels, range-bound markets
**Worst conditions**: Strong trends that break all levels

### Pattern Recognition
**Concept**: Chart patterns predict future price movement.

**Patterns include**:
- Head and shoulders (reversal)
- Double top/bottom (reversal)
- Triangles (continuation or reversal)
- Flags and pennants (continuation)
- Cup and handle (continuation)

**Best conditions**: High-volume confirmation of patterns
**Worst conditions**: Low liquidity, noise-driven markets

### Candlestick Patterns
**Concept**: Short-term price patterns signal reversals or continuations.

**Patterns include**:
- Doji (indecision)
- Engulfing (reversal)
- Hammer/Shooting star (reversal)
- Three white soldiers/black crows (continuation)

**Best conditions**: At key support/resistance levels
**Worst conditions**: Choppy, news-driven markets

### Moving Average Strategies
**Concept**: Moving averages smooth price and identify trends.

**Approaches**:
- Single MA: price above/below indicates trend
- MA crossover: fast crosses slow signals entry
- MA envelope: bands around MA for mean reversion

**Best conditions**: Trending markets with clear direction
**Worst conditions**: Whipsaw in ranging markets

---

## 8. Quantitative Strategies

### Factor Investing
**Concept**: Certain characteristics (factors) predict returns.

**Common factors**:
- Value: underpriced relative to fundamentals
- Momentum: recent performance persistence
- Size: smaller assets outperform
- Quality: profitable, stable companies
- Low volatility: less volatile assets outperform

**Best conditions**: Factor premiums are positive
**Worst conditions**: Factor crashes, regime changes

### Machine Learning Strategies
**Concept**: Use ML models to predict price movements.

**Approaches**:
- Supervised learning: predict direction/magnitude
- Reinforcement learning: optimize trading policy
- Deep learning: complex pattern recognition

**Best conditions**: Sufficient training data, stable patterns
**Worst conditions**: Regime changes, overfitting, data snooping

### Risk Parity
**Concept**: Allocate based on risk contribution, not capital.

**How it works**:
- Measure volatility/risk of each asset
- Allocate so each contributes equal risk
- Often uses leverage on low-risk assets

**Best conditions**: Diversified portfolios, stable correlations
**Worst conditions**: Correlation spikes in crises

---

## 9. High-Frequency Strategies

### Latency Arbitrage
**Concept**: Exploit speed advantage to front-run slower participants.

**How it works**:
- Detect orders on slow venues
- Trade ahead on fast venues
- Profit from predictable price impact

**Best conditions**: Speed advantage, fragmented markets
**Worst conditions**: Consolidated venues, colocation parity

### Market Microstructure
**Concept**: Trade based on order book dynamics and queue position.

**How it works**:
- Analyze order book depth and changes
- Predict short-term price impact
- Optimize queue position for fills

**Best conditions**: Predictable microstructure, stable markets
**Worst conditions**: News events, flash crashes

### Quote Stuffing Defense
**Concept**: Detect and avoid manipulation tactics.

**What to watch**:
- Unusually high message rates
- Orders that are quickly cancelled
- Spoofing patterns (large orders that disappear)

---

## 10. Event-Driven Strategies

### News Trading
**Concept**: React quickly to news and announcements.

**How it works**:
- Monitor news feeds and social media
- Parse sentiment and relevance
- Trade before/after announcements

**Best conditions**: Clear, actionable news, liquid markets
**Worst conditions**: Fake news, delayed information, whipsaw

### Earnings Plays
**Concept**: Trade around company earnings announcements.

**Approaches**:
- Predict earnings surprise
- Trade volatility expansion before
- Trade drift after surprise

**Best conditions**: Predictable patterns, sufficient history
**Worst conditions**: Random/unpredictable results

### Economic Calendar Trading
**Concept**: Trade around scheduled economic releases.

**Events**:
- Central bank decisions
- Employment reports
- GDP, inflation data
- Trade balances

**Best conditions**: Clear market expectations, decisive releases
**Worst conditions**: Ambiguous data, market indecision

---

## 11. Position Management Strategies

### Pyramiding
**Concept**: Add to winning positions as they move in your favor.

**How it works**:
- Start with base position
- Add at predetermined levels as profit grows
- Each addition is smaller than previous

**Best conditions**: Strong trends, high conviction
**Worst conditions**: Markets that reverse after initial move

### Scaling Out
**Concept**: Take partial profits at different levels.

**How it works**:
- Exit portion of position at first target
- Trail stop on remainder
- Reduces risk while allowing upside

**Best conditions**: Uncertain trend strength
**Worst conditions**: None (generally sound risk management)

### Grid Trading
**Concept**: Place orders at regular intervals in a range.

**How it works**:
- Define price range and grid spacing
- Buy at lower levels, sell at higher levels
- Profit from oscillation within range

**Best conditions**: Ranging markets with predictable bounds
**Worst conditions**: Breakouts beyond grid range

---

## 12. Risk Management Concepts

### Position Sizing Models

**Fixed Fractional**: Risk fixed percentage of capital per trade
**Kelly Criterion**: Optimal size based on edge and win rate
**Volatility-Based**: Size inversely proportional to volatility

### Stop Loss Types

**Fixed Stop**: Set distance from entry
**Trailing Stop**: Follows price, locks in profit
**Volatility Stop**: Distance based on ATR or standard deviation
**Time Stop**: Exit after certain time regardless of price

### Correlation Management

**Concept**: Correlated positions amplify risk.

**What to monitor**:
- Don't have multiple highly correlated positions
- Correlation increases in crises
- Diversification reduces total risk

---

## Strategy Selection Guidelines

The AI should consider:

1. **Current market regime**: Which strategies suit current conditions?
2. **Recent performance**: Is current strategy working or failing?
3. **Cost structure**: Can strategy overcome transaction costs?
4. **Risk tolerance**: How much drawdown is acceptable?
5. **Capital available**: Does strategy require scale?

**No strategy works in all conditions.** The key is matching strategy to market state and adapting when conditions change.
