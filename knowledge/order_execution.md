# Order Execution Strategies

## Order Type Selection

The HFT system supports four order type preferences that can be controlled by the AI tuner:

### Auto (Default)
- ExecutionEngine decides based on multiple factors:
  - Signal strength (strong = market, weak = limit)
  - Market regime (high volatility = market, ranging = limit)
  - Spread width (wide spread = limit, tight spread = market)
- Best for: General trading when no specific preference

### MarketOnly
- Always uses market orders for immediate execution
- Advantages:
  - Guaranteed fill (assuming liquidity)
  - Best for urgent entries/exits
  - Essential during high volatility
- Disadvantages:
  - Pays the spread (slippage)
  - Higher trading costs
- When to use:
  - Strong trend following signals
  - Risk-off situations requiring immediate exit
  - High volatility environments where prices move fast
  - When fill certainty > cost savings

### LimitOnly
- Always uses limit orders at calculated prices
- Advantages:
  - No slippage (fill at specified price or better)
  - Maker rebates on many exchanges
  - Lower effective trading costs
- Disadvantages:
  - May not fill (missed opportunities)
  - Adverse selection (fills when price moving against you)
- When to use:
  - Market making strategies
  - Low volatility, ranging markets
  - When time-to-fill is not critical
  - Cost minimization priority

### Adaptive
- Starts with limit order, converts to market after timeout
- Best of both worlds approach
- Configuration:
  - `limit_timeout_ms`: How long to wait for limit fill
  - Shorter timeout = more aggressive (closer to market behavior)
  - Longer timeout = more passive (closer to limit behavior)
- When to use:
  - Medium urgency trades
  - When you want to try for better price but need guaranteed execution
  - Balancing fill probability vs cost

## Limit Price Calculation

For limit orders, the price is calculated inside the spread:

```
Buy Limit = Bid + (Spread * offset_bps / 100)
Sell Limit = Ask - (Spread * offset_bps / 100)
```

The `limit_offset_bps` parameter controls aggressiveness:
- Lower offset (2-5 bps): More aggressive, faster fills, slightly worse price
- Higher offset (5-10 bps): Less aggressive, slower fills, better price

## Cost Analysis

### Market Order Costs
1. **Spread Cost**: Half the bid-ask spread per side
   - Entry: pay ask (mid + half spread)
   - Exit: receive bid (mid - half spread)
   - Total: full spread cost for round trip

2. **Slippage**: Additional adverse price movement
   - Typically 5-10 bps in normal conditions
   - Can be 20-50 bps during volatility

### Limit Order Costs
1. **No Spread Cost**: Fill at limit price
2. **No Slippage**: Price is guaranteed
3. **Opportunity Cost**: Missed trades when not filled
4. **Adverse Selection**: Filled trades may be losers

## AI Tuner Decision Framework

The AI should consider:

### Use MarketOnly when:
- High slippage_cost_total relative to PnL
- Symbol is trending strongly (need quick entry)
- Position is underwater and needs risk reduction
- Volatility is increasing (spread likely to widen)

### Use LimitOnly when:
- Slippage costs are significant portion of profits
- Market is ranging (prices oscillating)
- Strategy is mean-reversion based
- Operating as market maker

### Use Adaptive when:
- Uncertain about market direction
- Want to capture better price but ensure execution
- Normal market conditions
- Moderate urgency

### Use Auto when:
- Let ExecutionEngine make regime-based decisions
- New symbol with unknown characteristics
- Testing/baseline comparison

## Monitoring Metrics

Track these to optimize order type selection:

1. **Fill Rate**: Percentage of limit orders that fill
   - Low fill rate = offset too aggressive or market too fast

2. **Slippage per Trade**: Cost of market order execution
   - High slippage = consider more limit orders

3. **Time to Fill**: Average time for limit orders to fill
   - Long time = consider more aggressive offset

4. **Adverse Selection Rate**: Percentage of limit fills that become losers
   - High rate = market is smarter, use market orders

## Per-Symbol Configuration

Each symbol can have different order type preferences:

```json
{
  "symbol": "BTCUSDT",
  "order_type": "LimitOnly",
  "limit_offset_bps": 3.0,
  "limit_timeout_ms": 500
}
```

This allows:
- BTCUSDT: LimitOnly (high liquidity, tight spreads)
- ETHUSDT: Adaptive (moderate liquidity)
- ALTCOIN: MarketOnly (low liquidity, need guaranteed fills)

## Implementation Notes

### SharedConfig Fields
- `order_type_default`: Global default (0=Auto, 1=Market, 2=Limit, 3=Adaptive)
- `limit_offset_bps_x100`: Default limit offset (stored as bps * 100)
- `limit_timeout_ms`: Default adaptive timeout

### SymbolTuningConfig Fields
- `order_type_preference`: Per-symbol override
- `limit_offset_bps_x100`: Per-symbol limit offset
- `limit_timeout_ms`: Per-symbol timeout

### Signal Fields
- `order_pref`: OrderPreference enum (Market, Limit, Either)
- `limit_price`: Calculated limit price (0 = auto-calculate)
