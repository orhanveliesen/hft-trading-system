# Parameter Tuning Concepts

This document explains what each parameter does and how they relate to each other. The AI tuner should use this understanding along with real-time data to make its own decisions.

## Core Parameters

### EMA Deviation Filter (ema_dev_*)

**Purpose**: Controls how far price can deviate from EMA before blocking trades.

**Concept**:
- Measures `(current_price - ema) / ema * 100` as percentage
- Higher threshold = more trades allowed (accepts prices further from EMA)
- Lower threshold = fewer trades (only trades near EMA)

**Relationships**:
- In trending markets, price naturally stays above/below EMA, so tighter filter blocks good trades
- In ranging markets, price oscillates around EMA, so tighter filter catches better entries
- In volatile markets, wider swings mean higher risk of bad entries

**What to consider**:
- Current market regime
- Recent price behavior relative to EMA
- Whether recent trades entered at good or bad levels

### Position Size (base_position_pct, max_position_pct)

**Purpose**: Controls trade size as percentage of capital.

**Concept**:
- Larger positions = higher potential profit AND higher potential loss
- Smaller positions = lower risk but also lower returns
- Should scale with confidence and market conditions

**Relationships**:
- After losses: smaller positions protect remaining capital
- After wins: maintain discipline, don't over-leverage
- High volatility: smaller positions reduce exposure to sudden moves
- Higher win rate: can justify slightly larger positions

**What to consider**:
- Current win rate
- Recent consecutive wins/losses
- Market volatility
- Available capital

### Cooldown (cooldown_ms)

**Purpose**: Minimum time between trades for the same symbol.

**Concept**:
- Prevents overtrading and whipsaw losses
- Allows market to develop after a trade
- Directly affects trading costs (fewer trades = lower total costs)

**Relationships**:
- More trades = more transaction costs
- Choppy markets benefit from longer cooldown
- Strong trends might justify shorter cooldown
- After losses, longer cooldown prevents revenge trading

**What to consider**:
- Current trades per hour rate
- Total costs vs total profits
- Market regime (trending vs ranging)
- Signal quality

### Target and Stop Loss (target_pct, stop_pct)

**Purpose**: Defines exit points for profitable and losing trades.

**Concept**:
- Target: Price movement needed to close with profit
- Stop: Maximum loss allowed before exiting
- Risk/Reward ratio = target / stop

**Relationships**:
- Higher targets work better in trending markets (let winners run)
- Lower targets work better in ranging markets (take quick profits)
- Stops should be outside normal market noise
- Stops too tight = stopped out by random fluctuations
- Stops too wide = large losses when wrong

**What to consider**:
- Current market volatility (ATR)
- Market regime
- Recent win rate
- Target hit rate vs stop hit rate

## Cost Awareness

### Understanding Trading Costs

Every trade incurs costs that directly impact profitability:
- **Commission**: Fee paid per trade
- **Slippage**: Difference between expected and actual execution price
- **Spread**: Bid-ask spread cost

### Key Cost Relationships

**Trade Frequency vs Costs**:
- More trades = higher total costs
- Each trade must overcome costs to be profitable
- High trading frequency can turn a profitable strategy into a losing one

**Break-Even Concept**:
```
net_profit_per_trade = gross_profit - costs
```
If costs are significant relative to profits, the strategy loses money even with good entries.

**Cost/Profit Ratio**:
- Ratio of total costs to gross profits
- When this ratio is high, trading is inefficient
- When costs exceed profits, the strategy is net negative

### What AI Should Monitor

**From the data provided**:
- `total_costs` vs `gross_pnl` - are costs eating profits?
- `trades_per_hour` - is the strategy overtrading?
- `cost_pct_per_trade` - cost as percentage of each trade
- `win_rate` - is the strategy finding good entries?
- `total_targets` vs `total_stops` - risk/reward in practice

**When costs are high relative to profits**:
- Consider increasing cooldown (fewer trades)
- Consider reducing position frequency
- Consider pausing if costs consistently exceed profits

**When win rate is low**:
- Entry criteria may be too loose (tighten EMA filter)
- Market conditions may not suit the strategy
- Consider adjusting target/stop ratio

## Regime Adaptation

### Trending Markets
- Price moves directionally with momentum
- EMA filter can be wider (price stays away from EMA)
- Targets can be higher (let trends develop)
- Position size can be normal or slightly higher

### Ranging Markets
- Price oscillates without clear direction
- EMA filter should be tighter (enter near mean)
- Targets should be quicker (take profits at extremes)
- Mean reversion works better than momentum

### High Volatility
- Large price swings increase both opportunity and risk
- Wider stops needed to avoid noise
- Smaller positions reduce exposure
- Longer cooldown prevents overtrading

### Changing Regimes
- When regime changes, previous parameter settings may become suboptimal
- Recent losses after a regime change suggest parameters need adjustment
- Gradual transitions are safer than sudden large changes

## Decision Making Principles

### Use Real Data
- Always base decisions on actual metrics provided
- Compare current performance to recent history
- Look for patterns in win/loss sequences

### Consider Multiple Factors
- Don't optimize for a single metric
- Balance risk and reward
- Consider costs in all decisions

### Gradual Adjustments
- Large sudden changes increase risk
- Small incremental adjustments allow observation of effects
- Wait for sufficient trades before reassessing

### When in Doubt, Be Conservative
- Reduce position size rather than increase
- Increase cooldown rather than decrease
- Tighten filters rather than widen
- Protecting capital is priority over maximizing returns
