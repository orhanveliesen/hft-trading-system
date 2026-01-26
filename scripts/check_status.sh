#!/bin/bash
cd /mnt/c/Users/orhan/projects/hft/build

echo "=== Trader 8-Hour Test Status ==="
echo "Time: $(date)"
echo ""

# Process check
TRADER_PID=$(pgrep -f "^./trader --paper" 2>/dev/null)
TUNER_PID=$(pgrep -f "trader_tuner" 2>/dev/null)
echo "Processes:"
echo "  Trader: ${TRADER_PID:-NOT RUNNING}"
echo "  Tuner: ${TUNER_PID:-NOT RUNNING}"
echo ""

# Config summary
echo "Current Config:"
./trader_control status 2>/dev/null | grep -E "(target_pct|stop_pct|cooldown_ms|base_position)" | head -5
echo ""

# Recent tuning
echo "Recent AI Decisions:"
tail -3 ../logs/tuning_history_*.log 2>/dev/null | grep "2026-01-21"
echo ""

# Progress
echo "Progress Log (last entry):"
tail -5 ../logs/progress_8h_*.log 2>/dev/null | head -3
