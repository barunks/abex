#!/usr/bin/env bash
set -euo pipefail

cli=${ABEX_CLI:-./build/abex_cli}
config=${ABEX_CONFIG:-config/gateway.example.json}
state=${ABEX_STATE:-/tmp/abex-demo-orders.jsonl}

run() {
  "$cli" --mode simulation --config "$config" --state "$state" --command "$1"
}

run "place --id okx-demo-1 --venue OKX --symbol BTC-USDT --side BUY --type LIMIT --price 55000 --qty 0.1 --tif GTC"
run "amend --id okx-demo-1 --request-id amend-1 --new-price 54500 --new-quantity 0.08"
run "cancel --id okx-demo-1 --request-id cancel-1"

run "place --id binance-demo-1 --venue BINANCE --symbol ETH-USDT --side SELL --type LIMIT --price 10000 --qty 1 --tif GTC"
run "amend --id binance-demo-1 --request-id amend-2 --new-price 9900 --new-quantity 0.8"
run "cancel --id binance-demo-1 --request-id cancel-2"

run "list"
