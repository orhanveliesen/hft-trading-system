# BUG-001: Web API Portfolio Equity Calculation Wrong

## Priority: HIGH

## Location
`tools/trader_web_api.cpp:478-479`

## Description
The web API `/portfolio` endpoint returns incorrect `total_equity` value. It uses the wrong formula which causes reported equity to be significantly lower than actual.

## Current Code (WRONG)
```cpp
json.kv("total_equity", portfolio_state_->cash() +
                        portfolio_state_->total_unrealized_pnl());
```

## Expected Code (CORRECT)
```cpp
json.kv("total_equity", portfolio_state_->total_equity());
```

Or alternatively:
```cpp
json.kv("total_equity", portfolio_state_->cash() +
                        portfolio_state_->total_market_value());
```

## Impact
- Web API reports equity ~$56k lower than actual
- Misleading portfolio data for any consumers of the API
- Dashboard/monitoring tools may show incorrect values

## Root Cause
`total_equity` should be `cash + position_value` (market value of holdings), not `cash + unrealized_pnl`.

Formula:
- `unrealized_pnl = current_value - cost_basis`
- `total_equity = cash + current_value` (NOT cash + unrealized_pnl)

## Test Required
1. Create portfolio with known positions
2. Call `/portfolio` endpoint
3. Verify `total_equity` equals `cash + sum(position_qty * current_price)`

## Found
- Date: 2026-02-18
- Session: Paper trading 2h session
- Discovered by: Comparing web API output vs shared memory state
