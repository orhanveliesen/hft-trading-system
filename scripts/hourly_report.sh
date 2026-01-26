#!/bin/bash
# Trader Hourly Report Script

REPORT_DIR="/mnt/c/Users/orhan/projects/hft/logs/reports"
mkdir -p "$REPORT_DIR"

HOUR=$1
if [ -z "$HOUR" ]; then
    HOUR=$(date +%H)
fi

TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')
REPORT_FILE="$REPORT_DIR/report_hour_${HOUR}.txt"

echo "════════════════════════════════════════════════════════════════" > "$REPORT_FILE"
echo "         TRADER HOURLY REPORT - HOUR $HOUR" >> "$REPORT_FILE"
echo "         $TIMESTAMP" >> "$REPORT_FILE"
echo "════════════════════════════════════════════════════════════════" >> "$REPORT_FILE"

# Get portfolio data from API
DATA=$(curl -s http://localhost:8888/api/portfolio 2>/dev/null)

if [ -z "$DATA" ] || [ "$DATA" == "" ]; then
    echo "ERROR: Could not fetch portfolio data" >> "$REPORT_FILE"
    cat "$REPORT_FILE"
    exit 1
fi

# Parse JSON with Python
python3 << PYTHON >> "$REPORT_FILE"
import json
import sys

data = json.loads('''$DATA''')

cash = data.get('cash', 0)
realized_pnl = data.get('total_realized_pnl', 0)
unrealized_pnl = data.get('total_unrealized_pnl', 0)
total_equity = data.get('total_equity', 0)

# Cost assumptions (pessimistic)
SLIPPAGE_BPS = 10  # 0.1%
COMMISSION_PCT = 0.15  # 0.15%
ROUND_TRIP_COST = (SLIPPAGE_BPS / 100 + COMMISSION_PCT) * 2  # ~0.5% per round trip

net_pnl = realized_pnl + unrealized_pnl

print(f"""
PORTFOLIO SUMMARY:
─────────────────────────────────────────────────────
  Cash Balance:       \${cash:>12,.2f}
  Total Equity:       \${total_equity:>12,.2f}
─────────────────────────────────────────────────────
  Realized P&L:       \${realized_pnl:>+12,.2f}
  Unrealized P&L:     \${unrealized_pnl:>+12,.2f}
─────────────────────────────────────────────────────
  NET P&L:            \${net_pnl:>+12,.2f}
═════════════════════════════════════════════════════

COST SETTINGS (Pessimistic):
─────────────────────────────────────────────────────
  Slippage:           {SLIPPAGE_BPS} bps (0.1%)
  Commission:         {COMMISSION_PCT}%
  Round-trip cost:    ~{ROUND_TRIP_COST:.2f}%
─────────────────────────────────────────────────────

OPEN POSITIONS:
─────────────────────────────────────────────────────""")

positions = data.get('positions', [])
total_position_value = 0
for p in positions:
    qty = p.get('quantity', 0)
    if qty > 0:
        symbol = p.get('symbol', 'UNKNOWN')
        price = p.get('current_price', 0)
        unrealized = p.get('unrealized_pnl', 0)
        value = qty * price
        total_position_value += value
        print(f"  {symbol:<12} {qty:>10.4f} @ \${price:>10.2f} = \${value:>10.2f} (P&L: \${unrealized:>+8.2f})")

print(f"""─────────────────────────────────────────────────────
  Total Position Value: \${total_position_value:>12,.2f}
═════════════════════════════════════════════════════
""")

# Performance metrics
if net_pnl >= 0:
    print("RESULT: ✅ PROFIT")
else:
    print("RESULT: ❌ LOSS")

PYTHON

echo "" >> "$REPORT_FILE"
echo "════════════════════════════════════════════════════════════════" >> "$REPORT_FILE"

# Display report
cat "$REPORT_FILE"

# Also save to main log
cat "$REPORT_FILE" >> "$REPORT_DIR/all_reports.log"

