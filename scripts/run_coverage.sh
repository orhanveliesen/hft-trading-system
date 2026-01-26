#!/bin/bash
# run_coverage.sh - Generate test coverage report
# Usage: ./scripts/run_coverage.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build-coverage"

echo "========================================="
echo "  Trader Test Coverage Report Generator "
echo "========================================="

# Create coverage build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure with coverage flags
echo ""
echo "[1/5] Configuring with coverage flags..."
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-O0 -g --coverage -fprofile-arcs -ftest-coverage" \
      -DCMAKE_EXE_LINKER_FLAGS="--coverage" \
      "$PROJECT_DIR" > /dev/null

# Build
echo "[2/5] Building..."
make -j$(nproc) > /dev/null 2>&1

# Clear previous coverage data
echo "[3/5] Clearing previous coverage data..."
lcov --zerocounters --directory . > /dev/null 2>&1 || true
lcov --capture --initial --directory . --output-file base.info > /dev/null 2>&1

# Run tests
echo "[4/5] Running tests..."
ctest --output-on-failure > /dev/null 2>&1

# Capture coverage data
echo "[5/5] Generating coverage report..."
lcov --capture --directory . --output-file test.info > /dev/null 2>&1
lcov --add-tracefile base.info --add-tracefile test.info --output-file total.info > /dev/null 2>&1

# Filter out system headers and test files
lcov --remove total.info \
     '/usr/*' \
     '*/external/*' \
     '*/tests/*' \
     '*/benchmarks/*' \
     --output-file coverage.info > /dev/null 2>&1

# Generate summary
echo ""
echo "========================================="
echo "  COVERAGE SUMMARY                      "
echo "========================================="
lcov --list coverage.info 2>/dev/null | head -50

# Total coverage
echo ""
echo "========================================="
lcov --summary coverage.info 2>&1 | grep -E "lines|functions"
echo "========================================="

# Optionally generate HTML report
if command -v genhtml &> /dev/null; then
    genhtml coverage.info --output-directory coverage-report > /dev/null 2>&1
    echo ""
    echo "HTML report generated at: $BUILD_DIR/coverage-report/index.html"
fi

# List files with low coverage
echo ""
echo "Files with < 50% line coverage:"
lcov --list coverage.info 2>/dev/null | awk -F'|' '
    NR > 2 && $2 != "" {
        gsub(/[^0-9.]/, "", $2);
        if ($2 + 0 < 50) print $0
    }
' | head -20

echo ""
echo "Done!"
