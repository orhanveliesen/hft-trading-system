#!/bin/bash
# Coverage Filter Script
# Reads patterns from coverage_excludes.list and applies chained lcov --remove
# Usage: ./coverage_filter.sh <input.info> <output.info> <excludes_list>

set -e

INPUT_FILE="$1"
OUTPUT_FILE="$2"
EXCLUDES_LIST="$3"
LCOV_OPTS="--ignore-errors unused"

if [ ! -f "$INPUT_FILE" ]; then
    echo "Error: Input file $INPUT_FILE not found"
    exit 1
fi

if [ ! -f "$EXCLUDES_LIST" ]; then
    echo "Error: Excludes list $EXCLUDES_LIST not found"
    exit 1
fi

# Read patterns from excludes list (skip comments and empty lines)
PATTERNS=()
while IFS= read -r line || [ -n "$line" ]; do
    # Trim whitespace
    line=$(echo "$line" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
    # Skip empty lines and comments
    if [ -n "$line" ] && [[ ! "$line" =~ ^# ]]; then
        PATTERNS+=("$line")
    fi
done < "$EXCLUDES_LIST"

if [ ${#PATTERNS[@]} -eq 0 ]; then
    echo "Warning: No patterns found in $EXCLUDES_LIST"
    cp "$INPUT_FILE" "$OUTPUT_FILE"
    exit 0
fi

# Chain lcov --remove calls (lcov 2.0 requires separate calls per pattern)
CURRENT_FILE="$INPUT_FILE"
STEP=0

for pattern in "${PATTERNS[@]}"; do
    STEP=$((STEP + 1))
    NEXT_FILE="coverage_step_${STEP}.info"

    lcov --remove "$CURRENT_FILE" "$pattern" -o "$NEXT_FILE" $LCOV_OPTS 2>/dev/null || true

    # Clean up previous intermediate file (except the original input)
    if [ "$CURRENT_FILE" != "$INPUT_FILE" ]; then
        rm -f "$CURRENT_FILE"
    fi

    CURRENT_FILE="$NEXT_FILE"
done

# Move final result to output file
mv "$CURRENT_FILE" "$OUTPUT_FILE"

echo "Filtered coverage: ${#PATTERNS[@]} patterns applied"
