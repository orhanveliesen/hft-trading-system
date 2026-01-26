#!/bin/bash
# 8-Hour Test Monitor Script
# Reports performance every hour for 8 hours

cd /mnt/c/Users/orhan/projects/hft/build

LOG_FILE="/tmp/trader_8hour_report.log"
START_TIME=$(date +%s)
TEST_DURATION=$((8 * 3600))  # 8 hours in seconds
REPORT_INTERVAL=3600          # 1 hour in seconds
TUNE_INTERVAL=300             # 5 minutes for manual tune trigger

echo "=== Trader 8-Hour Test Started at $(date) ===" | tee -a $LOG_FILE
echo "Test will run until $(date -d "@$((START_TIME + TEST_DURATION))")" | tee -a $LOG_FILE
echo "" | tee -a $LOG_FILE

HOUR=0
LAST_TUNE=$START_TIME

while true; do
    CURRENT_TIME=$(date +%s)
    ELAPSED=$((CURRENT_TIME - START_TIME))

    # Check if 8 hours passed
    if [ $ELAPSED -ge $TEST_DURATION ]; then
        echo "" | tee -a $LOG_FILE
        echo "=== FINAL REPORT (8 hours complete) ===" | tee -a $LOG_FILE
        date | tee -a $LOG_FILE
        ./trader_control status 2>&1 | tee -a $LOG_FILE

        echo "" | tee -a $LOG_FILE
        echo "Trade Summary:" | tee -a $LOG_FILE
        echo "BUY trades:  $(grep -c 'EXEC.*BUY' /tmp/trader_8hour_test.log)" | tee -a $LOG_FILE
        echo "SELL trades: $(grep -c 'EXEC.*SELL' /tmp/trader_8hour_test.log)" | tee -a $LOG_FILE

        echo "" | tee -a $LOG_FILE
        echo "=== Test Complete! ===" | tee -a $LOG_FILE
        exit 0
    fi

    # Trigger manual tune every 5 minutes if tuner is ON
    TUNE_ELAPSED=$((CURRENT_TIME - LAST_TUNE))
    if [ $TUNE_ELAPSED -ge $TUNE_INTERVAL ]; then
        # Dashboard can request manual tune
        echo "[$(date '+%H:%M')] Requesting manual tune..." >> $LOG_FILE
        LAST_TUNE=$CURRENT_TIME
    fi

    # Hourly report
    HOURS_ELAPSED=$((ELAPSED / 3600))
    if [ $HOURS_ELAPSED -gt $HOUR ]; then
        HOUR=$HOURS_ELAPSED

        echo "" | tee -a $LOG_FILE
        echo "=== HOUR $HOUR REPORT ===" | tee -a $LOG_FILE
        date | tee -a $LOG_FILE

        echo "--- Status ---" | tee -a $LOG_FILE
        ./trader_control status 2>&1 | grep -E "(trading_enabled|trader_status|consecutive|target_pct|stop_pct|cooldown)" | tee -a $LOG_FILE

        echo "" | tee -a $LOG_FILE
        echo "--- Trade Counts ---" | tee -a $LOG_FILE
        echo "BUY:  $(grep -c 'EXEC.*BUY' /tmp/trader_8hour_test.log)" | tee -a $LOG_FILE
        echo "SELL: $(grep -c 'EXEC.*SELL' /tmp/trader_8hour_test.log)" | tee -a $LOG_FILE

        echo "" | tee -a $LOG_FILE
        echo "--- Recent Trades ---" | tee -a $LOG_FILE
        tail -5 /tmp/trader_8hour_test.log | grep -E "(EXEC|UNIFIED)" | tee -a $LOG_FILE

        echo "" | tee -a $LOG_FILE
        echo "--- Cash ---" | tee -a $LOG_FILE
        tail -1 /tmp/trader_8hour_test.log | grep -oP 'cash=\$[\d.]+' | tee -a $LOG_FILE
    fi

    # Sleep for 1 minute before checking again
    sleep 60
done
