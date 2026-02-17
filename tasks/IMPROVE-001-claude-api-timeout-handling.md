# IMPROVE-001: Claude API Timeout Handling

## Priority: LOW

## Location
`include/tuner/claude_client.hpp`

## Description
During paper trading session, observed one Claude API timeout error (code 1003). The tuner recovered automatically, but improved handling could provide better resilience.

## Error Observed
```
[ERROR] Claude API request failed: error code 1003
```

## Current Behavior
- Tuner logs the error
- Continues with next iteration after interval

## Suggested Improvements
1. **Exponential backoff**: On timeout, wait longer before retry
2. **Circuit breaker**: After N consecutive failures, pause API calls for extended period
3. **Fallback strategy**: Use cached/default parameters when API unavailable
4. **Metrics tracking**: Count API failures for monitoring

## Implementation Notes
- Non-critical: Current behavior is acceptable (recovers on next cycle)
- Consider adding retry logic with backoff before giving up on single request
- Log warning level instead of error for recoverable timeouts

## Found
- Date: 2026-02-18
- Session: Paper trading 2h session
- Impact: Minimal (recovered automatically)
