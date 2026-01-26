#!/bin/bash
# Simple progress tracker for 8-hour test
# Logs portfolio and tuning status every 10 minutes

cd /mnt/c/Users/orhan/projects/hft/build

LOG_FILE="../logs/progress_8h_$(date +%Y%m%d_%H%M%S).log"
DURATION_HOURS=8
START_TIME=$(date +%s)
END_TIME=$((START_TIME + DURATION_HOURS * 3600))
REPORT_INTERVAL=600  # 10 minutes

echo "======================================================" | tee "$LOG_FILE"
echo " Trader 8-Hour Test - Progress Tracker" | tee -a "$LOG_FILE"
echo " Started: $(date)" | tee -a "$LOG_FILE"
echo " Target: 1% profit (\$1,000 on \$100k capital)" | tee -a "$LOG_FILE"
echo " Tuning interval: 5 minutes" | tee -a "$LOG_FILE"
echo " Report interval: 10 minutes" | tee -a "$LOG_FILE"
echo "======================================================" | tee -a "$LOG_FILE"

REPORT_NUM=0

while [ $(date +%s) -lt $END_TIME ]; do
    REPORT_NUM=$((REPORT_NUM + 1))
    ELAPSED_SECS=$(( $(date +%s) - START_TIME ))
    ELAPSED_MINS=$((ELAPSED_SECS / 60))
    ELAPSED_HOURS=$(echo "scale=2; $ELAPSED_MINS / 60" | bc)

    echo "" | tee -a "$LOG_FILE"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" | tee -a "$LOG_FILE"
    echo "REPORT #$REPORT_NUM @ $(date '+%H:%M:%S') (${ELAPSED_HOURS}h elapsed)" | tee -a "$LOG_FILE"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" | tee -a "$LOG_FILE"

    # Check if processes are running
    TRADER_PID=$(pgrep -f "^./trader --paper" 2>/dev/null)
    TUNER_PID=$(pgrep -f "trader_tuner" 2>/dev/null)

    echo "Processes: Trader=$TRADER_PID, Tuner=$TUNER_PID" | tee -a "$LOG_FILE"

    if [ -z "$TRADER_PID" ]; then
        echo "⚠ WARNING: Trader process not running!" | tee -a "$LOG_FILE"
    fi

    # Get tuning history summary
    TUNE_COUNT=$(wc -l < ../logs/tuning_history_*.log 2>/dev/null || echo "0")
    echo "Tuning decisions so far: $TUNE_COUNT" | tee -a "$LOG_FILE"

    # Get latest tuning decision
    echo "" | tee -a "$LOG_FILE"
    echo "Latest tuning decision:" | tee -a "$LOG_FILE"
    tail -1 ../logs/tuning_history_*.log 2>/dev/null | tee -a "$LOG_FILE"

    # Get current config from shared memory (via trader_control)
    echo "" | tee -a "$LOG_FILE"
    echo "Current config:" | tee -a "$LOG_FILE"
    ./trader_control show 2>/dev/null | head -20 | tee -a "$LOG_FILE"

    echo "" | tee -a "$LOG_FILE"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" | tee -a "$LOG_FILE"

    # Wait for next report
    sleep $REPORT_INTERVAL
done

echo "" | tee -a "$LOG_FILE"
echo "======================================================" | tee -a "$LOG_FILE"
echo " TEST COMPLETED at $(date)" | tee -a "$LOG_FILE"
echo "======================================================" | tee -a "$LOG_FILE"

# Final summary
echo "" | tee -a "$LOG_FILE"
echo "FINAL TUNING HISTORY:" | tee -a "$LOG_FILE"
cat ../logs/tuning_history_*.log 2>/dev/null | tee -a "$LOG_FILE"
