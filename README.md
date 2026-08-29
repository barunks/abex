# ABEX Exchange Gateway

ABEX is a production-grade C++20 order-management gateway with a single exchange-neutral order
model for OKX and Binance Spot. It provides a durable OMS journal, a persistent CLI, an
asynchronous REST/WebSocket server, and a responsive browser operations console. All presentation
paths share the same application service and normalized JSON views; trading rules stay in the
domain and application layers.

Runtime mode is selected with `--mode live|simulation` and defaults to `live`. Live mode includes
signed OKX REST order entry/cancel/amend with private WebSocket updates and signed Binance Spot
WebSocket API trading with user-data execution reports. Simulation mode uses the same application
path and fills orders from the current mapped public market quote.

## Start here

For a safe local evaluation with no credentials and no real orders:

```bash
cmake --preset debug
cmake --build --preset debug -j2
ctest --preset debug --output-on-failure
./build/abex_server --mode simulation --config config/gateway.example.json
```

Wait for `ABEX complete setup ready`, then open <http://127.0.0.1:8080>. Stop the complete setup
with `Ctrl-C`; the server handles `SIGINT`/`SIGTERM`, stops the HTTP listener, drains gateway
workers, and terminates its supervised `abex_market_data` child. The automated equivalent is:

```bash
./scripts/start_demo.sh --auto
```

### System pipeline at a glance

```text
Browser / REST client / CLI
            │ common order schema
            ▼
       OrderGateway
       ├─ validation and risk preflight
       ├─ append complete intent record ──► JSONL journal ──► background fdatasync
       ├─ OKX adapter ──► REST commands + private WebSocket updates
       └─ Binance adapter ──► signed WebSocket commands + execution reports
                              │
                              ▼
                    per-venue execution lane
                              │
                              ▼
                  deterministic state machine
                              │
              REST snapshots / client WebSocket / UI

OKX + Binance public market data
            │ REST polling or public WebSockets
            ▼
     abex_market_data process ──► mmap ring ──► abex_server market book
```

The journal record is appended before venue routing. When `durableWrites=true`, a dedicated
worker coalesces `fdatasync` calls; this keeps filesystem-sync latency off the command path while
leaving a bounded power-loss window between append and completed sync. See
[Architecture](docs/ARCHITECTURE.md) for the full invariants and trade-offs.

---

## Table of contents

1. [Start here](#start-here)
2. [Requirements compliance](#requirements-compliance)
3. [What is implemented](#what-is-implemented)
4. [Project layout](#project-layout)
5. [Prerequisites](#prerequisites)
6. [Build system and CMake targets](#build-system-and-cmake-targets)
7. [Build](#build)
8. [Test](#test)
9. [Sanitizer builds](#sanitizer-builds)
10. [Run the server and UI](#run-the-server-and-ui)
11. [Process lifecycle and shutdown](#process-lifecycle-and-shutdown)
12. [Run the CLI](#run-the-cli)
13. [Credentials and runtime mode](#credentials-and-runtime-mode)
14. [REST API reference](#rest-api-reference)
15. [WebSocket stream reference](#websocket-stream-reference)
16. [Configuration reference](#configuration-reference)
17. [Demo scripts](#demo-scripts)
18. [OMS capture, restart, and retry evidence](#oms-capture-restart-and-retry-evidence)
19. [Performance](#performance)
20. [Security boundary and limitations](#security-boundary-and-limitations)

---

## Requirements compliance

The table below maps every requirement group from `project_requirements.txt` to its implementation


| Requirement | Status | Implementation |
|---|---|---|
| Exchange-neutral REST API | Complete | `GatewayApi`, strict common-field validation, OpenAPI 3.1 at `/api/v1/openapi.json` |
| Common OKX/Binance order schema | Complete | `OrderRequest` — `clientOrderId`, `venue`, `symbol`, `side`, `type`, `price`, `quantity`, `timeInForce`; exchange-specific fields rejected |
| OKX adapter — REST place/cancel/amend, WS updates | Complete | `OkxAdapter`: REST `/trade/order`, `/trade/cancel-order`, `/trade/amend-order`; private WS `orders` channel |
| Binance adapter — WS order submission and updates | Complete | `BinanceAdapter`: signed WS `order.place`, `order.cancel`, `order.cancelReplace`; user-data `executionReport` |
| Adapters isolated behind common interface | Complete | `IExchangeAdapter` port; `OrderGateway` has zero venue JSON/signing/socket logic |
| Market and limit orders | Complete | Both types on both venues; IOC/GTC/FOK time-in-force |
| Cancel order | Complete | Common cancel with durable `requestId` idempotency |
| Amend/replace order | Complete | OKX native amend; Binance cancel-replace with generation tracking |
| Normalized states: Live, Partially Filled, Filled, Canceled, Rejected | Complete | `OrderStateMachine`; `UNKNOWN` + `pendingAction` for operational uncertainty |
| `clientOrderId` → `exchangeOrderId` mapping | Complete | Durable alias index; Binance cancel-replace generation aliases persisted |
| Out-of-order WebSocket messages | Complete | Sequence-guarded merge; stale reports contribute fills but cannot regress lifecycle |
| Duplicate execution reports | Complete | Per-order processed `eventId` set; duplicate is a no-op |
| REST vs WebSocket race conditions | Complete | Intent journaled before I/O; execution arriving before ACK correlates by `clientOrderId` |
| Client retry idempotency | Complete | Create keyed by `clientOrderId` + fingerprint; amend/cancel keyed by `requestId` |
| Persist state for restart recovery | Complete | Checksummed append-only JSONL journal; background `fdatasync`; exclusive file lock |
| Max order size per instrument | Complete | Configured `risk.*.maxOrderSize`; checked before routing |
| Max notional per order | Complete | Configured `risk.*.maxNotional`; MARKET uses fresh mapped quote |
| Per-instrument position limit | Complete | Conservative full-fill view of open orders plus realized fills |
| Clear rejection reasons | Complete | Machine-readable codes + human-readable reasons; persisted as `REJECTED` orders |
| WebSocket disconnect and reconnect | Complete | TLS WS transport: hostname verification, bounded exponential backoff, subscription replay |
| Duplicate/missing messages | Complete | Duplicate suppression; sequence-gap detection triggers reconciliation |
| Gateway restart with live orders | Complete | Startup: load journal → restore aliases → enumerate venue open orders → reconcile |
| Reload persisted orders on startup | Complete | Latest complete snapshot per `clientOrderId` recovered; torn final append ignored |
| Reconcile with OKX open orders | Complete | `GET /trade/orders-pending`; ownership boundary enforced (journal-owned orders only) |
| Re-establish Binance WebSocket state | Complete | Signed user-data stream re-subscribed after each reconnect |
| Sequence number tracking and gap detection | Complete | Per-adapter sequence trackers; gaps degrade health and trigger reconciliation |
| Rate-limit awareness per exchange | Complete | Lock-free GCRA `TokenBucket`; Binance `rateLimits` response fields resynchronize capacity |
| Backpressure between WS ingestion and REST clients | Complete | Bounded SPSC execution ring per venue; bounded 256-message client WS queue; `resync.required` |
| WebSocket client endpoint | Complete | `/ws/v1/orders` — order/market/system snapshots + normalized updates |
| Property-based tests for order state transitions | Complete | 20,000 randomized transitions covering all lifecycle rules |
| Authentication / key management (out of scope) | Intentionally omitted | Environment-only venue secrets; loopback bind; production guidance in ARCHITECTURE.md §21 |
| Ultra-low-latency optimization (out of scope) | Intentionally omitted | Correctness-first journal; clear runtime ports; performance engineering documented |

---

## What is implemented

### Order management

- Market and limit orders, cancellation, and amendment/replacement on both venues
- Fixed-point eight-decimal `Decimal` arithmetic — no binary floating-point drift in any monetary field
- Normalized `LIVE`, `PARTIALLY_FILLED`, `FILLED`, `CANCELED`, `REJECTED` states; `UNKNOWN` + `pendingAction` for uncertain outcomes
- Durable `clientOrderId` → `exchangeOrderId` mapping; Binance cancel-replace generation aliases persisted across restarts
- Duplicate suppression, out-of-order merge rules, and ACK/update race handling
- Create/cancel/amend idempotency keyed by `clientOrderId`/`requestId` fingerprints; exact retries replay without a second venue call

### Risk and preflight

- Configured per-instrument maximum order size and notional
- Authoritative venue instrument rules: trading status, min/max quantity, step, price range/tick, notional bounds
- Conservative position limit: full-fill view of open orders plus realized fills on terminal orders
- Authoritative available-balance preflight before every placement; fails closed on query failure
- All local rejections persisted as `REJECTED` orders with machine-readable codes and human-readable reasons

### Persistence and recovery

- Checksummed append-only JSONL journal; complete append before venue I/O and coalesced background `fdatasync`; exclusive file lock prevents split-brain writers
- Order intent and request identity written before venue I/O
- Startup recovery: load latest snapshot per order → restore exchange aliases → enumerate venue open orders → reconcile owned orders → query individually any journaled non-terminal order absent from the open snapshot
- Ownership boundary: account-wide orders not in the journal are never adopted, canceled, or treated as health failures
- Periodic reconciliation (default 30 s) and post-reconnect reconciliation
- Durable operational event timeline: process starts/restarts, intents, ACKs, retries, disconnects, gaps, backpressure, reconciliation results

### Resilience and rate limiting

- TLS WebSocket transport: hostname verification, bounded exponential backoff, write serialization, subscription/auth replay after reconnect
- Lock-free GCRA `TokenBucket` per venue adapter; one 64-bit CAS per admission; `static_assert` lock-free guarantee
- Binance `rateLimits` response fields continuously resynchronize local token-bucket capacity, consumption, and refill rate
- Bounded SPSC execution ring per venue (lock-free submit path); bounded 256-message client WebSocket queue
- Sequence-gap detection; gaps degrade venue health and demand reconciliation

### APIs and interfaces

- Strict common REST schema; unknown fields rejected; OpenAPI 3.1 document at `/api/v1/openapi.json`
- WebSocket stream `/ws/v1/orders`: `orders.snapshot`, `market.snapshot`, `system.snapshot` on connect; then `order.updated`, `market.updated`, `system.event`; `resync.required` for slow consumers
- CLI in-process adapter over the same `OrderGateway` application service; identical normalized JSON output
- Responsive browser UI: live prices, routeable quantity range, blocking preflight guidance, persisted restart/retry/alert history

### Market data

- Standalone `abex_market_data` process: streaming OKX/Binance public top-of-book WebSockets when both URLs are configured, with concurrent REST polling as the fallback
- Fixed-layout POSIX memory-mapped ring-buffer file; seqlock slots; advisory write lock; generation tracking for publisher restarts
- `abex_server` tails the ring without making market-data network requests; exposes quotes at `/api/v1/market-data` and over the client WebSocket
- Five-second configurable maximum quote age; stale quotes block MARKET order submission

### Test suite — 127 deterministic tests

| Tag | Count | What it covers |
|---|---|---|
| `[decimal]` | 8 | Fixed-point arithmetic, overflow, formatting |
| `[state-machine]` | 12 | All order lifecycle transitions and race rules |
| `[risk]` | 10 | Size, notional, position, balance, instrument-rule checks |
| `[gateway]` | 20 | Idempotency, replacement generations, race handling, observer concurrency |
| `[recovery]` | 8 | Journal load, alias restoration, torn-append handling |
| `[crash][recovery]` | 10 | SIGKILL scenarios, multi-crash, two-venue recovery |
| `[fault]` | 28 | Uncertain outcomes, sequence gaps, backpressure, fill races |
| `[property]` | 3 | 20,000 randomized state-machine transitions |
| `[mmap]` | 6 | Ring-buffer publication, consumption, generation tracking |
| `[server]` | 2 | Loopback REST API and WebSocket stream |
| `[protocols]` | 6 | OKX/Binance protocol serialization and normalization |
| `[environment]` | 3 | Credential loading and validation |
| `[rate-limiter]` | 9 | Token-bucket admission, gateway rejection, idempotent retry, synchronize, ADAPTER_EXCEPTION |

- Zero data races — verified with Clang 18 ThreadSanitizer, 127/127 pass
- Zero memory leaks — verified with Clang 18 AddressSanitizer + LeakSanitizer, 127/127 pass
- Environment-only live credentials; secret values are never serialized or logged

---

## Project layout

```text
apps/abex_cli/               CLI executable
apps/abex_server/            REST/WebSocket server executable
apps/abex_market_data/       separate streaming/polling public quote publisher
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
docs/                        architecture and performance documentation
examples/                    runnable CLI and REST workflows
scripts/                     build, demo, and evidence-capture scripts
evidence/                    captured test run outputs
```

See [Architecture and threading model](docs/ARCHITECTURE.md) for lifecycle rules, concurrency,
persistence, recovery, and journal-compaction design; see
[Benchmarking](docs/BENCHMARKING.md) for measurement methodology and current results.

---

## Prerequisites

| Dependency | Minimum version | Notes |
|---|---|---|
| CMake | 3.24 | Presets require 3.24+ |
| C++ compiler | GCC 13 or Clang 18 | Full C++20 required |
| Ninja | any | Used by all presets |
| Boost | 1.81 | Asio, Beast, system |
| OpenSSL | 3.x | TLS for live venue connections |
| libcurl | 7.x | REST transport; Ubuntu 24.04 provides 8.x |
| nlohmann/json | 3.x | JSON serialization |
| yaml-cpp | 0.7 | YAML configuration support |
| Catch2 | 3.x | Test framework |

Ubuntu 24.04 apt packages cover most dependencies but **not** CMake 3.24+, nlohmann/json 3.11,
Boost 1.81, or Catch2 v3. The verified install sequence for a fresh Ubuntu 24.04 machine:

```bash
# 1. Compiler, Ninja, OpenSSL, libcurl, yaml-cpp (all correct versions from apt)
sudo apt-get install -y \
  g++-13 clang-18 ninja-build \
  libssl-dev libcurl4-openssl-dev libyaml-cpp-dev

# 2. CMake 3.24+ via Kitware apt repo
wget -qO- https://apt.kitware.com/keys/kitware-archive-latest.asc \
  | sudo gpg --dearmor -o /usr/share/keyrings/kitware-archive-keyring.gpg
echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] \
  https://apt.kitware.com/ubuntu/ $(lsb_release -cs) main" \
  | sudo tee /etc/apt/sources.list.d/kitware.list
sudo apt-get update && sudo apt-get install -y cmake

# 3. nlohmann/json 3.11 (apt provides 3.10 on 24.04 — build from source)
git clone --depth 1 --branch v3.11.3 https://github.com/nlohmann/json.git /tmp/json
cmake -S /tmp/json -B /tmp/json/build -DJSON_BuildTests=OFF
sudo cmake --build /tmp/json/build --target install

# 4. Boost 1.81 (apt provides 1.74 — build from source)
wget -q https://archives.boost.io/release/1.81.0/source/boost_1_81_0.tar.gz -O /tmp/boost.tar.gz
tar -xf /tmp/boost.tar.gz -C /tmp
cd /tmp/boost_1_81_0 && ./bootstrap.sh --with-libraries=system
sudo ./b2 install -j$(nproc)

# 5. Catch2 v3 (apt provides v2 — build from source)
git clone --depth 1 --branch v3.6.0 https://github.com/catchorg/Catch2.git /tmp/catch2
cmake -S /tmp/catch2 -B /tmp/catch2/build -DCATCH_INSTALL_DOCS=OFF
sudo cmake --build /tmp/catch2/build --target install -j$(nproc)
```

---

## Build system and CMake targets

`CMakeLists.txt` defines one reusable core library, one optional server library, three runtime
executables, the Catch2 test binary, and an opt-in benchmark binary. Configuration is expressed
through target-scoped include paths, compile features, warnings, and link dependencies.

```text
abex_core
├─ abex_cli
├─ abex_market_data
├─ abex_server_lib ──► abex_server
├─ abex_tests
└─ abex_benchmark  (ABEX_BUILD_BENCHMARKS=ON)
```

| CMake option | Default | Effect |
|---|---:|---|
| `ABEX_BUILD_TESTS` | `ON` | Builds `abex_tests`, enables CTest, and discovers Catch2 cases |
| `ABEX_BUILD_SERVER` | `ON` | Builds Beast HTTP/WebSocket hosting, `abex_server`, and HTTP integration tests |
| `ABEX_BUILD_BENCHMARKS` | `OFF` | Builds the standalone `abex_benchmark` harness |
| `ABEX_ENABLE_SANITIZERS` | `OFF` | Adds GCC/Clang AddressSanitizer and UndefinedBehaviorSanitizer flags |

| Target | Responsibility | Principal dependencies |
|---|---|---|
| `abex_core` | Domain, gateway, adapters, journal, market-data ring, CLI/API logic | Threads, OpenSSL, libcurl, Boost.System, nlohmann/json, yaml-cpp |
| `abex_server_lib` | Asynchronous HTTP, WebSocket, static UI hosting | `abex_core`, Boost.System, Threads |
| `abex_server` | OMS composition root and supervised market-data lifecycle | `abex_server_lib`, `abex_market_data` build dependency |
| `abex_cli` | Interactive and one-shot administrative interface | `abex_core` |
| `abex_market_data` | Public quote publisher and mmap-ring writer | `abex_core` |
| `abex_tests` | Unit, integration, recovery, fault, and transport tests | `abex_core`, Catch2; optionally `abex_server_lib` |
| `abex_benchmark` | Release hot-path and end-to-end microbenchmarks | `abex_core` |

`CMakePresets.json` supplies separate build directories for `debug`, `release`, `asan`,
`clang-asan`, and `clang-tsan`. This prevents sanitizer and optimization flags from contaminating
one another. `compile_commands.json` is exported for IDEs and static-analysis tools.

The current `CMakeLists.txt` selects the Debian/Ubuntu x86-64 shared libcurl path explicitly to
avoid accidentally linking an incompatible `/usr/local` static libcurl on the development host.
For another architecture or distribution, remove or override `CURL_LIBRARY` and
`CURL_INCLUDE_DIR` so `find_package(CURL)` resolves the platform installation.

Install rules place executables under `bin`, the UI under `share/abex/web`, and the example JSON
configuration under `share/abex/config`. The run commands below use the build tree, which is the
recommended evaluation workflow.

---

## Build

### Quick start with presets

```bash
cmake --preset debug
cmake --build --preset debug

# Or in one step
./scripts/build.sh
```

### Without presets

```bash
cmake -S . -B build -G Ninja -DABEX_BUILD_TESTS=ON
cmake --build build -j$(nproc)
```

### Release build

```bash
cmake --preset release
cmake --build --preset release
```

### Release build with benchmarks

```bash
cmake --preset release -DABEX_BUILD_BENCHMARKS=ON
cmake --build --preset release --target abex_benchmark
./build-release/abex_benchmark
```

See [docs/BENCHMARKING.md](docs/BENCHMARKING.md) for benchmark reproduction and interpretation.

### Omit the browser server target

```bash
cmake -S . -B build -G Ninja -DABEX_BUILD_SERVER=OFF
cmake --build build -j$(nproc)
```

Live and simulation adapters are always compiled into the same binary. Mode selection is
runtime-only. The C++ source contains no conditional-compilation branches.

---

## Test

### Run all 127 tests

```bash
ctest --preset debug
# or
ctest --test-dir build --output-on-failure
```

### Run a specific test group

```bash
# Fault-tolerance and crash-recovery scenarios
build/abex_tests "[fault]" --reporter console --verbosity high

# Recovery tests only
build/abex_tests "[recovery]" --reporter console --verbosity high

# HTTP server and WebSocket tests
build/abex_tests "[server]" --reporter console --verbosity high

# Randomized property sweeps
build/abex_tests "[property]" --reporter console --verbosity high

# Rate-limiter and observer concurrency
build/abex_tests "[rate-limiter],[observer]" --reporter console
```

### Capture a structured evidence report

```bash
./scripts/run_fault_tolerance_evidence.sh
# Report saved to evidence/fault_tolerance_TIMESTAMP.txt
```

---

## Sanitizer builds

Four sanitizer presets are provided. ASAN and TSAN are mutually exclusive builds.

### AddressSanitizer + LeakSanitizer + UndefinedBehaviorSanitizer (GCC)

```bash
cmake --preset asan
cmake --build --preset asan
ctest --preset asan
# Expected: 127/127 passed, zero issues
```

### AddressSanitizer + LeakSanitizer + UndefinedBehaviorSanitizer (Clang 18)

```bash
cmake --preset clang-asan
cmake --build --preset clang-asan
ctest --preset clang-asan
# Expected: 127/127 passed, zero issues
```

### ThreadSanitizer (Clang 18)

```bash
cmake --preset clang-tsan
cmake --build --preset clang-tsan
ctest --preset clang-tsan -j1   # serial for clean race reports
# Expected: 127/127 passed, zero data races
```

LSAN suppressions for third-party libraries are applied automatically by the `clang-asan` test preset via `LSAN_OPTIONS` in `CMakePresets.json`.

---

## Run the server and UI

Start the complete system from the repository root. By default `abex_server` serves the UI and
supervises `abex_market_data` as a separate process. It waits for the publisher's first quote
before reporting the complete setup ready.

### Simulation mode (safe, no credentials required)

```bash
./build/abex_server --mode simulation --config config/gateway.example.json
```

Open <http://127.0.0.1:8080>. The UI submits the same schema to either venue and consumes
normalized updates from `ws://127.0.0.1:8080/ws/v1/orders`. Ctrl-C stops the OMS server and its
supervised market-data child cleanly.

### Live mode (requires credentials in `.env`)

```bash
cp .env.example .env
# Edit .env with real OKX and Binance API keys
chmod 600 .env
./build/abex_server --config config/gateway.example.json
```

### Independently managed processes

```bash
./build/abex_server --mode live --no-market-data --config config/gateway.example.json
./build/abex_market_data --config config/gateway.example.json
```

### Select a different port

```bash
./build/abex_server --mode simulation --config config/gateway.example.json --port 8081
```

`abex_server` is the primary OMS process and owns exchange connectivity, the order journal,
REST/WebSocket APIs, and the browser UI. The child owns only public quote ingestion and the single
writer side of `state/market-data.ring`; the server reads that same file and streams the quotes
into the UI.

The exchange transport is not a client option. It is part of each adapter contract: OKX commands
use authenticated REST and OKX updates use its private WebSocket; Binance commands and execution
updates both use its authenticated WebSocket API.

---

## Process lifecycle and shutdown

### Supervised mode

The normal command starts one parent and one child:

```bash
./build/abex_server --mode simulation --config config/gateway.example.json
```

1. The server loads configuration and the optional environment file.
2. `GatewayRuntime` opens and exclusively locks the journal, restores orders, starts adapters,
   starts the mmap-ring reader, and reconciles owned non-terminal orders.
3. The HTTP/WebSocket listener starts.
4. The server forks `abex_market_data`, passes it a readiness pipe, and waits up to 30 seconds for
   the first successfully published quote.
5. Only after readiness does the server print the UI, REST, and WebSocket endpoints.

Press `Ctrl-C`, or send `SIGTERM` to the server PID, for a graceful stop:

```bash
kill -TERM <abex_server_pid>
```

Shutdown stops reconciliation, closes adapter transports, drains execution and observer queues,
records `GATEWAY_STOPPED`, drains operational events, stops the HTTP listener, and reaps the
market-data child. If the child does not exit within five seconds, the supervisor terminates it.
Do not use `kill -9` for normal operation; reserve it for crash-recovery testing.

### Independently managed processes

For containers, systemd, or separate process supervision, start the publisher and server with the
same configuration and ring path:

```bash
# Terminal/service 1
./build/abex_market_data --config config/gateway.example.json

# Terminal/service 2
./build/abex_server --mode simulation --no-market-data \
  --config config/gateway.example.json
```

Stop the server first so it no longer consumes quotes, then stop the publisher:

```bash
kill -TERM <abex_server_pid>
kill -TERM <abex_market_data_pid>
```

In external-publisher mode, `abex_server` does not manage or restart the publisher. Health becomes
stale when the ring stops advancing, and stale quotes fail closed for MARKET orders.

### Restart and state isolation

- Reuse the same `--state`/`journal.path` to test recovery and idempotent retry.
- Use a new explicit `--state /tmp/abex-run.jsonl` for an isolated demo.
- Only one process can own a journal or market-data ring writer at a time.
- Never run the CLI and server concurrently against the same journal; both are composition roots
  and the second owner will be rejected by the file lock.
- Removing state is destructive. Archive it only while all ABEX processes are stopped.

---

## Run the CLI

```bash
./build/abex_cli --mode simulation --config config/gateway.example.json
```

### Typical interactive session

```text
abex> place --id order-123 --venue OKX --symbol BTC-USDT --side BUY --type LIMIT --price 30000 --qty 0.1 --tif IOC
abex> simulate --venue OKX --id order-123 --status PARTIALLY_FILLED --filled 0.04 --last-price 29995 --event-id fill-1 --sequence 1
abex> amend --id order-123 --request-id amend-1 --new-price 29950 --new-quantity 0.08
abex> cancel --id order-123 --request-id cancel-1
abex> get --id order-123
abex> list --venue OKX
abex> positions
abex> balances --venue OKX --symbol ETH-USDT --side BUY --type MARKET --price 3000
abex> rules --venue OKX --symbol ETH-USDT
abex> health
```

Every command returns a common JSON envelope.

### Single non-interactive command

```bash
./build/abex_cli --mode simulation --state /tmp/abex-orders.jsonl --command \
  'place --id b-1 --venue BINANCE --symbol ETH-USDT --side SELL --type LIMIT --price 3500 --qty 1'
```

### Read-only live funding check

```bash
./build/abex_cli --mode live --command \
  'balances --venue OKX --symbol ETH-USDT --side BUY --type MARKET --price 3000'
```

---

## Credentials and runtime mode

Copy `.env.example` to `.env`, replace the five placeholders, and restrict the file to the account
running ABEX. Live mode is the default and automatically loads `.env` from the current working
directory; inherited process variables take precedence. Use `--env-file FILE` when the credential
file is elsewhere.

```bash
cp .env.example .env
# Fill in real values:
#   ABEX_OKX_API_KEY
#   ABEX_OKX_SECRET_KEY
#   ABEX_OKX_PASSPHRASE
#   ABEX_BINANCE_API_KEY
#   ABEX_BINANCE_SECRET_KEY
chmod 600 .env
```

Use `--mode simulation` whenever external order submission is not intended. Treat acknowledgement
timeouts as unknown outcomes and reconcile before retrying with a new operation identifier.

The public market-data publisher does not load or use exchange credentials.

Protocol mappings follow the official
[OKX API v5 documentation](https://www.okx.com/docs-v5/en/) and
[Binance Spot WebSocket API documentation](https://developers.binance.com/en/docs/catalog/core-trading-spot-trading/api/ws-api/trade).

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

---

## REST API reference

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
| `GET` | `/api/v1/balances?venue=OKX&currency=USDT` | Current selected order-currency balance |
| `GET` | `/api/v1/instruments?venue=OKX&symbol=ETH-USDT` | Venue status, minimums, steps, ticks, notional |
| `GET` | `/api/v1/market-data` | Mapped quotes, ring status, and best executable prices |
| `GET` | `/api/v1/system` | Transport, journal, restart/retry counters, and durable audit events |
| `GET` | `/api/v1/openapi.json` | OpenAPI 3.1 description |

`currency` for `/api/v1/balances` must be one of `BTC`, `ETH`, or `USDT`.

### Place order

```bash
curl --fail-with-body http://127.0.0.1:8080/api/v1/orders \
  -H 'Content-Type: application/json' \
  -d '{
    "clientOrderId": "order-123",
    "venue": "OKX",
    "symbol": "BTC-USDT",
    "side": "BUY",
    "type": "LIMIT",
    "price": "30000",
    "quantity": "0.1",
    "timeInForce": "IOC"
  }'
```

### Amend order

```bash
curl --fail-with-body -X PATCH http://127.0.0.1:8080/api/v1/orders/order-123 \
  -H 'Content-Type: application/json' \
  -d '{"requestId": "amend-1", "newPrice": "29950", "newQuantity": "0.08"}'
```

### Cancel order

```bash
curl --fail-with-body -X DELETE http://127.0.0.1:8080/api/v1/orders/order-123 \
  -H 'Content-Type: application/json' \
  -d '{"requestId": "cancel-1"}'
```

### List orders with filter

```bash
curl 'http://127.0.0.1:8080/api/v1/orders?venue=OKX&status=LIVE'
```

### Check health

```bash
curl http://127.0.0.1:8080/api/v1/health
```

---

## WebSocket stream reference

Connect to `ws://127.0.0.1:8080/ws/v1/orders`.

On connection the server sends three snapshot messages in order:

| `type` | Content |
|---|---|
| `orders.snapshot` | Full list of all known orders |
| `market.snapshot` | Current bid/ask for all mapped symbols |
| `system.snapshot` | Transport state, journal status, operational event timeline |

Subsequent messages:

| `type` | Trigger |
|---|---|
| `order.updated` | Any order state change |
| `market.updated` | New quote from the ring-buffer publisher |
| `system.event` | Operational event (connect, disconnect, reconcile, gap, etc.) |
| `resync.required` | Client fell behind; reload snapshots from REST |
| `pong` | Response to a `{"type":"ping"}` message |

The outbound queue is bounded at 256 messages per connection. A slow consumer receives
`resync.required` and should reload authoritative state from the REST endpoints.

---

## Configuration reference

`config/gateway.example.json` documents every field:

```json
{
  "journal": {
    "path": "state/orders.jsonl",
    "durableWrites": true
  },
  "eventQueueCapacity": 4096,
  "reconciliationIntervalMs": 30000,
  "marketData": {
    "ringPath": "state/market-data.ring",
    "ringCapacity": 1024,
    "publishIntervalMs": 1000,
    "maximumAgeMs": 5000,
    "ringPollIntervalMs": 50,
    "okxRestUrl": "https://www.okx.com",
    "binanceRestUrl": "https://data-api.binance.vision",
    "okxPublicWebSocketUrl": "wss://ws.okx.com:8443/ws/v5/public",
    "binancePublicWebSocketUrl": "wss://stream.binance.com:9443/stream?streams=btcusdt@bookTicker/ethusdt@bookTicker"
  },
  "server": {
    "address": "127.0.0.1",
    "port": 8080,
    "ioThreads": 2,
    "webRoot": "web"
  },
  "risk": {
    "BTC-USDT": {
      "maxOrderSize": "2",
      "maxNotional": "250000",
      "positionLimit": "5"
    },
    "ETH-USDT": {
      "maxOrderSize": "25",
      "maxNotional": "150000",
      "positionLimit": "50"
    }
  },
  "live": {
    "demo": true,
    "okx": {
      "restUrl": "https://openapi.okx.com",
      "privateWebSocketUrl": "wss://wspap.okx.com:8443/ws/v5/private"
    },
    "binance": {
      "webSocketUrl": "wss://ws-api.testnet.binance.vision/ws-api/v3"
    }
  }
}
```

| Field | Default | Description |
|---|---|---|
| `journal.path` | `state/orders.jsonl` | Append-only OMS journal path |
| `journal.durableWrites` | `true` | Enables coalesced background `fdatasync` |
| `eventQueueCapacity` | `4096` | SPSC execution-event ring capacity per venue |
| `reconciliationIntervalMs` | `30000` | Periodic reconciliation interval |
| `marketData.maximumAgeMs` | `5000` | Quotes older than this are non-executable |
| `marketData.*PublicWebSocketUrl` | example public URLs | Enables streaming when both venue URLs are present; otherwise REST polling is used |
| `server.address` | `127.0.0.1` | Bind address; do not expose to untrusted networks |
| `server.ioThreads` | `2` | Asio io_context thread count |
| `risk.*.maxOrderSize` | — | Per-instrument maximum order quantity |
| `risk.*.maxNotional` | — | Per-instrument maximum order notional |
| `risk.*.positionLimit` | — | Per-instrument net position limit |

---

## Demo scripts

### Automated full-system demo (simulation, no credentials)

```bash
./scripts/start_demo.sh
```

Builds the project, starts `abex_server` in simulation mode, waits for it to be ready, runs the
REST workflow, runs the CLI workflow, streams a few WebSocket messages, then shuts down cleanly.
All output is captured to `evidence/`.

### CLI workflow

```bash
./examples/demo.sh
```

Places, amends, and cancels orders on both simulated venues using the CLI.

### REST workflow (requires a running server)

```bash
# In one terminal:
./build/abex_server --mode simulation --config config/gateway.example.json

# In another terminal:
./examples/rest_demo.sh
```

Places, amends, and cancels orders on both venues via the REST API.

### Fault-tolerance and crash-recovery interactive demo

```bash
# Interactive (pauses between sections)
./scripts/demo_failover.sh

# Fully automated (no pauses, suitable for CI)
./scripts/demo_failover.sh --auto
```

Walks through 10 crash/failover scenario groups covering process restart, uncertain outcomes,
sequence gaps, fill races, and backpressure. All 127 tests are run at the end.

### Structured evidence capture

```bash
./scripts/run_fault_tolerance_evidence.sh
# Saves to evidence/fault_tolerance_TIMESTAMP.txt
```

---

## OMS capture, restart, and retry evidence

Every create, amend, cancel, acknowledgement, execution update, retry, connection change, and
reconciliation is represented in the OMS journal. Order intent and its operation identifier are
written before venue I/O. Records are append-only JSONL with monotonic sequence numbers and
checksums; with the default `durableWrites=true`, a background worker coalesces `fdatasync`
requests after complete records are appended. A retained OS file lock permits only one writer.

At startup, the latest complete snapshot for every order is recovered. The gateway enumerates
venue open orders (including OKX `/trade/orders-pending`) and correlates only orders owned by the
durable journal. Account-wide orders created by another application, journal, or venue UI are
ignored and never adopted, canceled, or treated as gateway health failures. Journaled non-terminal
orders missing from the snapshot are queried individually. The same reconciliation runs
periodically and after reconnect. Identical retries replay the durable result without making a
second venue request; uncertain outcomes remain `UNKNOWN` until reconciliation rather than being
guessed as rejected or canceled.

The UI's **OMS stability** panel shows recovered orders, current-run retries, reconciliations,
journal sequence/durability, and logging failures. Its durable event timeline includes process
starts/restarts, disconnects/reconnects, request intent/acknowledgement, unknown outcomes,
idempotency conflicts/replays, sequence gaps, backpressure, and reconciliation results. The same
evidence is available at `/api/v1/system` and streamed as `system.event` messages.

---

## Performance

See [docs/BENCHMARKING.md](docs/BENCHMARKING.md) for methodology, the complete result set, and
interpretation. Latest unpinned Release run (2026-08-29, GCC 13.4, WSL2):

| Workload | p99 | Mechanism |
|---|---:|---|
| Latest market quote lookup | 31 ns | Four-slot seqlock |
| Order state-machine report | 90 ns | Allocation-free transition path |
| Decimal caller-buffer formatting | 29 ns | No returned-string allocation |
| Two-venue SPSC execution lanes | 598 ns/event | Independent per-venue rings |
| Simulated place, single caller | 92.1 µs/order | Non-durable gateway benchmark |
| Simulated place, four callers | 268.1 µs/order | p99 under concurrent callers |
| Journal append, non-durable | 14.8 µs/record | Complete serialized record write |
| Journal append, background sync enabled | 76.9 µs/record | p99; p50 4.99 µs, 100 samples |

With `durableWrites=true`, the measured command path appends a complete record and signals the
coalescing sync worker; it does not wait for `fdatasync`. Benchmark numbers are not venue latency
or production SLOs. Pin and isolate the process and increase low-sample workloads before capacity
planning.

---

## Security boundary and limitations

Client authentication, TLS termination, and production secret/key management are intentionally
outside this exercise (see `project_requirements.txt`). The server binds to `127.0.0.1` by
default; do not expose it to an untrusted network.

Live exchange tests are not run automatically because they mutate external demo accounts and are
nondeterministic. Protocol serialization, normalization, signing primitives, the common API, and
the server transports are covered locally.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#20-known-limitations-and-production-roadmap)
for the production roadmap.
