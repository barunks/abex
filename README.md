# ABEX Exchange Gateway

ABEX is a C++20 order gateway with one exchange-neutral order model for OKX and Binance.
It provides a persistent CLI, an asynchronous REST/WebSocket server, and a responsive browser
operations console. All presentation paths use the same application service and normalized JSON
views; trading rules stay in the domain and application layers.

Runtime mode is selected with `--mode live|simulation` and defaults to `live`. Live mode includes
signed OKX REST order entry/cancel/amend with private WebSocket updates and signed Binance Spot
WebSocket API trading with user-data execution reports. Simulation mode uses the same application
path and fills orders from the current mapped public market quote.

## What is implemented

- Market and limit orders, cancellation, and amendment/replacement on both venues
- Fixed-point, eight-decimal monetary arithmetic
- Normalized `LIVE`, `PARTIALLY_FILLED`, `FILLED`, `CANCELED`, and `REJECTED` states
- Durable client/exchange ID mapping and create/cancel/amend idempotency
- Duplicate suppression, out-of-order merge rules, and acknowledgement/update race handling
- Configured risk limits plus authoritative venue status/minimum/step/tick/notional and available-balance checks
- Checksummed append-only OMS/audit journal, restart recovery, open-order enumeration, and periodic/manual reconciliation
- Reconnect health, sequence-gap detection, rate awareness, and bounded backpressure
- Strict common REST schema and an OpenAPI 3.1 document
- Bounded order/market/system WebSocket stream with snapshot/resynchronization semantics
- Standalone one-second OKX/Binance quote publisher backed by a memory-mapped ring-buffer file
- Responsive UI with prices, routeable quantity range, blocking preflight guidance, and persisted restart/retry/alert history
- 66 deterministic tests, including venue-rule/funding rejection, replacement generations, mmap, recovery, loopback HTTP/WebSocket, and 20,000 randomized transitions
- Environment-only live credentials; secret values are never serialized or logged

## Build and test

Prerequisites are CMake 3.24+, a C++20 compiler, Ninja, Boost 1.81+, OpenSSL 3, libcurl 8,
nlohmann/json, and Catch2 3.

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Without presets:

```bash
cmake -S . -B build -G Ninja -DABEX_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

To omit the browser server target:

```bash
cmake -S . -B build -G Ninja \
  -DABEX_BUILD_SERVER=OFF
cmake --build build -j
```

Live and simulation adapters are always in the same binary; mode selection is runtime-only. The
C++ source contains no conditional-compilation branches.

## Run the server and UI

Start the complete system from the repository root with the primary OMS/gateway executable. By
default `abex_server` serves the UI and supervises `abex_market_data` as a separate process. It
waits for the publisher's first quote before reporting the complete setup ready. Runtime mode
defaults to live:

```bash
./build/abex_server --config config/gateway.example.json
```

For a safe deterministic fill demonstration, select simulation explicitly:

```bash
./build/abex_server --mode simulation --config config/gateway.example.json
```

Open <http://127.0.0.1:8080>. The UI submits the same schema to either venue and consumes normalized
updates from `ws://127.0.0.1:8080/ws/v1/orders`. Ctrl-C stops the OMS server and its supervised
market-data child cleanly.

For independently managed processes, disable server supervision and start the publisher separately:

```bash
./build/abex_server --mode live --no-market-data --config config/gateway.example.json
./build/abex_market_data --config config/gateway.example.json
```

`abex_server` remains the primary OMS process and owns exchange connectivity, the order journal,
REST/WebSocket APIs, and the browser UI. The child owns only public quote polling and the single
writer side of `state/market-data.ring`; the server reads that same file and streams the quotes into
the UI. Select another UI port with `--port 8081` if needed.

The browser explains each process boundary explicitly: order commands use the REST API, continuous
order/market/system updates use the client WebSocket, authoritative reconnect snapshots use REST,
and public market data enters the gateway through the memory-mapped ring file.

The exchange transport is deliberately not a client option. It is part of each adapter contract:
OKX commands use authenticated REST and OKX updates use its private WebSocket; Binance commands and
execution updates both use its authenticated WebSocket API. CLI commands call the same
`OrderGateway` application service in-process, while browser/UI commands use the public REST API.

| Method | Path | Purpose |
|---|---|---|
| `POST` | `/api/v1/orders` | Place a market or limit order |
| `GET` | `/api/v1/orders` | List orders; optional `venue` and `status` filters |
| `GET` | `/api/v1/orders/{clientOrderId}` | Get one normalized order |
| `PATCH` | `/api/v1/orders/{clientOrderId}` | Amend price and/or total quantity |
| `DELETE` | `/api/v1/orders/{clientOrderId}` | Cancel an order |
| `POST` | `/api/v1/reconcile/{venue}` | Reconcile non-terminal venue orders |
| `GET` | `/api/v1/health` | Venue connection/reconciliation health |
| `GET` | `/api/v1/positions` | Conservative per-symbol exposure |
| `GET` | `/api/v1/balances?venue=OKX&currency=USDT` | Current selected order-currency balance (`BTC`, `ETH`, or `USDT`; currency is required) |
| `GET` | `/api/v1/instruments?venue=OKX&symbol=ETH-USDT` | Current venue status, minimums, steps, ticks, and notional limits |
| `GET` | `/api/v1/market-data` | Mapped quotes, ring status, and best executable prices |
| `GET` | `/api/v1/system` | Transport, journal, restart/retry counters, and durable audit events |
| `GET` | `/api/v1/openapi.json` | OpenAPI 3.1 description |

Example placement:

```bash
curl --fail-with-body http://127.0.0.1:8080/api/v1/orders \
  -H 'Content-Type: application/json' \
  -d '{"clientOrderId":"order-123","venue":"OKX","symbol":"BTC-USDT","side":"BUY","type":"LIMIT","price":"30000","quantity":"0.1","timeInForce":"IOC"}'
```

Run [the REST workflow](examples/rest_demo.sh) in another terminal to place, amend, and cancel
orders on both simulated venues.

## OMS capture, restart, and retry evidence

Every create, amend, cancel, acknowledgement, execution update, retry, connection change, and
reconciliation is represented in the OMS journal. Order intent and its operation identifier are
written before venue I/O. Records are append-only JSONL with monotonic sequence numbers and
checksums; with the default `durableWrites=true`, `fdatasync` completes before the operation
advances. A retained OS file lock permits only one writer for a journal.

At startup, the latest complete snapshot for every order is recovered. The gateway enumerates venue
open orders (including OKX `/trade/orders-pending`) and correlates only orders owned by the durable
journal. Account-wide orders created by another application, journal, or venue UI are ignored and
never adopted, canceled, or treated as gateway health failures. Journaled non-terminal orders
missing from the snapshot are queried individually. The same reconciliation runs periodically and
after reconnect. Identical retries replay the durable
result without making a second venue request; uncertain outcomes remain `UNKNOWN` until
reconciliation rather than being guessed as rejected or canceled.

The UI's **OMS stability** panel shows recovered orders, current-run retries, reconciliations,
journal sequence/durability, and logging failures. Its durable event timeline includes process
starts/restarts, disconnects/reconnects, request intent/acknowledgement, unknown outcomes,
idempotency conflicts/replays, sequence gaps, backpressure, and reconciliation results. The same
evidence is available at `/api/v1/system` and streamed as `system.event` messages.

## Run the CLI

```bash
./build/abex_cli --mode simulation --config config/gateway.example.json
```

Typical session:

```text
place --id order-123 --venue OKX --symbol BTC-USDT --side BUY --type LIMIT --price 30000 --qty 0.1 --tif IOC
simulate --venue OKX --id order-123 --status PARTIALLY_FILLED --filled 0.04 --last-price 29995 --event-id fill-1 --sequence 1
amend --id order-123 --request-id amend-1 --new-price 29950 --new-quantity 0.08
cancel --id order-123 --request-id cancel-1
get --id order-123
list --venue OKX
positions
balances --venue OKX --symbol ETH-USDT --side BUY --type MARKET --price 3000
rules --venue OKX --symbol ETH-USDT
health
```

Every command returns a common JSON envelope. A single non-interactive command is useful for scripts:

```bash
./build/abex_cli --mode simulation --state /tmp/abex-orders.jsonl --command \
  'place --id b-1 --venue BINANCE --symbol ETH-USDT --side SELL --type LIMIT --price 3500 --qty 1'
```

The executable [CLI demo](examples/demo.sh) covers both simulated venues.

For a read-only live funding check and conservative maximum-quantity suggestion:

```bash
./build/abex_cli --mode live --command \
  'balances --venue OKX --symbol ETH-USDT --side BUY --type MARKET --price 3000'
```

## Runtime mode and credentials

Copy `.env.example` to `.env`, replace the five placeholders, and restrict the file to the account
running ABEX. Live mode is the default and automatically loads `.env` from the current working
directory; inherited process variables take precedence. Use `--env-file FILE` when the credential
file is elsewhere:

```bash
chmod 600 .env
./build/abex_cli --mode live --config config/gateway.example.json
# or
./build/abex_server --mode live --config config/gateway.example.json
```

Use `--mode simulation` whenever external order submission is not intended. Treat acknowledgement
timeouts as unknown outcomes and reconcile before retrying with a new operation identifier. The
credentials printed in the supplied requirements document should be considered exposed and rotated
before use; ABEX never reads that document. The public market-data publisher does not load or use
exchange credentials.

Protocol mappings follow the official [OKX API v5 documentation](https://www.okx.com/docs-v5/en/)
and [Binance Spot WebSocket API documentation](https://developers.binance.com/en/docs/catalog/core-trading-spot-trading/api/ws-api/trade).

### Acknowledgements, market orders, and timestamps

An accepted placement response means the venue accepted the command; it is not proof of a fill.
Consequently a market order can briefly be `LIVE`. Only an order-stream execution report or an
authoritative query moves it to `FILLED`/`CANCELED`; OKX market placements receive an immediate
follow-up query as well as the normal WebSocket and periodic recovery paths.

ABEX does not use `RDTSC` for exchange timestamps. Binance time is anchored to the venue's `time`
response, advanced with `std::chrono::steady_clock`, corrected by the measured request midpoint,
and biased backward by the larger of the configured safety margin and measured half-RTT
uncertainty. Epoch/audit times use `system_clock`. Binance response `rateLimits` continuously
resynchronize the local request-weight budget.

## Project layout

```text
apps/abex_cli/               CLI executable
apps/abex_server/            REST/WebSocket server executable
apps/abex_market_data/       separate one-second public quote publisher
include/abex/domain/         value types and order state machine
include/abex/application/    gateway orchestration, risk, sequencing, backpressure
include/abex/ports/          exchange and persistence interfaces
include/abex/infrastructure/ venue protocols, transports, mmap ring, journal, simulation
include/abex/cli/            CLI presentation adapter
include/abex/server/         REST API and HTTP/WebSocket boundary
include/abex/bootstrap/      shared CLI/server runtime composition
src/                         implementations mirroring include/abex
tests/                       unit, integration, recovery, and randomized tests
web/                         dependency-free browser UI
config/                      non-secret example configuration
docs/                        architecture and requirements audit
examples/                    runnable CLI and REST workflows
```

See [Architecture and decisions](docs/ARCHITECTURE.md) for lifecycle rules and trade-offs, and
[Requirements audit](docs/REQUIREMENTS_AUDIT.md) for the line-by-line compliance review.

## Security boundary and limitations

Client authentication, TLS termination, and production secret/key management are intentionally
outside this exercise. The server binds to `127.0.0.1` by default; do not expose it to an untrusted
network. Production deployment should add authenticated authorization at a reverse proxy or service
boundary and retrieve short-lived venue credentials from a secret manager.

Live exchange tests are not run automatically because they mutate external demo accounts and are
nondeterministic. Protocol serialization, normalization, signing primitives, the common API, and
the server transports are covered locally. See the audit for the remaining accepted limitations.
