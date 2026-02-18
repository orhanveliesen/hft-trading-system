#!/bin/bash
# Paper Trading Session
# - Builds project and runs tests first
# - Clears shared memory
# - Starts trader, tuner, web API
# - Tuner runs every 5 minutes
# - Hourly reports generated
# - All output logged
#
# Usage: ./paper-trading.sh [hours] [--profile]
#        ./paper-trading.sh 8              # Run for 8 hours
#        ./paper-trading.sh 0.5            # Run for 30 minutes
#        ./paper-trading.sh 8 --profile    # Run with perf profiling
#        ./paper-trading.sh                # Default: 8 hours

set -e

# Parse arguments
HOURS="${1:-8}"
PROFILE_MODE=0

# Check for --profile flag
for arg in "$@"; do
    if [ "$arg" = "--profile" ]; then
        PROFILE_MODE=1
    fi
done

# Validate hours is a number
if ! [[ "$HOURS" =~ ^[0-9]+\.?[0-9]*$ ]]; then
    echo "Usage: $0 [hours]"
    echo "  hours: Duration in hours (default: 8)"
    echo ""
    echo "Examples:"
    echo "  $0 8      # Run for 8 hours"
    echo "  $0 0.5    # Run for 30 minutes"
    echo "  $0 24     # Run for 24 hours"
    exit 1
fi

# Calculate duration in seconds (using awk for float support)
DURATION_SECS=$(awk "BEGIN {printf \"%.0f\", $HOURS * 3600}")
HOURS_INT=$(awk "BEGIN {printf \"%.0f\", $HOURS}")

cd "$(dirname "$0")/.."

# Configuration
BUILD_DIR="build"
LOG_DIR="logs/paper_trading_$(date +%Y%m%d_%H%M%S)"
WEB_API_PORT=1234
TUNER_INTERVAL=300

mkdir -p "$LOG_DIR"

# Source .env.local if it exists
if [ -f ".env.local" ]; then
    source .env.local
fi

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log() {
    echo -e "[$(date '+%H:%M:%S')] $1"
}

log_success() {
    echo -e "${GREEN}[$(date '+%H:%M:%S')] $1${NC}"
}

log_warn() {
    echo -e "${YELLOW}[$(date '+%H:%M:%S')] $1${NC}"
}

log_error() {
    echo -e "${RED}[$(date '+%H:%M:%S')] $1${NC}"
}

echo "============================================="
echo "  Paper Trading Session (${HOURS}h)"
echo "============================================="
echo "Duration: ${HOURS} hours (${DURATION_SECS} seconds)"
echo "Log directory: $LOG_DIR"
echo ""

# ============================================
# Step 0: Build and Test
# ============================================
log "Building project..."
mkdir -p "$BUILD_DIR"

# Only run cmake configure if not already configured
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    log "Configuring CMake (first time)..."
    if ! cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release > "$LOG_DIR/cmake.log" 2>&1; then
        log_error "CMake configuration failed!"
        cat "$LOG_DIR/cmake.log"
        exit 1
    fi
fi

# Build (cmake --build handles incremental builds efficiently)
if ! cmake --build "$BUILD_DIR" -j$(nproc) > "$LOG_DIR/build.log" 2>&1; then
    log_error "Build failed!"
    tail -50 "$LOG_DIR/build.log"
    exit 1
fi
log_success "Build completed"

log "Running tests..."
if ! ctest --test-dir "$BUILD_DIR" --output-on-failure > "$LOG_DIR/tests.log" 2>&1; then
    log_error "Tests failed!"
    cat "$LOG_DIR/tests.log"
    exit 1
fi
log_success "All tests passed"

# PIDs for cleanup
TRADER_PID=""
TUNER_PID=""
WEB_API_PID=""
DASHBOARD_PID=""
REPORTER_PID=""
PERF_PID=""

# Cleanup function
cleanup() {
    echo ""
    log "Shutting down..."

    # Kill all processes
    for pid_var in PERF_PID TRADER_PID TUNER_PID WEB_API_PID DASHBOARD_PID REPORTER_PID; do
        pid=${!pid_var}
        if [ -n "$pid" ] && kill -0 $pid 2>/dev/null; then
            log "Stopping $pid_var ($pid)..."
            kill $pid 2>/dev/null
            wait $pid 2>/dev/null || true
        fi
    done

    log "Final report:"
    generate_report "FINAL"

    # Show perf results if profiling was enabled
    if [ "$PROFILE_MODE" = "1" ] && [ -f "$LOG_DIR/perf_stat.txt" ]; then
        echo ""
        echo "============================================="
        echo "  PERF PROFILING RESULTS"
        echo "============================================="
        cat "$LOG_DIR/perf_stat.txt"
        echo ""
        echo "Key metrics to watch:"
        echo "  - IPC (insn per cycle): Should be > 1.0"
        echo "  - Branch misses: Should be < 5%"
        echo "  - Cache misses: Should be < 10%"
        echo "============================================="
    fi

    echo ""
    echo "Session completed. Logs in: $LOG_DIR"
    echo "  - trader.log"
    echo "  - tuner.log"
    echo "  - web_api.log"
    [ "$PROFILE_MODE" = "1" ] && echo "  - perf_stat.txt"
    exit 0
}

trap cleanup SIGINT SIGTERM EXIT

# Generate report function
generate_report() {
    local label="$1"

    echo ""
    echo "========== $label REPORT [$(date '+%Y-%m-%d %H:%M:%S')] =========="

    # Get portfolio state from web API
    if curl -s http://localhost:$WEB_API_PORT/api/status > /dev/null 2>&1; then
        echo ""
        echo "--- Status ---"
        curl -s http://localhost:$WEB_API_PORT/api/status 2>/dev/null | head -20

        echo ""
        echo "--- Portfolio ---"
        curl -s http://localhost:$WEB_API_PORT/api/portfolio 2>/dev/null | head -30

        echo ""
        echo "--- Recent Errors ---"
        curl -s "http://localhost:$WEB_API_PORT/api/errors?limit=5" 2>/dev/null | head -20
    else
        echo "Web API not available"
    fi

    echo "=================================================="
    echo ""
}

# Export for subshell
export -f generate_report
export LOG_DIR BUILD_DIR WEB_API_PORT

# ============================================
# Step 1: Kill Existing Processes
# ============================================
log "Stopping any existing trader processes..."

pkill -f "trader --paper" 2>/dev/null && log "  Killed trader" || true
pkill -f "trader_tuner" 2>/dev/null && log "  Killed tuner" || true
pkill -f "trader_web_api" 2>/dev/null && log "  Killed web_api" || true
pkill -f "trader_dashboard" 2>/dev/null && log "  Killed dashboard" || true
sleep 1

# ============================================
# Step 2: Clear Shared Memory
# ============================================
log "Clearing shared memory..."

SHM_SEGMENTS=(
    "/trader_config"
    "/trader_portfolio"
    "/trader_events"
    "/trader_ledger"
    "/trader_symbol_configs"
    "/trader_paper_config"
    "/trader_event_log"
    "/tuner_decisions"
)

for shm in "${SHM_SEGMENTS[@]}"; do
    if [ -f "/dev/shm$shm" ]; then
        rm -f "/dev/shm$shm"
        log "  Removed $shm"
    fi
done
log_success "Shared memory cleared"

# ============================================
# Step 3: Check CLAUDE_API_KEY
# ============================================
if [ -z "$CLAUDE_API_KEY" ]; then
    log_warn "CLAUDE_API_KEY not set - tuner will not work!"
    log_warn "Set it with: export CLAUDE_API_KEY='your-key'"
    echo ""
    read -p "Continue without tuner? (y/N) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
    SKIP_TUNER=1
else
    log_success "CLAUDE_API_KEY is set"
    SKIP_TUNER=0
fi

# ============================================
# Step 4: Start Trader
# ============================================
log "Starting Trader (paper mode, ${HOURS}h)..."
"$BUILD_DIR/trader" --paper -d "$DURATION_SECS" --verbose > "$LOG_DIR/trader.log" 2>&1 &
TRADER_PID=$!
sleep 3

if ! kill -0 $TRADER_PID 2>/dev/null; then
    log_error "Trader failed to start!"
    cat "$LOG_DIR/trader.log"
    exit 1
fi
log_success "Trader started (PID: $TRADER_PID)"

# ============================================
# Step 4b: Attach Perf Profiling (if --profile)
# ============================================
PERF_PID=""
if [ "$PROFILE_MODE" = "1" ]; then
    log "Attaching perf profiler..."
    perf stat -e cycles,instructions,cache-references,cache-misses,branches,branch-misses,L1-dcache-load-misses,L1-icache-load-misses,context-switches,cpu-migrations,page-faults \
        -p $TRADER_PID \
        -o "$LOG_DIR/perf_stat.txt" &
    PERF_PID=$!
    sleep 1
    if kill -0 $PERF_PID 2>/dev/null; then
        log_success "Perf profiler attached (PID: $PERF_PID)"
        log "  Output: $LOG_DIR/perf_stat.txt"
    else
        log_warn "Perf failed to attach"
    fi
fi

# ============================================
# Step 5: Start Web API
# ============================================
log "Starting Web API (port $WEB_API_PORT)..."
"$BUILD_DIR/trader_web_api" --port "$WEB_API_PORT" > "$LOG_DIR/web_api.log" 2>&1 &
WEB_API_PID=$!
sleep 2

if ! kill -0 $WEB_API_PID 2>/dev/null; then
    log_warn "Web API failed to start"
    cat "$LOG_DIR/web_api.log"
else
    log_success "Web API started (PID: $WEB_API_PID)"
    log "  Dashboard: http://localhost:$WEB_API_PORT"
    log "  Status:    http://localhost:$WEB_API_PORT/api/status"
    log "  Errors:    http://localhost:$WEB_API_PORT/api/errors"
fi

# ============================================
# Step 5: Start Tuner (if API key set)
# ============================================
if [ "$SKIP_TUNER" = "0" ]; then
    log "Starting Tuner (5-min interval)..."
    "$BUILD_DIR/trader_tuner" --interval "$TUNER_INTERVAL" > "$LOG_DIR/tuner.log" 2>&1 &
    TUNER_PID=$!
    sleep 2

    if ! kill -0 $TUNER_PID 2>/dev/null; then
        log_warn "Tuner failed to start"
        cat "$LOG_DIR/tuner.log"
    else
        log_success "Tuner started (PID: $TUNER_PID)"
    fi
else
    log_warn "Tuner skipped (no API key)"
fi

# ============================================
# Step 7: Start Dashboard (if display available)
# ============================================
if [ -n "$DISPLAY" ]; then
    log "Starting Dashboard..."
    "$BUILD_DIR/trader_dashboard" > "$LOG_DIR/dashboard.log" 2>&1 &
    DASHBOARD_PID=$!
    sleep 2

    if ! kill -0 $DASHBOARD_PID 2>/dev/null; then
        log_warn "Dashboard failed to start"
    else
        log_success "Dashboard started (PID: $DASHBOARD_PID)"
    fi
else
    log_warn "Dashboard skipped (no DISPLAY)"
fi

# ============================================
# Step 8: Hourly Reporter (background)
# ============================================
if [ "$HOURS_INT" -ge 1 ]; then
    log "Starting hourly reporter..."
    (
        HOUR=0
        while [ $HOUR -lt "$HOURS_INT" ]; do
            sleep 3600  # 1 hour
            HOUR=$((HOUR + 1))
            generate_report "HOUR $HOUR/${HOURS_INT}" >> "$LOG_DIR/reports.log" 2>&1
        done
    ) &
    REPORTER_PID=$!
fi

# ============================================
# Initial Status
# ============================================
sleep 3
echo ""
echo "============================================="
echo "  All Systems Running"
echo "============================================="
echo ""
echo "Processes:"
echo "  Trader:   PID $TRADER_PID"
[ -n "$TUNER_PID" ] && echo "  Tuner:    PID $TUNER_PID"
[ -n "$WEB_API_PID" ] && echo "  Web API:  PID $WEB_API_PID"
[ -n "$PERF_PID" ] && echo "  Perf:     PID $PERF_PID (profiling enabled)"
echo ""
echo "Logs:"
echo "  $LOG_DIR/trader.log"
echo "  $LOG_DIR/tuner.log"
echo "  $LOG_DIR/web_api.log"
echo ""
echo "Monitoring:"
echo "  curl http://localhost:$WEB_API_PORT/api/status"
echo "  curl http://localhost:$WEB_API_PORT/api/errors"
echo "  tail -f $LOG_DIR/trader.log"
echo ""
echo "Dashboard (if X11 available):"
echo "  $BUILD_DIR/trader_dashboard"
echo ""
echo "Press Ctrl+C to stop"
echo "============================================="
echo ""

# Initial report
generate_report "INITIAL"

# Wait for duration (or until interrupted)
log "Session running for ${HOURS} hours..."
sleep "$DURATION_SECS"

# cleanup will be called by trap
