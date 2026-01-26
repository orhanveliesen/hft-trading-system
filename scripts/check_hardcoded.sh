#!/bin/bash
# check_hardcoded.sh - Detect hardcoded values and magic numbers in Trader codebase
# Usage: ./scripts/check_hardcoded.sh [--strict]
# Returns: 0 if clean, 1 if issues found (in strict mode)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
STRICT_MODE=false

if [[ "$1" == "--strict" ]]; then
    STRICT_MODE=true
fi

RED='\033[0;31m'
YELLOW='\033[1;33m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

WARNINGS=0
ERRORS=0

# Source directories to check
INCLUDE_DIR="$PROJECT_DIR/include"
TOOLS_DIR="$PROJECT_DIR/tools"

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}  Trader Hardcoded Value Detection     ${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""

# ==============================================================================
# KNOWN EXCEPTIONS (legitimate hardcoded values)
# ==============================================================================
# Price scaling: 10000 is the standard price multiplier (Price = double * 10000)
# These patterns are allowed:
# - * 10000, / 10000 (price conversions)
# - constexpr, const definitions
# - _MS, _ms, timeout (time constants)
# - Comments

# ==============================================================================
# 1. HARDCODED API KEYS / SECRETS (CRITICAL)
# ==============================================================================
echo -e "${YELLOW}[1/4] Checking for hardcoded secrets...${NC}"

# Search for potential secrets
# Exclude: lines searching for env var patterns (e.g., find("CLAUDE_API_KEY="))
secrets_found=$(grep -rn --include="*.cpp" --include="*.hpp" \
    -E '(api_key|API_KEY|secret|SECRET|password|PASSWORD|token)\s*=\s*"[^"]{10,}"' \
    "$INCLUDE_DIR" "$TOOLS_DIR" 2>/dev/null | \
    grep -v '//' | grep -v 'test_' | \
    grep -v 'find\|getenv\|env\[' || true)

if [[ -n "$secrets_found" ]]; then
    while IFS= read -r line; do
        echo -e "  ${RED}ERROR${NC}: $line"
        ((ERRORS++)) || true
    done <<< "$secrets_found"
fi

echo ""

# ==============================================================================
# 2. HARDCODED URLS (should be configurable)
# ==============================================================================
echo -e "${YELLOW}[2/4] Checking for hardcoded URLs...${NC}"

# Find URLs that aren't in constants or comments
urls_found=$(grep -rn --include="*.cpp" --include="*.hpp" \
    -E 'https?://|wss?://' \
    "$INCLUDE_DIR" "$TOOLS_DIR" 2>/dev/null | \
    grep -v '^\s*//' | \
    grep -v 'constexpr' | \
    grep -v 'const.*=' | \
    grep -v 'test_' | \
    grep -v '// ' | \
    grep -v 'URL\|url\|endpoint\|Endpoint' || true)

if [[ -n "$urls_found" ]]; then
    while IFS= read -r line; do
        # Skip if it's clearly in a string constant definition
        if echo "$line" | grep -qE 'const\s+(char|std::string)'; then
            continue
        fi
        echo -e "  ${YELLOW}WARNING${NC}: $(echo "$line" | cut -d: -f1,2)"
        echo -e "    $(echo "$line" | cut -d: -f3-)"
        ((WARNINGS++)) || true
    done <<< "$urls_found"
fi

echo ""

# ==============================================================================
# 3. MAGIC PERCENTAGES (common config values that should be parameters)
# ==============================================================================
echo -e "${YELLOW}[3/4] Checking for magic percentage values...${NC}"

# Find decimal values like 0.01, 0.05, 0.1, etc. that are assigned
# Exclude: constexpr, const, _pct, percent, ratio, config accessor patterns
magic_pct=$(grep -rn --include="*.cpp" --include="*.hpp" \
    -E '=[[:space:]]*0\.(0[1-9]|[1-9])[0-9]*[^0-9]' \
    "$INCLUDE_DIR" "$TOOLS_DIR" 2>/dev/null | \
    grep -v 'constexpr' | \
    grep -v 'const\s' | \
    grep -v '_pct\|_PCT\|percent\|Percent\|ratio\|Ratio\|config\.' | \
    grep -v '^\s*//' | \
    grep -v 'test_' | \
    grep -v 'tolerance\|epsilon\|EPSILON' || true)

if [[ -n "$magic_pct" ]]; then
    count=0
    while IFS= read -r line; do
        # Skip if in member initialization with default
        if echo "$line" | grep -qE 'double\s+\w+\s*=\s*0\.[0-9]+;.*//'; then
            # It has a comment explaining it - still warn but lower priority
            :
        fi
        echo -e "  ${YELLOW}WARNING${NC}: $(echo "$line" | cut -d: -f1,2)"
        echo -e "    $(echo "$line" | cut -d: -f3-)"
        ((WARNINGS++)) || true
        ((count++)) || true
        if [[ $count -ge 15 ]]; then
            echo -e "  ${YELLOW}... and more (truncated)${NC}"
            break
        fi
    done <<< "$magic_pct"
fi

echo ""

# ==============================================================================
# 4. LARGE MAGIC NUMBERS (potential config values)
# ==============================================================================
echo -e "${YELLOW}[4/4] Checking for large magic numbers...${NC}"

# Find large numbers (1000+) that might be config values
# Exclude: 10000 (price scaling), sizes/capacities, constexpr/const
magic_nums=$(grep -rn --include="*.cpp" --include="*.hpp" \
    -E '[^0-9_x][1-9][0-9]{3,}[^0-9_]' \
    "$INCLUDE_DIR" "$TOOLS_DIR" 2>/dev/null | \
    grep -v 'constexpr\|const\s' | \
    grep -v '10000\|100000' | \
    grep -v 'MAX_\|MIN_\|SIZE\|CAPACITY\|BUFFER\|_MS\|_ms\|timeout' | \
    grep -v '^\s*//' | \
    grep -v 'test_' | \
    grep -v '0x[0-9a-fA-F]' | \
    grep -v 'magic\|MAGIC\|version\|VERSION' || true)

if [[ -n "$magic_nums" ]]; then
    count=0
    while IFS= read -r line; do
        echo -e "  ${YELLOW}WARNING${NC}: $(echo "$line" | cut -d: -f1,2)"
        echo -e "    $(echo "$line" | cut -d: -f3-)"
        ((WARNINGS++)) || true
        ((count++)) || true
        if [[ $count -ge 15 ]]; then
            echo -e "  ${YELLOW}... and more (truncated)${NC}"
            break
        fi
    done <<< "$magic_nums"
fi

echo ""

# ==============================================================================
# SUMMARY
# ==============================================================================
echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}  SUMMARY                              ${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""

if [[ $ERRORS -gt 0 ]]; then
    echo -e "  ${RED}ERRORS:   $ERRORS${NC} (must fix)"
fi
if [[ $WARNINGS -gt 0 ]]; then
    echo -e "  ${YELLOW}WARNINGS: $WARNINGS${NC} (review recommended)"
fi
if [[ $ERRORS -eq 0 && $WARNINGS -eq 0 ]]; then
    echo -e "  ${GREEN}No issues found!${NC}"
fi

echo ""
echo -e "${BLUE}Exceptions:${NC}"
echo "  - Price scaling (10000) is allowed"
echo "  - constexpr/const definitions are allowed"
echo "  - Values in comments are ignored"
echo "  - Known config patterns (_pct, ratio, etc.) are allowed"
echo ""

# Exit with error in strict mode if errors found
if [[ "$STRICT_MODE" == "true" && $ERRORS -gt 0 ]]; then
    echo -e "${RED}Build failed due to hardcoded value errors.${NC}"
    exit 1
fi

exit 0
