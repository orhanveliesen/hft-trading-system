# Crypto Market Characteristics

## Market Context

This trading system operates on **Binance cryptocurrency markets**. All symbols (BTCUSDT, ETHUSDT, BNBUSDT, SOLUSDT, XRPUSDT, ADAUSDT, DOGEUSDT, NEOUSDT) are crypto assets traded against USDT.

## Key Differences from Stock Markets

### 1. Higher Volatility
- Crypto markets have significantly higher volatility than traditional stock markets
- BTC can swing 3-5% in a single hour during normal conditions
- Altcoins can swing 10-20% in a day
- Flash crashes of 10%+ can occur in minutes

### 2. 24/7 Trading
- Crypto markets never close
- Volatility can spike at any time
- Weekend and holiday trading is active

### 3. Liquidity Characteristics
- Liquidity varies significantly by time of day
- Asian trading hours often see different patterns than US hours
- Major news events can drain liquidity instantly

## Parameter Recommendations for Crypto

### Stop Loss Settings
- **Minimum recommended**: 5% (to avoid frequent stop-outs)
- **Moderate risk**: 7-10%
- **Aggressive (experienced only)**: 3-5%
- **Never use**: < 3% stops (will trigger on normal volatility)

### Take Profit Settings
- **Conservative**: 2-3%
- **Moderate**: 3-5%
- **Aggressive**: 5-10%

### Risk/Reward Ratio
- For crypto, accept lower R:R ratios (0.5-1.0) due to higher volatility
- Tight stops with high targets rarely work in crypto

## Tuning Guidelines for AI Tuner

### When to WIDEN Stops
- High volatility detected (regime = HIGH_VOL)
- Multiple stop losses hit in short period
- During news events or market uncertainty
- Win rate < 30% with current settings

### When to TIGHTEN Stops
- Market trending strongly in one direction
- Low volatility period (regime = LOW_VOL)
- High win rate (> 60%) sustained

### Conservative Approach
When starting a new session or after losses:
1. Use wider stops (7-10%)
2. Use smaller position sizes (1-2% of capital)
3. Wait for at least 10-20 trades before adjusting
4. Observe market conditions before acting aggressively

### DO NOT
- Tighten stops below 3% for any crypto asset
- Use tight stops during high volatility
- React to single trade outcomes
- Make multiple parameter changes simultaneously

## Symbol-Specific Notes

| Symbol | Volatility | Recommended Stop |
|--------|------------|------------------|
| BTCUSDT | Medium-High | 5-7% |
| ETHUSDT | High | 5-8% |
| BNBUSDT | Medium | 5-7% |
| SOLUSDT | Very High | 7-10% |
| XRPUSDT | High | 6-10% |
| ADAUSDT | High | 7-10% |
| DOGEUSDT | Very High | 8-12% |
| NEOUSDT | Very High | 8-12% |

## Summary

**Key Principle**: In crypto markets, it's better to have wider stops and miss some trades than to get stopped out constantly on normal volatility. The tuner should observe patterns over many trades before making adjustments, and always err on the side of caution with stop loss settings.
