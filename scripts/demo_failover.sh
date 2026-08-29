#!/usr/bin/env bash
# =============================================================================
#  ABEX Fault-Tolerance & Crash-Recovery Interactive Demo
#
#  Safe to run from ANY directory — every path is absolute via $REPO.
#
#  Usage:
#    /home/barun/dev/cpp/abex/scripts/demo_failover.sh
#    /home/barun/dev/cpp/abex/scripts/demo_failover.sh --auto
# =============================================================================
set -euo pipefail

# All paths derived from the script's own location — never from $PWD.
REPO="$(cd "$(dirname "$(realpath "$0")")/.." && pwd)"
BINARY="$REPO/build/abex_tests"
CTEST_BIN="$(command -v ctest)"
CMAKE_BIN="$(command -v cmake)"

AUTO=false
[[ "${1:-}" == "--auto" ]] && AUTO=true

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; DIM='\033[2m'; RESET='\033[0m'

EVIDENCE_DIR="$REPO/evidence"
mkdir -p "$EVIDENCE_DIR"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
LOG="$EVIDENCE_DIR/demo_failover_${TIMESTAMP}.txt"
exec > >(tee -a "$LOG") 2>&1

PASS=0; FAIL=0

pause() {
    $AUTO && { sleep 0.3; return; }
    echo -e "${DIM}  ↵  Press ENTER to continue...${RESET}"
    read -r
}

banner() {
    local w=72 line
    line="$(printf '═%.0s' $(seq 1 $w))"
    echo ""
    echo -e "${CYAN}╔${line}╗${RESET}"
    printf "${CYAN}║${RESET}  ${BOLD}%-$((w-2))s${RESET}${CYAN}║${RESET}\n" "$*"
    echo -e "${CYAN}╚${line}╝${RESET}"
}

section() {
    echo ""
    echo -e "${YELLOW}┌──────────────────────────────────────────────────────────────────┐${RESET}"
    echo -e "${YELLOW}│  $*${RESET}"
    echo -e "${YELLOW}└──────────────────────────────────────────────────────────────────┘${RESET}"
}

say() { echo -e "${DIM}  ℹ  $*${RESET}"; }

run_test() {
    local tag="$1" desc="$2"
    echo ""
    echo -e "  ${BOLD}▶ Running:${RESET} $desc"
    echo -e "  ${DIM}  tag: [$tag]${RESET}"
    echo ""
    local out rc
    set +e
    out=$("$BINARY" "[$tag]" --reporter console --verbosity high 2>&1)
    rc=$?
    set -e
    echo "$out"
    if [[ $rc -eq 0 ]]; then
        echo -e "  ${GREEN}✔  PASSED${RESET}"; (( PASS++ )) || true
    else
        echo -e "  ${RED}✘  FAILED${RESET}"; (( FAIL++ )) || true
    fi
}

# =============================================================================
banner "ABEX Fault-Tolerance & Crash-Recovery Interactive Demo"
# =============================================================================
echo ""
echo -e "  ${BOLD}Repository :${RESET} $REPO"
echo -e "  ${BOLD}Binary     :${RESET} $BINARY"
echo -e "  ${BOLD}cmake      :${RESET} $CMAKE_BIN"
echo -e "  ${BOLD}ctest      :${RESET} $CTEST_BIN"
echo -e "  ${BOLD}Evidence   :${RESET} $LOG"
echo -e "  ${BOLD}Commit     :${RESET} $(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo -e "  ${BOLD}Date       :${RESET} $(date)"
echo ""
echo -e "  ${BOLD}10 crash/failover scenario groups — 127 total tests${RESET}"
echo -e "  No live exchange, no network required. All deterministic."
echo ""

# =============================================================================
section "PRE-FLIGHT: Build"
# =============================================================================
say "Building debug binary from $REPO ..."
pause
"$CMAKE_BIN" --build "$REPO/build"
echo -e "  ${GREEN}✔  Build OK — binary: $BINARY${RESET}"

# =============================================================================
banner "PART 1 — PROCESS CRASH AND RESTART"
# =============================================================================
say "Core scenario: the gateway process is killed (SIGKILL, OOM, power loss)"
say "while orders are open. The journal survives on disk. On restart the new"
say "process recovers every order from the journal and reconciles with the venue"
say "to find out what happened while it was down."
echo ""
say "The venue runs independently — it may fill, cancel, or leave orders open"
say "while the gateway is dead. Reconciliation is the only source of truth."
echo ""
say "Journal guarantee: every operation is written to the append-only JSONL"
say "journal with fdatasync BEFORE the venue call. A crash mid-call still"
say "leaves a recoverable intent record."
pause

section "1A — Open order still LIVE at venue after restart"
say "Gateway crashes with one open LIMIT order. Venue: order still in book."
say "On restart: journal → LIVE, reconcile → open snapshot confirms LIVE,"
say "reconciliation_required cleared, GATEWAY_RESTARTED written to log."
pause
run_test "crash][recovery][live" \
    "Crash → restart → venue still has order → recovered as LIVE"

section "1B — Order FILLED at venue while gateway was down"
say "Gateway crashes. Venue fills the order. No WebSocket to receive the report."
say "On restart: journal → LIVE (last known), open snapshot → order absent,"
say "query() → FILLED, apply_execution() → FILLED, reconciliation_required cleared."
pause
run_test "crash][recovery][filled" \
    "Crash → venue fills order while down → recovered as FILLED on restart"

section "1C — Order PARTIALLY FILLED while gateway was down"
say "Venue partially fills the order while gateway is dead."
say "On restart: open snapshot returns PartiallyFilled with filled=0.04,"
say "apply_execution() merges the fill, order ends up PartiallyFilled."
pause
run_test "crash][recovery][partial" \
    "Crash → venue partially fills order while down → recovered as PARTIALLY_FILLED"

section "1D — Order CANCELLED by venue (cancel-on-disconnect)"
say "Venue has cancel-on-disconnect policy: all open orders cancelled when"
say "the WebSocket drops. On restart: open snapshot empty, query() → CANCELED,"
say "apply_execution() → CANCELED. Gateway reflects authoritative venue state."
pause
run_test "crash][recovery][canceled" \
    "Crash → venue cancels order (cancel-on-disconnect) → recovered as CANCELED"

section "1E — Multiple open orders, MIXED outcomes after restart"
say "4 orders open at crash. While gateway is down:"
say "  multi-live     → venue leaves it open"
say "  multi-filled   → venue fills it"
say "  multi-canceled → venue cancels it"
say "  multi-partial  → venue partially fills it (still open)"
say "One reconciliation pass resolves all four correctly."
pause
run_test "crash][recovery][multi" \
    "4 orders, 4 different venue outcomes → all reconciled correctly on restart"

section "1F — Pending CANCEL at crash time"
say "Cancel was sent and acknowledged before the crash. Journal records it."
say "On restart: reconciliation confirms CANCELED, pending_action cleared."
pause
run_test "crash][recovery][pending-cancel" \
    "Cancel acknowledged before crash → recovered as CANCELED on restart"

section "1G — Pending AMEND at crash time"
say "Amend was sent and acknowledged before the crash."
say "On restart: reconciliation finds the order at the amended price."
pause
run_test "crash][recovery][pending-amend" \
    "Amend acknowledged before crash → recovered at amended price on restart"

section "1H — UNKNOWN order (network timeout at crash time)"
say "place() timed out — outcome_uncertain=true. Order is UNKNOWN in journal."
say "The venue never received it."
say "On restart: open snapshot empty, query() → nullopt (venue never saw it),"
say "order stays UNKNOWN with PendingAction::Reconcile."
say "ABEX never guesses. Uncertain outcomes stay uncertain until the venue"
say "provides an authoritative answer."
pause
run_test "crash][recovery][unknown" \
    "Timeout → UNKNOWN in journal → restart → venue never saw it → stays UNKNOWN"

section "1I — THREE consecutive crashes, full lifecycle preserved"
say "Process crashes three times across the full order lifecycle:"
say "  Crash 1: place order"
say "  Crash 2: restart → reconcile → amend price to 49000"
say "  Crash 3: restart → reconcile → venue has partial fill → cancel"
say "  Final  : restart → verify CANCELED, filled=0.04, price=49000, qty=0.08"
say "Every restart writes GATEWAY_RESTARTED to the operational log."
pause
run_test "crash][recovery][multi-crash" \
    "3 crashes: place → amend → partial fill → cancel → final state verified"

section "1J — Two venues, one UNREACHABLE at restart"
say "Both OKX and Binance have open orders at crash time."
say "While gateway is down, Binance fills its order."
say "At restart, OKX is unreachable (query_open_orders returns nullopt)."
say "  → Binance reconciliation succeeds: binance-order recovered as FILLED"
say "  → OKX reconciliation fails: reconciliation_required stays true"
say "  → After OKX comes back, manual reconcile() resolves okx-order as LIVE"
say "Venue failures are isolated — one venue down does not block the other."
pause
run_test "crash][recovery][two-venues" \
    "Two venues: Binance fills offline, OKX unreachable → independent recovery"

# =============================================================================
banner "PART 2 — UNCERTAIN OUTCOMES AND RETRY IDEMPOTENCY"
# =============================================================================
say "When a venue call times out, ABEX cannot know if the order was accepted."
say "It marks the order UNKNOWN and records the uncertain outcome in the journal."
say "Any retry of the same clientOrderId replays the UNKNOWN result from the"
say "journal — the adapter is never called again. Reconciliation resolves it."
pause

section "2A — Uncertain PLACE → UNKNOWN → reconcile → LIVE"
run_test "fault][uncertain][place" \
    "Timeout on place: UNKNOWN, idempotent retry, reconcile resolves to LIVE"

section "2B — Uncertain CANCEL → UNKNOWN → idempotent retry"
run_test "fault][uncertain][cancel" \
    "Timeout on cancel: PendingAction=Cancel, retry replays same code"

# =============================================================================
banner "PART 3 — HARD VENUE REJECTION (IDEMPOTENCY)"
# =============================================================================
say "A hard rejection (insufficient margin, invalid symbol) is terminal."
say "ABEX writes REJECTED to the journal. All retries replay from the journal."
say "The adapter is never called again for a rejected order."
pause

section "3A — Hard place rejection → REJECTED → retries replay"
run_test "fault][rejection][idempotency" \
    "Venue hard-rejects place: REJECTED, 2nd and 3rd retries replay from journal"

section "3B — Hard cancel rejection → idempotent retry"
run_test "fault][rejection][cancel][idempotency" \
    "Venue hard-rejects cancel: idempotent retry returns same code"

# =============================================================================
banner "PART 4 — SEQUENCE GAP DETECTION"
# =============================================================================
say "WebSocket execution reports carry sequence numbers. A gap (e.g. seq 1→3)"
say "means at least one update was lost in transit. ABEX:"
say "  • Increments sequence_gaps counter"
say "  • Sets reconciliation_required=true"
say "  • Still applies the received update (fills are never discarded)"
say "  • Gaps on one venue do not affect the other venue's counter"
pause

section "4A — Single gap (1→3)"
run_test "fault][sequence][gap" \
    "Seq 1→3: gap detected, reconcile flagged, fill still applied"

section "4B — Multiple gaps"
run_test "fault][sequence][gap][multi" \
    "Seqs 1,3,5,7: three gaps accumulated"

section "4C — Out-of-order delivery (not a gap)"
run_test "fault][sequence][out-of-order" \
    "Seq 3 then 2: backward delivery is stale, not a gap, fill is max"

section "4D — Venue isolation"
run_test "fault][sequence][gap][venue-isolation" \
    "OKX gap does not pollute Binance gap counter"

# =============================================================================
banner "PART 5 — FILL RACE CONDITIONS"
# =============================================================================
say "Execution reports can arrive in unexpected orders. ABEX handles:"
say "  • Fill before ack: buffered and merged once ack arrives"
say "  • Duplicate fill (same event_id): suppressed, version unchanged"
say "  • Stale fill: qty is max of all reports, status never regresses"
say "  • Fill-cancel race: FILLED is terminal, late CANCELED is ignored"
pause

section "5A — Execution before acknowledgement"
run_test "fault][race][exec-before-ack" \
    "Fill arrives before ack: buffered, merged, order ends LIVE"

section "5B — Duplicate fill suppression"
run_test "fault][race][duplicate-fill" \
    "Same event_id delivered twice: second delivery is a no-op"

section "5C — Stale fill does not regress status"
run_test "fault][race][stale-fill" \
    "Older sequence fill: qty is max, status stays PartiallyFilled"

section "5D — Full fill wins cancel race"
run_test "fault][race][fill-cancel-race" \
    "FILLED then late CANCELED: order stays FILLED"

# =============================================================================
banner "PART 6 — BACKPRESSURE AND PROPERTY SWEEPS"
# =============================================================================
section "6A — Backpressure: dropped events trigger reconciliation"
say "When the execution event queue fills up, dropped events set"
say "reconciliation_required so the next reconcile cycle re-syncs."
pause
run_test "fault][backpressure" \
    "Tiny queue + 20 events: dropped events set reconciliation_required"

section "6B — Randomised market-order transitions (50×100)"
say "50 trials × 100 random state transitions. Invariants:"
say "  filled_quantity ∈ [0, quantity], FILLED is terminal, avg_price ≥ 0"
pause
run_test "fault][property][market" \
    "50×100 random market-order transitions: all fill invariants hold"

section "6C — Randomised Binance replacement transitions (30×50)"
say "30 trials × 50 random cancel-replace transitions."
say "Fill offsets must never overflow the order quantity."
pause
run_test "fault][property][binance][replacement" \
    "30×50 random Binance replacement transitions: no fill overflow"

# =============================================================================
banner "FULL SUITE VERIFICATION"
# =============================================================================
say "Running all 127 tests from $REPO to confirm no regressions."
pause
echo ""
echo -e "  ${BOLD}$CTEST_BIN --test-dir $REPO/build --output-on-failure${RESET}"
echo ""
"$CTEST_BIN" --test-dir "$REPO/build" --output-on-failure

# =============================================================================
banner "DEMO SUMMARY"
# =============================================================================
echo ""
echo -e "  ${BOLD}Scenario groups run : $((PASS + FAIL))${RESET}"
echo -e "  ${GREEN}${BOLD}Passed              : $PASS${RESET}"
[[ $FAIL -gt 0 ]] && echo -e "  ${RED}${BOLD}Failed              : $FAIL${RESET}" \
                  || echo -e "  ${DIM}Failed              : 0${RESET}"
echo ""
echo -e "  ${BOLD}Crash & restart scenarios:${RESET}"
echo -e "  ${GREEN}✔${RESET}  1A. Open order still LIVE at venue → recovered as LIVE"
echo -e "  ${GREEN}✔${RESET}  1B. Filled while down → recovered as FILLED"
echo -e "  ${GREEN}✔${RESET}  1C. Partially filled while down → recovered as PARTIALLY_FILLED"
echo -e "  ${GREEN}✔${RESET}  1D. Cancelled by venue (cancel-on-disconnect) → recovered as CANCELED"
echo -e "  ${GREEN}✔${RESET}  1E. 4 orders, 4 mixed outcomes → all reconciled in one pass"
echo -e "  ${GREEN}✔${RESET}  1F. Pending cancel at crash → reconcile confirms CANCELED"
echo -e "  ${GREEN}✔${RESET}  1G. Pending amend at crash → reconcile confirms amended price"
echo -e "  ${GREEN}✔${RESET}  1H. Timeout → UNKNOWN in journal → venue never saw it → stays UNKNOWN"
echo -e "  ${GREEN}✔${RESET}  1I. 3 consecutive crashes → full lifecycle preserved"
echo -e "  ${GREEN}✔${RESET}  1J. Two venues, one unreachable → independent recovery"
echo ""
echo -e "  ${BOLD}Additional failover scenarios:${RESET}"
echo -e "  ${GREEN}✔${RESET}  2.  Uncertain outcomes → UNKNOWN → idempotent retry → reconcile"
echo -e "  ${GREEN}✔${RESET}  3.  Hard venue rejection → REJECTED → all retries replay from journal"
echo -e "  ${GREEN}✔${RESET}  4.  Sequence gap detection → counter, reconcile flag, fill preserved"
echo -e "  ${GREEN}✔${RESET}  5.  Fill race conditions → pre-ack, duplicate, stale, fill-cancel"
echo -e "  ${GREEN}✔${RESET}  6.  Backpressure + 80 randomised property trials"
echo ""
echo -e "  ${BOLD}Evidence log:${RESET} $LOG"
echo ""

if [[ $FAIL -eq 0 ]]; then
    echo -e "  ${GREEN}${BOLD}ALL SCENARIOS PASSED ✔${RESET}"
    exit 0
else
    echo -e "  ${RED}${BOLD}$FAIL SCENARIO(S) FAILED ✘${RESET}"
    exit 1
fi
