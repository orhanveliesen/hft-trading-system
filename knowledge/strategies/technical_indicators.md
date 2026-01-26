# Technical Indicators Strategy

## Overview
The Technical Indicators Strategy uses a combination of momentum and mean-reversion indicators to generate trading signals. It is the default strategy in non-tuner mode.

## Indicators Used

### EMA Crossover (Trend Detection)
- **Fast EMA**: 12-period exponential moving average
- **Slow EMA**: 26-period exponential moving average
- **Signal**: Fast EMA crossing above Slow EMA = bullish, below = bearish

### RSI (Relative Strength Index)
- **Period**: 14 (configurable)
- **Overbought**: > 70 (default, configurable `rsi_overbought`)
- **Oversold**: < 30 (default, configurable `rsi_oversold`)
- **Signal**: RSI < oversold = buy opportunity, RSI > overbought = sell opportunity

### Bollinger Bands
- **Period**: 20
- **Standard Deviations**: 2
- **Signal**: Price below lower band = oversold, above upper band = overbought

## Signal Generation Rules

### Buy Signal Strength
1. **Strong Buy**: RSI oversold + Price below lower BB + EMA bullish crossover
2. **Medium Buy**: RSI < 40 + Price near lower BB
3. **Weak Buy**: Any single bullish indicator

### Sell Signal Strength
1. **Strong Sell**: RSI overbought + Price above upper BB + EMA bearish crossover
2. **Medium Sell**: RSI > 60 + Price near upper BB
3. **Weak Sell**: Any single bearish indicator

## Regime-Specific Behavior

### Trending Up
- Follow trend, buy on pullbacks to EMA
- Use looser EMA deviation threshold (1-2%)
- Favor momentum signals

### Trending Down
- No new buys, only exits
- Wait for trend reversal signal
- Tighten stop losses

### Ranging Market
- Mean reversion strategy
- Buy at lower BB, sell at upper BB
- Tighter EMA deviation (0.5%)

### High Volatility
- Require stronger signals (only Strong buy/sell)
- Reduce position sizes
- Tighten EMA deviation (0.2%)

## Parameter Tuning Guidelines

| Parameter | Conservative | Normal | Aggressive |
|-----------|-------------|--------|------------|
| RSI Oversold | 20 | 30 | 40 |
| RSI Overbought | 80 | 70 | 60 |
| EMA Dev Trending | 0.5% | 1.0% | 2.0% |
| EMA Dev Ranging | 0.2% | 0.5% | 1.0% |
| Signal Strength | Strong only | Medium+ | Any |

## When to Adjust Parameters

- **After 3+ consecutive losses**: Tighten RSI thresholds, increase signal strength requirement
- **Win rate > 60%**: Can relax thresholds slightly
- **High volatility detected**: Require Strong signals only
- **Low volatility/ranging**: Use mean-reversion mode
