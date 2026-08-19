#!/usr/bin/env bash
set -euo pipefail

gateway_url="${ABEX_URL:-http://127.0.0.1:8080}"
run_id="$(date +%s)"
okx_id="rest-okx-${run_id}"
binance_id="rest-binance-${run_id}"

request() {
  local method="$1"
  local path="$2"
  local body="${3:-}"
  if [[ -n "${body}" ]]; then
    curl --silent --show-error --fail-with-body \
      -X "${method}" "${gateway_url}${path}" \
      -H 'Content-Type: application/json' \
      --data "${body}"
  else
    curl --silent --show-error --fail-with-body -X "${method}" "${gateway_url}${path}"
  fi
  printf '\n'
}

request POST /api/v1/orders \
  "{\"clientOrderId\":\"${okx_id}\",\"venue\":\"OKX\",\"symbol\":\"BTC-USDT\",\"side\":\"BUY\",\"type\":\"LIMIT\",\"price\":\"50000\",\"quantity\":\"0.10\",\"timeInForce\":\"GTC\"}"

request POST /api/v1/orders \
  "{\"clientOrderId\":\"${binance_id}\",\"venue\":\"BINANCE\",\"symbol\":\"ETH-USDT\",\"side\":\"SELL\",\"type\":\"LIMIT\",\"price\":\"10000\",\"quantity\":\"1\",\"timeInForce\":\"GTC\"}"

request PATCH "/api/v1/orders/${okx_id}" \
  "{\"requestId\":\"amend-${run_id}\",\"newPrice\":\"49950\",\"newQuantity\":\"0.08\"}"

request PATCH "/api/v1/orders/${binance_id}" \
  "{\"requestId\":\"amend-binance-${run_id}\",\"newPrice\":\"9900\",\"newQuantity\":\"0.8\"}"

request DELETE "/api/v1/orders/${okx_id}" \
  "{\"requestId\":\"cancel-okx-${run_id}\"}"

request DELETE "/api/v1/orders/${binance_id}" \
  "{\"requestId\":\"cancel-binance-${run_id}\"}"

request GET /api/v1/orders
