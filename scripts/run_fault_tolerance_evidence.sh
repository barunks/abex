#!/usr/bin/env bash
# Fault-tolerance evidence runner.
# Builds, runs the full test suite with verbose output, and captures a
# structured evidence report to evidence/fault_tolerance_TIMESTAMP.txt
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
EVIDENCE_DIR="$REPO/evidence"
REPORT="$EVIDENCE_DIR/fault_tolerance_${TIMESTAMP}.txt"
BINARY="$REPO/build/abex_tests"

mkdir -p "$EVIDENCE_DIR"

log()     { echo "[$(date +%H:%M:%S)] $*" | tee -a "$REPORT"; }
section() {
  echo "" | tee -a "$REPORT"
  echo "================================================================" | tee -a "$REPORT"
  echo "  $*" | tee -a "$REPORT"
  echo "================================================================" | tee -a "$REPORT"
}

{
  echo "ABEX Fault-Tolerance Evidence Report"
  echo "Generated : $(date)"
  echo "Host      : $(uname -n)"
  echo "Commit    : $(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "Branch    : $(git -C "$REPO" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
} | tee -a "$REPORT"

section "1. BUILD"
log "Building debug preset..."
cmake --build "$REPO/build" 2>&1 | tee -a "$REPORT"
log "Build complete."

section "2. FULL TEST SUITE"
log "Running all 127 tests..."
ctest --test-dir "$REPO/build" --output-on-failure 2>&1 | tee -a "$REPORT"

section "3. FAULT-TOLERANCE TESTS (verbose)"
"$BINARY" "[fault]" --reporter console --verbosity high 2>&1 | tee -a "$REPORT"

section "4. SEQUENCE GAP DETECTION"
"$BINARY" "[fault][sequence]" --reporter console --verbosity high 2>&1 | tee -a "$REPORT"

section "5. UNCERTAIN OUTCOMES AND RETRY IDEMPOTENCY"
"$BINARY" "[fault][uncertain],[fault][rejection]" --reporter console --verbosity high 2>&1 | tee -a "$REPORT"

section "6. DISCONNECT / RECONNECT / FAILOVER"
"$BINARY" "[fault][disconnect]" --reporter console --verbosity high 2>&1 | tee -a "$REPORT"

section "7. JOURNAL RECOVERY"
"$BINARY" "[fault][journal],[recovery]" --reporter console --verbosity high 2>&1 | tee -a "$REPORT"

section "8. RACE CONDITIONS"
"$BINARY" "[fault][race],[gateway][race]" --reporter console --verbosity high 2>&1 | tee -a "$REPORT"

section "9. PROPERTY-BASED SWEEPS"
"$BINARY" "[fault][property],[property]" --reporter console --verbosity high 2>&1 | tee -a "$REPORT"

section "10. SUMMARY"
log "Evidence report: $REPORT"
grep -E "^[0-9]+/[0-9]+ tests passed|All tests passed|tests failed" "$REPORT" | tail -5 | tee -a "$REPORT"
echo ""
echo "Report saved to: $REPORT"
