#!/bin/bash
# 8-hour progress tracker - logs every 10 minutes
# Target: 1% profit with good risk management

LOG_FILE="../logs/progress_$(date +%Y%m%d_%H%M%S).log"
DURATION_HOURS=8
INTERVAL_MINS=10
START_TIME=$(date +%s)
END_TIME=$((START_TIME + DURATION_HOURS * 3600))

echo "========================================" | tee -a "$LOG_FILE"
echo "Trader 8-Hour Test Progress Tracker" | tee -a "$LOG_FILE"
echo "Start: $(date)" | tee -a "$LOG_FILE"
echo "Target: 1% profit (\$1,000 on \$100k)" | tee -a "$LOG_FILE"
echo "Tracking interval: ${INTERVAL_MINS} minutes" | tee -a "$LOG_FILE"
echo "========================================" | tee -a "$LOG_FILE"

while [ $(date +%s) -lt $END_TIME ]; do
    ELAPSED_MINS=$(( ($(date +%s) - START_TIME) / 60 ))
    ELAPSED_HOURS=$(echo "scale=2; $ELAPSED_MINS / 60" | bc)

    echo "" | tee -a "$LOG_FILE"
    echo "----------------------------------------" | tee -a "$LOG_FILE"
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Progress Report (${ELAPSED_HOURS}h elapsed)" | tee -a "$LOG_FILE"
    echo "----------------------------------------" | tee -a "$LOG_FILE"

    # Get portfolio stats via observer (quick mode)
    if [ -f /dev/shm/trader_portfolio ]; then
        # Use trader_observer to get status
        timeout 5 ./trader_observer --once 2>/dev/null | head -30 | tee -a "$LOG_FILE"
    else
        echo "Portfolio shared memory not available" | tee -a "$LOG_FILE"
    fi

    # Get recent tuning decisions
    echo "" | tee -a "$LOG_FILE"
    echo "Recent Tuning (last 5):" | tee -a "$LOG_FILE"
    tail -5 ../logs/tuning_history_*.log 2>/dev/null | tee -a "$LOG_FILE"

    # Calculate progress toward 1% target
    echo "" | tee -a "$LOG_FILE"
    echo "----------------------------------------" | tee -a "$LOG_FILE"

    sleep $((INTERVAL_MINS * 60))
done

echo "" | tee -a "$LOG_FILE"
echo "========================================" | tee -a "$LOG_FILE"
echo "TEST COMPLETED at $(date)" | tee -a "$LOG_FILE"
echo "========================================" | tee -a "$LOG_FILE"
