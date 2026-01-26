#!/bin/bash
# 8-Hour Trader Test with Tuner
# - Tuner runs every 5 minutes
# - Hourly reports generated
# - All output logged

cd "$(dirname "$0")/.."
BUILD_DIR="build"
LOG_DIR="logs/8h_test_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$LOG_DIR"

echo "============================================="
echo "8-Hour Trader Test Starting"
echo "Log directory: $LOG_DIR"
echo "============================================="

# Cleanup function
cleanup() {
    echo ""
    echo "[$(date '+%H:%M:%S')] Shutting down..."

    # Kill trader and tuner
    if [ -n "$TRADER_PID" ]; then
        kill $TRADER_PID 2>/dev/null
        wait $TRADER_PID 2>/dev/null
    fi
    if [ -n "$TUNER_PID" ]; then
        kill $TUNER_PID 2>/dev/null
        wait $TUNER_PID 2>/dev/null
    fi
    if [ -n "$REPORTER_PID" ]; then
        kill $REPORTER_PID 2>/dev/null
    fi

    echo "[$(date '+%H:%M:%S')] Final report:"
    generate_report "FINAL"

    echo ""
    echo "Test completed. Logs in: $LOG_DIR"
    exit 0
}

trap cleanup SIGINT SIGTERM

# Generate hourly report function
generate_report() {
    local label="$1"
    local report_file="$LOG_DIR/report_$(date +%H%M%S).txt"

    echo ""
    echo "========== $label REPORT [$(date '+%Y-%m-%d %H:%M:%S')] =========="

    # Get portfolio state from observer
    if [ -f "$BUILD_DIR/trader_observer" ]; then
        timeout 2 "$BUILD_DIR/trader_observer" --snapshot 2>/dev/null | head -50
    fi

    # Check tuner stats
    if [ -f /dev/shm/trader_symbol_configs ]; then
        echo ""
        echo "--- Tuner Stats ---"
        echo "Tune count: $(cat /proc/$(pgrep -f trader_tuner)/status 2>/dev/null | grep -i state || echo 'N/A')"
    fi

    echo "=================================================="
    echo ""
}

# Export for subshell
export -f generate_report
export LOG_DIR BUILD_DIR

# 1. Start Trader
echo "[$(date '+%H:%M:%S')] Starting Trader..."
"$BUILD_DIR/trader" --paper --tuner-mode > "$LOG_DIR/trader.log" 2>&1 &
TRADER_PID=$!
sleep 3

if ! kill -0 $TRADER_PID 2>/dev/null; then
    echo "ERROR: Trader failed to start"
    cat "$LOG_DIR/trader.log"
    exit 1
fi
echo "[$(date '+%H:%M:%S')] Trader started (PID: $TRADER_PID)"

# 2. Start Tuner (5-minute interval)
echo "[$(date '+%H:%M:%S')] Starting Tuner (5-min interval)..."
"$BUILD_DIR/trader_tuner" --interval 300 > "$LOG_DIR/tuner.log" 2>&1 &
TUNER_PID=$!
sleep 2

if ! kill -0 $TUNER_PID 2>/dev/null; then
    echo "WARNING: Tuner failed to start"
    cat "$LOG_DIR/tuner.log"
else
    echo "[$(date '+%H:%M:%S')] Tuner started (PID: $TUNER_PID)"
fi

# 3. Hourly reporter (background)
echo "[$(date '+%H:%M:%S')] Starting hourly reporter..."
(
    HOUR=0
    while [ $HOUR -lt 8 ]; do
        sleep 3600  # 1 hour
        HOUR=$((HOUR + 1))
        generate_report "HOUR $HOUR/8"
    done
) &
REPORTER_PID=$!

# Initial report
sleep 5
generate_report "INITIAL"

echo ""
echo "Test running for 8 hours..."
echo "Press Ctrl+C to stop early"
echo ""

# Wait for 8 hours
sleep 28800  # 8 hours in seconds

cleanup
