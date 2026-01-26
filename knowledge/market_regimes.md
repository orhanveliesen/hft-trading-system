# Market Regimes

Understanding market regimes is essential for adapting trading behavior. Each regime has distinct characteristics and requires different approaches.

## Regime Types

### TRENDING_UP
**Characteristics**:
- Price consistently above moving averages
- Higher highs and higher lows pattern
- Strong directional momentum
- ADX elevated (strong trend strength)

**Implications**:
- Momentum strategies work well
- Mean reversion strategies fail (buying dips gets run over)
- Price stays far from EMA, so tight EMA filters block good entries
- Let winners run - don't take profits too early

**Parameter relationships**:
- Wider EMA filter: price naturally deviates from mean
- Higher targets: trends can extend further than expected
- Moderate stops: allow for pullbacks within trend
- Normal or shorter cooldown: more opportunities

### TRENDING_DOWN
**Characteristics**:
- Price consistently below moving averages
- Lower highs and lower lows pattern
- Negative momentum, selling pressure dominant

**Implications for long-only systems**:
- Very selective buying - only extreme oversold conditions
- Buying dips is dangerous - they often continue lower
- Quick profit taking - don't expect extended rallies
- Smaller positions - higher probability of loss

**Parameter relationships**:
- Tighter EMA filter: only buy near mean, not on extensions
- Lower targets: take profits quickly on any bounce
- Tight stops: cut losses fast, don't hope for recovery
- Longer cooldown: fewer opportunities, be patient

### RANGING
**Characteristics**:
- Price oscillating around mean (EMA)
- No clear directional trend
- ADX low (weak trend strength)
- Support and resistance levels hold

**Implications**:
- Mean reversion works well
- Buy at support, sell at resistance
- Technical indicators (RSI, Bollinger) are reliable
- Breakout attempts often fail

**Parameter relationships**:
- Tight EMA filter: enter close to mean for best reversion
- Moderate targets: take profits at range extremes
- Moderate stops: exit if range breaks
- Normal cooldown: steady opportunities

### HIGH_VOLATILITY
**Characteristics**:
- ATR significantly above normal
- Large price swings in both directions
- Frequent whipsaws and false signals
- Often news-driven or panic conditions

**Implications**:
- DEFENSIVE MODE - reduce or pause trading
- Normal strategies fail - too much noise
- Stops get hit by random moves
- Entry timing is nearly impossible

**Parameter relationships**:
- Very tight EMA filter: avoid chasing moves
- Much smaller positions: protect capital
- Quick targets: take any profit available
- Very tight stops: limit damage per trade
- Much longer cooldown: wait for clarity

### LOW_VOLATILITY
**Characteristics**:
- ATR below normal
- Tight price ranges
- Clear technical levels
- Predictable, slow price movement

**Implications**:
- Mean reversion works very well
- Technical levels are respected
- Smaller profit targets needed (moves are small)
- Lower risk environment

**Parameter relationships**:
- Tight EMA filter: precision entries work
- Can use slightly larger positions (lower risk)
- Lower targets: movements are smaller
- Tighter stops: less noise to filter
- Normal cooldown

### SPIKE
**Characteristics**:
- Sudden extreme price move
- Usually news or event-driven
- Volume spike
- Unpredictable direction after initial move

**Implications**:
- DO NOT TRADE during spikes
- Wait for market to stabilize
- Previous levels become invalid
- Liquidity may be poor

**Action**:
- Pause all new entries
- Tighten stops on existing positions
- Wait for regime to clarify
- Reassess after stability returns

## Regime Detection

The AI should consider these signals when assessing regime:

**Trend indicators**:
- Price position relative to EMA
- ADX or similar trend strength measure
- Higher/lower highs and lows pattern

**Volatility indicators**:
- ATR relative to recent average
- Bollinger Band width
- Price range as percentage

**Mean reversion indicators**:
- RSI oscillation patterns
- Price returning to EMA repeatedly
- Support/resistance respect

## Regime Transitions

**Key concept**: Parameters that worked in the previous regime may fail in the new one.

**When regime changes**:
- Recent losses may indicate mismatch
- Reassess all parameters
- Start conservatively in new regime
- Increase activity as new regime confirms

**Warning signs of regime change**:
- Previous support/resistance fails
- Volatility pattern changes
- Win rate suddenly drops
- Strategy behavior changes

## Indicator Reliability by Regime

Different indicators work better in different regimes:

**Trending markets**:
- Moving average crossovers: reliable
- Momentum indicators: reliable
- RSI: less reliable (can stay overbought/oversold)
- Mean reversion: not reliable

**Ranging markets**:
- RSI: very reliable
- Bollinger Bands: very reliable
- Mean reversion: reliable
- Momentum: not reliable

**High volatility**:
- Most indicators less reliable
- ATR is useful for sizing
- Wait for stability

## Key Principles

1. **No universal parameters**: What works in one regime fails in another
2. **Regime recognition comes first**: Before any trade, assess regime
3. **Adapt quickly**: Markets change, parameters must change too
4. **Defensive in uncertainty**: When regime unclear, be conservative
5. **Respect regime transitions**: Old rules may not apply
