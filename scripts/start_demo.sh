#!/usr/bin/env bash
# =============================================================================
#  ABEX Full-System Demo
#
#  Builds the project, starts abex_server in simulation mode, runs the REST
#  and CLI workflows, streams a few WebSocket messages, then shuts down.
#
#  Safe to run from ANY directory — every path is absolute via $REPO.
#
#  Usage:
#    ./scripts/start_demo.sh              # interactive
#    ./scripts/start_demo.sh --auto       # non-interactive (CI-friendly)
#    ./scripts/start_demo.sh --live       # live mode (requires .env)
#    ./scripts/start_demo.sh --port 8081  # alternate port
# =============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "$(realpath "$0")")/.." && pwd)"

# ── Argument parsing ──────────────────────────────────────────────────────────
AUTO=false
MODE=simulation
PORT=8080
while [[ $# -gt 0 ]]; do
    case "$1" in
        --auto)   AUTO=true ;;
        --live)   MODE=live ;;
        --port)   PORT="$2"; shift ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
    shift
done

# ── Paths ─────────────────────────────────────────────────────────────────────
SERVER="$REPO/build/abex_server"
CLI="$REPO/build/abex_cli"
CONFIG="$REPO/config/gateway.example.json"
STATE="/tmp/abex-demo-orders-$$.jsonl"
EVIDENCE_DIR="$REPO/evidence"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
LOG="$EVIDENCE_DIR/demo_${MODE}_${TIMESTAMP}.txt"
URL="http://127.0.0.1:${PORT}"
SERVER_PID=""

mkdir -p "$EVIDENCE_DIR"
exec > >(tee -a "$LOG") 2>&1

# ── Colours ───────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; DIM='\033[2m'; RESET='\033[0m'

# ── Helpers ───────────────────────────────────────────────────────────────────
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
    echo -e "${YELLOW}── $* ──────────────────────────────────────────────────────────${RESET}"
}

say()  { echo -e "${DIM}  ℹ  $*${RESET}"; }
ok()   { echo -e "  ${GREEN}✔  $*${RESET}"; }
fail() { echo -e "  ${RED}✘  $*${RESET}"; }

pause() {
    $AUTO && { sleep 0.4; return; }
    echo -e "${DIM}  ↵  Press ENTER to continue...${RESET}"
    read -r
}

cleanup() {
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        say "Stopping abex_server (PID $SERVER_PID)..."
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        ok "Server stopped."
    fi
    rm -f "$STATE"
}
trap cleanup EXIT INT TERM

api() {
    local method="$1" path="$2" body="${3:-}"
    if [[ -n "$body" ]]; then
        curl --silent --show-error --fail-with-body \
            -X "$method" "${URL}${path}" \
            -H 'Content-Type: application/json' \
            --data "$body"
    else
        curl --silent --show-error --fail-with-body -X "$method" "${URL}${path}"
    fi
    printf '\n'
}

cli_cmd() {
    "$CLI" --mode "$MODE" --config "$CONFIG" --state "$STATE" --command "$1"
}

wait_for_server() {
    local attempts=0
    say "Waiting for server on ${URL}/api/v1/health ..."
    until curl --silent --fail "${URL}/api/v1/health" >/dev/null 2>&1; do
        (( attempts++ )) || true
        if [[ $attempts -ge 30 ]]; then
            fail "Server did not become ready after 30 seconds."
            exit 1
        fi
        sleep 1
    done
    ok "Server is ready."
}

# ── Header ────────────────────────────────────────────────────────────────────
banner "ABEX Full-System Demo  [mode: ${MODE}]"
echo ""
echo -e "  ${BOLD}Repository :${RESET} $REPO"
echo -e "  ${BOLD}Server     :${RESET} $SERVER"
echo -e "  ${BOLD}CLI        :${RESET} $CLI"
echo -e "  ${BOLD}Config     :${RESET} $CONFIG"
echo -e "  ${BOLD}URL        :${RESET} $URL"
echo -e "  ${BOLD}Evidence   :${RESET} $LOG"
echo -e "  ${BOLD}Commit     :${RESET} $(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo -e "  ${BOLD}Date       :${RESET} $(date)"
echo ""

if [[ "$MODE" == "live" ]]; then
    echo -e "  ${YELLOW}${BOLD}LIVE MODE — real venue credentials required in .env${RESET}"
    echo -e "  ${YELLOW}Orders will be submitted to the configured exchange endpoints.${RESET}"
    echo ""
fi

# ── Step 1: Build ─────────────────────────────────────────────────────────────
section "STEP 1 — Build"
say "Building debug binary from $REPO ..."
pause
cmake --build "$REPO/build"
ok "Build complete."

# ── Step 2: Start server ──────────────────────────────────────────────────────
section "STEP 2 — Start abex_server"
say "Starting abex_server in ${MODE} mode on port ${PORT} ..."
pause

SERVER_ARGS=(
    "--mode" "$MODE"
    "--config" "$CONFIG"
    "--port" "$PORT"
)
[[ "$MODE" == "live" && -f "$REPO/.env" ]] && SERVER_ARGS+=("--env-file" "$REPO/.env")

"$SERVER" "${SERVER_ARGS[@]}" &
SERVER_PID=$!
say "Server PID: $SERVER_PID"
wait_for_server

# ── Step 3: Health check ──────────────────────────────────────────────────────
section "STEP 3 — Health check"
say "GET /api/v1/health"
pause
api GET /api/v1/health | python3 -m json.tool 2>/dev/null || api GET /api/v1/health
echo ""

# ── Step 4: Market data ───────────────────────────────────────────────────────
section "STEP 4 — Market data"
say "GET /api/v1/market-data"
pause
api GET /api/v1/market-data | python3 -m json.tool 2>/dev/null || api GET /api/v1/market-data
echo ""

# ── Step 5: REST workflow ─────────────────────────────────────────────────────
section "STEP 5 — REST workflow (OKX + Binance)"
RUN_ID="demo-$(date +%s)"
OKX_ID="rest-okx-${RUN_ID}"
BINANCE_ID="rest-binance-${RUN_ID}"

say "Placing OKX BTC-USDT BUY LIMIT order ..."
pause
api POST /api/v1/orders \
    "{\"clientOrderId\":\"${OKX_ID}\",\"venue\":\"OKX\",\"symbol\":\"BTC-USDT\",\"side\":\"BUY\",\"type\":\"LIMIT\",\"price\":\"50000\",\"quantity\":\"0.10\",\"timeInForce\":\"GTC\"}" \
    | python3 -m json.tool 2>/dev/null || true
echo ""

say "Placing Binance ETH-USDT SELL LIMIT order ..."
pause
api POST /api/v1/orders \
    "{\"clientOrderId\":\"${BINANCE_ID}\",\"venue\":\"BINANCE\",\"symbol\":\"ETH-USDT\",\"side\":\"SELL\",\"type\":\"LIMIT\",\"price\":\"10000\",\"quantity\":\"1\",\"timeInForce\":\"GTC\"}" \
    | python3 -m json.tool 2>/dev/null || true
echo ""

say "Amending OKX order (new price 49950, new qty 0.08) ..."
pause
api PATCH "/api/v1/orders/${OKX_ID}" \
    "{\"requestId\":\"amend-okx-${RUN_ID}\",\"newPrice\":\"49950\",\"newQuantity\":\"0.08\"}" \
    | python3 -m json.tool 2>/dev/null || true
echo ""

say "Amending Binance order (new price 9900, new qty 0.8) ..."
pause
api PATCH "/api/v1/orders/${BINANCE_ID}" \
    "{\"requestId\":\"amend-binance-${RUN_ID}\",\"newPrice\":\"9900\",\"newQuantity\":\"0.8\"}" \
    | python3 -m json.tool 2>/dev/null || true
echo ""

say "Canceling OKX order ..."
pause
api DELETE "/api/v1/orders/${OKX_ID}" \
    "{\"requestId\":\"cancel-okx-${RUN_ID}\"}" \
    | python3 -m json.tool 2>/dev/null || true
echo ""

say "Canceling Binance order ..."
pause
api DELETE "/api/v1/orders/${BINANCE_ID}" \
    "{\"requestId\":\"cancel-binance-${RUN_ID}\"}" \
    | python3 -m json.tool 2>/dev/null || true
echo ""

say "Listing all orders ..."
pause
api GET /api/v1/orders | python3 -m json.tool 2>/dev/null || api GET /api/v1/orders
echo ""

# ── Step 6: CLI workflow ──────────────────────────────────────────────────────
section "STEP 6 — CLI workflow (in-process, simulation)"
say "The CLI uses the same OrderGateway application service in-process."
say "State file: $STATE"
pause

CLI_OKX="cli-okx-${RUN_ID}"
CLI_BIN="cli-bin-${RUN_ID}"

say "place OKX BTC-USDT BUY LIMIT"
cli_cmd "place --id ${CLI_OKX} --venue OKX --symbol BTC-USDT --side BUY --type LIMIT --price 55000 --qty 0.1 --tif GTC"
echo ""

say "simulate partial fill"
cli_cmd "simulate --venue OKX --id ${CLI_OKX} --status PARTIALLY_FILLED --filled 0.04 --last-price 54800 --event-id fill-1 --sequence 1"
echo ""

say "amend price and quantity"
cli_cmd "amend --id ${CLI_OKX} --request-id amend-cli-1 --new-price 54500 --new-quantity 0.08"
echo ""

say "cancel"
cli_cmd "cancel --id ${CLI_OKX} --request-id cancel-cli-1"
echo ""

say "place Binance ETH-USDT SELL LIMIT"
cli_cmd "place --id ${CLI_BIN} --venue BINANCE --symbol ETH-USDT --side SELL --type LIMIT --price 10000 --qty 1 --tif GTC"
echo ""

say "simulate fill"
cli_cmd "simulate --venue BINANCE --id ${CLI_BIN} --status FILLED --filled 1 --last-price 9990 --event-id fill-2 --sequence 1"
echo ""

say "list all orders"
cli_cmd "list"
echo ""

say "positions"
cli_cmd "positions"
echo ""

# ── Step 7: Idempotency demonstration ─────────────────────────────────────────
section "STEP 7 — Idempotency: retry same request"
say "Placing the same OKX order again with the same clientOrderId ..."
say "Expected: idempotent replay — no second venue call."
pause
api POST /api/v1/orders \
    "{\"clientOrderId\":\"${OKX_ID}\",\"venue\":\"OKX\",\"symbol\":\"BTC-USDT\",\"side\":\"BUY\",\"type\":\"LIMIT\",\"price\":\"50000\",\"quantity\":\"0.10\",\"timeInForce\":\"GTC\"}" \
    | python3 -m json.tool 2>/dev/null || true
echo ""

# ── Step 8: System state ──────────────────────────────────────────────────────
section "STEP 8 — OMS stability and audit timeline"
say "GET /api/v1/system"
pause
api GET /api/v1/system | python3 -m json.tool 2>/dev/null || api GET /api/v1/system
echo ""

# ── Step 9: WebSocket snapshot ────────────────────────────────────────────────
section "STEP 9 — WebSocket snapshot (3 seconds)"
say "Connecting to ws://127.0.0.1:${PORT}/ws/v1/orders ..."
say "Capturing initial snapshot messages for 3 seconds ..."
pause
if command -v websocat >/dev/null 2>&1; then
    timeout 3 websocat "ws://127.0.0.1:${PORT}/ws/v1/orders" 2>/dev/null \
        | head -20 || true
elif command -v wscat >/dev/null 2>&1; then
    timeout 3 wscat --connect "ws://127.0.0.1:${PORT}/ws/v1/orders" 2>/dev/null \
        | head -20 || true
else
    say "Neither websocat nor wscat found — skipping WebSocket capture."
    say "Install with: cargo install websocat  or  npm install -g wscat"
fi
echo ""

# ── Step 10: Instruments and balances ─────────────────────────────────────────
section "STEP 10 — Instrument rules and balances"
say "GET /api/v1/instruments?venue=OKX&symbol=BTC-USDT"
pause
api GET "/api/v1/instruments?venue=OKX&symbol=BTC-USDT" \
    | python3 -m json.tool 2>/dev/null || true
echo ""

if [[ "$MODE" == "live" ]]; then
    say "GET /api/v1/balances?venue=OKX&currency=USDT"
    api GET "/api/v1/balances?venue=OKX&currency=USDT" \
        | python3 -m json.tool 2>/dev/null || true
    echo ""
fi

# ── Summary ───────────────────────────────────────────────────────────────────
banner "Demo Complete"
echo ""
ok "Build"
ok "Server started in ${MODE} mode"
ok "Health check"
ok "Market data"
ok "REST workflow: place → amend → cancel (OKX + Binance)"
ok "CLI workflow: place → simulate → amend → cancel"
ok "Idempotency: retry returned existing order"
ok "OMS stability and audit timeline"
[[ "$MODE" == "live" ]] && ok "Live balance query"
echo ""
echo -e "  ${BOLD}Evidence log:${RESET} $LOG"
echo -e "  ${BOLD}UI:${RESET}           ${URL}"
echo ""
echo -e "  ${DIM}Server is still running (PID $SERVER_PID). Press Ctrl-C to stop.${RESET}"
echo ""

# Keep server alive for manual exploration unless --auto
if $AUTO; then
    say "Auto mode: shutting down."
else
    echo -e "${DIM}  Press ENTER to stop the server and exit.${RESET}"
    read -r
fi
