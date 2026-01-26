#!/bin/bash
#
# 6-Hour Trader Test with AI Tuner
# Usage: ./run_6h_test.sh [API_KEY]
#
# Kullanım:
#   export CLAUDE_API_KEY="sk-ant-..."
#   ./run_6h_test.sh
#

set -e

# Configuration
DURATION_HOURS=6
DURATION_SECS=$((DURATION_HOURS * 3600))
REPORT_INTERVAL=3600  # 1 hour
TUNER_INTERVAL=300    # 5 minutes

# Pessimistic trading parameters
SLIPPAGE_BPS=10       # 10 bps = 0.1% (pessimistic)
COMMISSION_PCT=0.1    # 0.1% Binance taker fee
INITIAL_CAPITAL=100000

# Directories
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
LOG_DIR="$PROJECT_DIR/logs/test_$(date +%Y%m%d_%H%M%S)"

# Create log directory
mkdir -p "$LOG_DIR"

# Log files
TRADER_LOG="$LOG_DIR/trader.log"
TUNER_LOG="$LOG_DIR/tuner.log"
REPORT_LOG="$LOG_DIR/hourly_reports.log"
SUMMARY_LOG="$LOG_DIR/summary.log"
AI_DECISIONS_LOG="$LOG_DIR/ai_decisions.log"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

log() {
    echo -e "${BLUE}[$(date '+%Y-%m-%d %H:%M:%S')]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[$(date '+%Y-%m-%d %H:%M:%S')]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[$(date '+%Y-%m-%d %H:%M:%S')]${NC} $1"
}

log_error() {
    echo -e "${RED}[$(date '+%Y-%m-%d %H:%M:%S')]${NC} $1"
}

log_ai() {
    echo -e "${CYAN}[AI]${NC} $1"
}

# API Key handling
if [ -n "$1" ]; then
    export CLAUDE_API_KEY="$1"
fi

if [ -z "$CLAUDE_API_KEY" ]; then
    log_error "CLAUDE_API_KEY not set!"
    log_error "Usage: export CLAUDE_API_KEY=<key> && $0"
    log_error "   or: $0 <API_KEY>"
    exit 1
fi

# Check build
if [ ! -f "$BUILD_DIR/trader" ] || [ ! -f "$BUILD_DIR/trader_tuner" ]; then
    log_error "Build not found. Run: cd build && cmake .. && make -j\$(nproc)"
    exit 1
fi

cd "$BUILD_DIR"

log "════════════════════════════════════════════════════════════"
log "  6-HOUR TRADER TEST WITH AI TUNER"
log "════════════════════════════════════════════════════════════"
log ""
log "Configuration:"
log "  Duration:     $DURATION_HOURS hours ($DURATION_SECS seconds)"
log "  Slippage:     $SLIPPAGE_BPS bps (pessimistic - market orders)"
log "  Commission:   $COMMISSION_PCT% (Binance taker)"
log "  Capital:      \$$INITIAL_CAPITAL"
log "  AI Tuner:     Every $TUNER_INTERVAL seconds"
log "  Reports:      Every $REPORT_INTERVAL seconds"
log ""
log "Log Directory: $LOG_DIR"
log ""

# Start Trader in background first (creates shared memory)
log "Starting Trader engine with verbose logging..."
./trader --paper -v -c $INITIAL_CAPITAL -d $DURATION_SECS > "$TRADER_LOG" 2>&1 &
TRADER_PID=$!
log_success "Trader started (PID: $TRADER_PID)"

# Wait for Trader to initialize shared memory
sleep 5

# Check if Trader is running
if ! kill -0 $TRADER_PID 2>/dev/null; then
    log_error "Trader failed to start! Check $TRADER_LOG"
    tail -20 "$TRADER_LOG"
    exit 1
fi

# Configure pessimistic parameters
log "Setting pessimistic parameters..."
./trader_control set slippage_bps $SLIPPAGE_BPS 2>/dev/null && log "  Slippage: $SLIPPAGE_BPS bps" || true
./trader_control set commission $COMMISSION_PCT 2>/dev/null && log "  Commission: $COMMISSION_PCT%" || true

# Enable tuner mode for unified strategy
./trader_control set tuner_mode 1 2>/dev/null && log "  Tuner mode: enabled" || true

# Show initial config
log ""
log "Initial configuration:"
./trader_control status 2>/dev/null | tee "$SUMMARY_LOG"

# Start AI Tuner with verbose logging
log ""
log "Starting AI Tuner..."
export TRADER_TUNER_MODEL="claude-sonnet-4-20250514"

./trader_tuner --verbose --interval $TUNER_INTERVAL > "$TUNER_LOG" 2>&1 &
TUNER_PID=$!
log_success "AI Tuner started (PID: $TUNER_PID)"

# Cleanup function
cleanup() {
    log_warn ""
    log_warn "Stopping processes..."
    kill $TRADER_PID 2>/dev/null || true
    kill $TUNER_PID 2>/dev/null || true

    # Final report
    log ""
    log "════════════════════════════════════════════════════════════"
    log "  FINAL SUMMARY"
    log "════════════════════════════════════════════════════════════"

    generate_report "FINAL"

    log ""
    log "All logs saved to: $LOG_DIR"
    log "  - Trader Log:     $TRADER_LOG"
    log "  - Tuner Log:      $TUNER_LOG"
    log "  - Hourly Reports: $REPORT_LOG"
    log "  - AI Decisions:   $AI_DECISIONS_LOG"

    # Quick summary
    log ""
    log "Quick Statistics:"
    local BUYS=$(grep -c "\[BUY" "$TRADER_LOG" 2>/dev/null || echo "0")
    local FILLS=$(grep -c "\[FILL" "$TRADER_LOG" 2>/dev/null || echo "0")
    local TARGETS=$(grep -c "TARGET\|target_hit" "$TRADER_LOG" 2>/dev/null || echo "0")
    local STOPS=$(grep -c "STOP\|stop_loss" "$TRADER_LOG" 2>/dev/null || echo "0")
    local MKT=$(grep -c ":MKT\]" "$TRADER_LOG" 2>/dev/null || echo "0")
    local LMT=$(grep -c ":LMT\]" "$TRADER_LOG" 2>/dev/null || echo "0")
    local AI_CHANGES=$(grep -c "Applied\|Updated\|Changed" "$TUNER_LOG" 2>/dev/null || echo "0")

    log "  Total Buys:      $BUYS"
    log "  Total Fills:     $FILLS"
    log "  Target Hits:     $TARGETS"
    log "  Stop Losses:     $STOPS"
    log "  Market Orders:   $MKT"
    log "  Limit Orders:    $LMT"
    log "  AI Adjustments:  $AI_CHANGES"
}

trap cleanup EXIT INT TERM

# Report generation function
generate_report() {
    local REPORT_NUM=$1
    local TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

    echo ""
    echo "════════════════════════════════════════════════════════════"
    echo "  HOURLY REPORT #$REPORT_NUM"
    echo "  $TIMESTAMP"
    echo "════════════════════════════════════════════════════════════"
    echo ""

    # Get current status from trader_control
    echo "╔══════════════════════════════════════════════════════════╗"
    echo "║  PORTFOLIO STATUS                                        ║"
    echo "╚══════════════════════════════════════════════════════════╝"
    ./trader_control status 2>/dev/null || echo "Status unavailable"

    echo ""
    echo "╔══════════════════════════════════════════════════════════╗"
    echo "║  TRADER ENGINE                                           ║"
    echo "╚══════════════════════════════════════════════════════════╝"
    if kill -0 $TRADER_PID 2>/dev/null; then
        echo "Status: RUNNING (PID: $TRADER_PID)"
        echo ""
        echo "Recent Trades:"
        tail -50 "$TRADER_LOG" 2>/dev/null | grep -E "(BUY|SELL|FILL|TARGET|STOP|UNIFIED)" | tail -10 || echo "No recent trades"
    else
        echo "Status: STOPPED"
    fi

    echo ""
    echo "╔══════════════════════════════════════════════════════════╗"
    echo "║  AI TUNER ACTIVITY                                       ║"
    echo "╚══════════════════════════════════════════════════════════╝"
    if kill -0 $TUNER_PID 2>/dev/null; then
        echo "Status: RUNNING (PID: $TUNER_PID)"
        echo ""
        echo "Recent AI Decisions & Reasoning:"
        tail -200 "$TUNER_LOG" 2>/dev/null | grep -E "(Analysis|Recommendation|Reason|order_type|symbol|Applied|config)" | tail -15 || echo "No AI decisions yet"
    else
        echo "Status: STOPPED"
    fi

    echo ""
    echo "╔══════════════════════════════════════════════════════════╗"
    echo "║  SMART STRATEGY PER SYMBOL                               ║"
    echo "╚══════════════════════════════════════════════════════════╝"
    # Extract symbol-specific AI decisions
    echo "AI-controlled order types per symbol:"
    tail -500 "$TUNER_LOG" 2>/dev/null | grep -E "(BTCUSDT|ETHUSDT|BNBUSDT|SOLUSDT).*order_type" | tail -10 || echo "No symbol-specific configs yet"
    echo ""
    echo "Current symbol positions:"
    tail -100 "$TRADER_LOG" 2>/dev/null | grep -E "position|holding" | tail -5 || echo "No position data"

    echo ""
    echo "╔══════════════════════════════════════════════════════════╗"
    echo "║  TRADE STATISTICS                                        ║"
    echo "╚══════════════════════════════════════════════════════════╝"
    local BUYS=$(grep -c "\[BUY" "$TRADER_LOG" 2>/dev/null || echo "0")
    local SELLS=$(grep -c "\[SELL" "$TRADER_LOG" 2>/dev/null || echo "0")
    local TARGETS=$(grep -c "TARGET\|target_hit" "$TRADER_LOG" 2>/dev/null || echo "0")
    local STOPS=$(grep -c "STOP\|stop_loss" "$TRADER_LOG" 2>/dev/null || echo "0")
    local FILLS=$(grep -c "\[FILL" "$TRADER_LOG" 2>/dev/null || echo "0")
    local MKT_ORDERS=$(grep -c ":MKT\]" "$TRADER_LOG" 2>/dev/null || echo "0")
    local LMT_ORDERS=$(grep -c ":LMT\]" "$TRADER_LOG" 2>/dev/null || echo "0")

    printf "  %-20s %s\n" "Buys:" "$BUYS"
    printf "  %-20s %s\n" "Sells:" "$SELLS"
    printf "  %-20s %s\n" "Fills:" "$FILLS"
    printf "  %-20s %s\n" "Target Hits:" "$TARGETS"
    printf "  %-20s %s\n" "Stop Losses:" "$STOPS"
    echo ""
    printf "  %-20s %s\n" "Market Orders:" "$MKT_ORDERS"
    printf "  %-20s %s\n" "Limit Orders:" "$LMT_ORDERS"

    # Win rate calculation
    if [ $((TARGETS + STOPS)) -gt 0 ]; then
        local WIN_RATE=$(echo "scale=1; $TARGETS * 100 / ($TARGETS + $STOPS)" | bc 2>/dev/null || echo "N/A")
        printf "  %-20s %s%%\n" "Win Rate:" "$WIN_RATE"
    fi

    echo ""
    echo "╔══════════════════════════════════════════════════════════╗"
    echo "║  AI ERROR ANALYSIS                                       ║"
    echo "╚══════════════════════════════════════════════════════════╝"
    # Look for AI mistakes or errors
    tail -500 "$TUNER_LOG" 2>/dev/null | grep -iE "(error|fail|mistake|wrong|bad)" | tail -5 || echo "No errors detected"
    echo ""
    # Check for loss patterns
    echo "Recent losses (AI learning opportunities):"
    tail -200 "$TRADER_LOG" 2>/dev/null | grep -E "STOP|loss" | tail -5 || echo "No recent losses"

    echo ""
    echo "════════════════════════════════════════════════════════════"
}

# Save AI decisions periodically
save_ai_decisions() {
    local TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')
    {
        echo ""
        echo "═══════════════════════════════════════"
        echo "AI DECISIONS SNAPSHOT: $TIMESTAMP"
        echo "═══════════════════════════════════════"

        echo ""
        echo "--- Full AI Analysis (last 100 lines) ---"
        tail -100 "$TUNER_LOG" 2>/dev/null | grep -vE "^$" || true

        echo ""
        echo "--- Order Type Decisions ---"
        tail -300 "$TUNER_LOG" 2>/dev/null | grep -E "order_type" || true

        echo ""
        echo "--- Parameter Changes ---"
        tail -300 "$TUNER_LOG" 2>/dev/null | grep -E "(Applied|Updated|Changed|Set)" || true

    } >> "$AI_DECISIONS_LOG"
}

# Main monitoring loop
log ""
log "════════════════════════════════════════════════════════════"
log "  MONITORING STARTED"
log "════════════════════════════════════════════════════════════"
log ""
log "Press Ctrl+C to stop early"
log ""

REPORT_COUNT=0
START_TIME=$(date +%s)
LAST_REPORT_TIME=0
LAST_AI_SAVE=0

while true; do
    # Check if Trader is still running
    if ! kill -0 $TRADER_PID 2>/dev/null; then
        log_warn "Trader process ended"
        break
    fi

    # Calculate elapsed time
    CURRENT_TIME=$(date +%s)
    ELAPSED=$((CURRENT_TIME - START_TIME))

    # Check if test duration completed
    if [ $ELAPSED -ge $DURATION_SECS ]; then
        log_success "Test duration completed!"
        break
    fi

    # Hourly report
    HOURS_ELAPSED=$((ELAPSED / 3600))
    if [ $HOURS_ELAPSED -gt $REPORT_COUNT ]; then
        REPORT_COUNT=$HOURS_ELAPSED
        log ""
        log_success "Generating hourly report #$REPORT_COUNT..."
        generate_report "$REPORT_COUNT" | tee -a "$REPORT_LOG"
        save_ai_decisions
    fi

    # Progress update every 5 minutes
    if [ $((ELAPSED % 300)) -lt 5 ] && [ $ELAPSED -gt 60 ]; then
        HOURS=$((ELAPSED / 3600))
        MINS=$(((ELAPSED % 3600) / 60))
        REMAINING=$((DURATION_SECS - ELAPSED))
        REM_HOURS=$((REMAINING / 3600))
        REM_MINS=$(((REMAINING % 3600) / 60))

        # Quick status
        BUYS=$(grep -c "\[BUY" "$TRADER_LOG" 2>/dev/null || echo "0")
        FILLS=$(grep -c "\[FILL" "$TRADER_LOG" 2>/dev/null || echo "0")

        log "Progress: ${HOURS}h ${MINS}m | Remaining: ${REM_HOURS}h ${REM_MINS}m | Buys: $BUYS | Fills: $FILLS"
    fi

    # Save AI decisions every 15 minutes
    if [ $((ELAPSED - LAST_AI_SAVE)) -ge 900 ]; then
        save_ai_decisions
        LAST_AI_SAVE=$ELAPSED
    fi

    sleep 30
done

log_success ""
log_success "6-hour test completed successfully!"
