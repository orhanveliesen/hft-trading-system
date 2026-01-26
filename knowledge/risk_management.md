# Risk Management Concepts

Risk management is the foundation of sustainable trading. Without proper risk management, even profitable strategies eventually fail.

## Core Principles

### Capital Preservation
- Survival is the priority - you can't trade if you lose your capital
- No single trade should threaten the portfolio
- Accept small losses to avoid catastrophic ones
- Protecting downside is more important than maximizing upside

### Position Sizing
- Trade size should reflect confidence and conditions
- Reduce size when uncertain or after losses
- Never "bet the farm" on any single trade
- Size inversely with volatility

### Always Have an Exit Plan
- Know the stop loss before entering
- Know the target before entering
- Never hope a losing position recovers
- Exit discipline is more important than entry precision

## Position Sizing Methods

### Percentage Risk
**Concept**: Risk a consistent percentage of capital per trade.

**How it works**:
- Determine maximum loss acceptable
- Set stop loss distance
- Calculate position size: `Size = (Capital × Risk%) / Stop Distance`

**Relationship to performance**:
- Reduces size after losses (less capital)
- Increases size after wins (more capital)
- Self-adjusting to portfolio changes

### Volatility-Based Sizing
**Concept**: Size inversely proportional to asset volatility.

**How it works**:
- Measure volatility (ATR or standard deviation)
- Higher volatility = smaller position
- Lower volatility = can use larger position

**Why it matters**:
- Normalizes risk across different volatility environments
- Prevents outsized losses in volatile conditions
- Allows larger positions when risk is lower

### Kelly Criterion
**Concept**: Mathematically optimal sizing based on edge.

**Formula**: `Kelly% = Win% - (Loss% / Win:Loss Ratio)`

**In practice**:
- Full Kelly is too aggressive
- Use fractional Kelly for safety
- Requires accurate estimates of win rate and ratio

## Stop Loss Strategies

### Fixed Stop
- Set at trade entry, never moves against position
- Simple and consistent
- No discretion required
- May be too rigid for some conditions

### Trailing Stop
- Follows price to lock in profits
- Only moves in favorable direction
- Captures more profit in trends
- May exit too early in choppy markets

### Volatility Stop
- Distance based on volatility measure (ATR)
- Adapts to market conditions
- Wider in volatile markets (avoids noise)
- Tighter in calm markets (precision exits)

### Time Stop
- Exit if position doesn't move favorably within time limit
- Prevents capital being tied up
- Forces discipline about opportunity cost

## Take Profit Strategies

### Fixed Target
- Set at entry, exit at predetermined level
- Ensures profits are taken
- May leave money on table in trends

### Trailing Target
- No fixed exit, trail with price
- Captures more in trends
- May give back profits in reversals

### Scaled Exit
- Exit portions at different levels
- Balances locking profit vs letting run
- Example: half at first target, half trailing

## Drawdown Management

### Why Drawdowns Matter
- Larger drawdowns require larger recoveries
- 50% loss requires 100% gain to recover
- Emotional impact increases with drawdown
- Risk of capitulation at worst time

### Drawdown Response

**As drawdown increases**:
- Reduce position sizes
- Increase selectivity (fewer trades)
- Review strategy for issues
- Consider pausing to reassess

**Recovery approach**:
- Start small when returning
- Rebuild confidence gradually
- Don't try to make it back quickly

### Consecutive Loss Management
**Concept**: Multiple losses in a row signal potential problems.

**Responses**:
- Pause and assess after streak
- Reduce size on return
- Check if market regime changed
- Avoid revenge trading

## Portfolio-Level Risk

### Concentration Risk
- Multiple positions in same direction amplifies risk
- Correlated assets move together
- Diversification reduces risk

### Exposure Management
- Monitor total market exposure
- Reduce exposure in uncertainty
- Don't be over-leveraged

### Correlation Awareness
- Positions that seem different may be correlated
- Correlation increases in market stress
- Plan for correlation to spike

## Emergency Procedures

### Flash Crash
- Exit or hedge all positions immediately
- Cancel open orders
- Do not re-enter until stable
- Assess damage and cause

### Extreme Volatility
- Reduce position sizes significantly
- Widen stops (to avoid whipsaw)
- Pause new entries
- Wait for normalization

### Connection Issues
- Ensure server-side stops are in place
- Verify position state on reconnect
- Reconcile expected vs actual

## Risk Metrics to Monitor

**Real-time**:
- Current P&L (realized + unrealized)
- Total exposure
- Position count
- Largest single position risk

**Session/Daily**:
- Win rate
- Average win vs average loss
- Drawdown from peak
- Risk/reward realization

**What to watch for**:
- Declining win rate: strategy may not fit current regime
- Average loss > average win: risk/reward imbalance
- Increasing drawdown: need defensive action
- Many consecutive losses: pause and assess

## Common Risk Management Mistakes

### No Stop Loss
"It will come back" - one of the most dangerous beliefs in trading. Always have a stop.

### Moving Stop Against Position
Defeats the purpose of the stop. Accept the loss.

### Averaging Down
Adding to losers increases risk. Cut losers, let winners run.

### Oversizing on Conviction
No trade is certain. Size discipline must be maintained.

### Revenge Trading
Trying to make back losses quickly usually makes them worse.

### Ignoring Correlation
Multiple "different" positions may all lose together.

### Removing Protection Before Events
Maximum risk at maximum uncertainty is backwards.

## Key Takeaways

1. **Risk first, reward second**: Manage downside before thinking about upside
2. **Position size matters**: Wrong size can turn winning strategy into losing one
3. **Stops are non-negotiable**: Every position needs an exit plan
4. **Drawdowns are exponential**: Small losses are much easier to recover from
5. **Discipline over discretion**: Follow the rules, especially when it's hard
6. **Survival enables success**: Can't profit if you blow up
