# ABEX Exchange Gateway — Submission

## Table of Contents

1. [Setup and Installation](#1-setup-and-installation)
2. [Running the Application](#2-running-the-application)
3. [System Pipeline and Flow](#3-system-pipeline-and-flow)
4. [Class Design](#4-class-design)
5. [Benchmark Results](#5-benchmark-results)
6. [Assumptions](#6-assumptions)
7. [Future Improvements](#7-future-improvements)

---

## 1. Setup and Installation

### Language and Compiler

| Requirement | Version | Notes |
|---|---|---|
| C++ standard | C++20 | `std::jthread`, `std::stop_token`, concepts, ranges |
| GCC | 13+ | Primary build compiler |
| Clang | 18+ | Required for ASAN/TSAN sanitizer presets |
| CMake | 3.24+ | Presets API (`CMakePresets.json`) requires 3.24 |
| Ninja | any | Used by all CMake presets |

### Dependencies

| Library | Version | Purpose |
|---|---|---|
| Boost | 1.81 | Asio (async I/O), Beast (HTTP/WebSocket) |
| OpenSSL | 3.x | TLS for live venue connections; HMAC-SHA256 signing |
| libcurl | 7.x | Synchronous REST transport (OKX REST, balance/instrument queries) |
| nlohmann/json | 3.11 | JSON serialization throughout |
| yaml-cpp | 0.7 | YAML configuration file support |
| Catch2 | 3.x | Test framework (127 deterministic tests) |

### Fresh Ubuntu 24.04 Install

Ubuntu 24.04 apt ships CMake 3.22, nlohmann/json 3.10, Boost 1.74, and Catch2 v2 — all below
the required minimums. Build the four out-of-date dependencies from source:

```bash
# 1. Compiler, Ninja, OpenSSL, libcurl, yaml-cpp
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

# 3. nlohmann/json 3.11
git clone --depth 1 --branch v3.11.3 https://github.com/nlohmann/json.git /tmp/json
cmake -S /tmp/json -B /tmp/json/build -DJSON_BuildTests=OFF
sudo cmake --build /tmp/json/build --target install

# 4. Boost 1.81
wget -q https://archives.boost.io/release/1.81.0/source/boost_1_81_0.tar.gz -O /tmp/boost.tar.gz
tar -xf /tmp/boost.tar.gz -C /tmp
cd /tmp/boost_1_81_0 && ./bootstrap.sh --with-libraries=system
sudo ./b2 install -j$(nproc)

# 5. Catch2 v3
git clone --depth 1 --branch v3.6.0 https://github.com/catchorg/Catch2.git /tmp/catch2
cmake -S /tmp/catch2 -B /tmp/catch2/build -DCATCH_INSTALL_DOCS=OFF
sudo cmake --build /tmp/catch2/build --target install -j$(nproc)
```

### Build

```bash
# Debug build with tests (recommended for evaluation)
cmake --preset debug
cmake --build --preset debug -j$(nproc)

# Release build
cmake --preset release
cmake --build --preset release

# Release build with benchmarks
cmake --preset release -DABEX_BUILD_BENCHMARKS=ON
cmake --build --preset release --target abex_benchmark
```

### Memory and Thread Safety Tools

Four sanitizer presets are provided. ASAN and TSAN are mutually exclusive.

| Preset | Tool | What it checks |
|---|---|---|
| `asan` | GCC AddressSanitizer + LeakSanitizer + UBSan | Memory errors, leaks, undefined behaviour |
| `clang-asan` | Clang 18 ASan + LSan + UBSan | Same, with Clang's more precise diagnostics |
| `clang-tsan` | Clang 18 ThreadSanitizer | Data races and lock-order violations |

```bash
# AddressSanitizer (GCC)
cmake --preset asan && cmake --build --preset asan
ctest --preset asan
# Expected: 127/127 passed, zero issues

# ThreadSanitizer (Clang 18) — run serially for clean race reports
cmake --preset clang-tsan && cmake --build --preset clang-tsan
ctest --preset clang-tsan -j1
# Expected: 127/127 passed, zero data races
```

Both sanitizer suites pass 127/127 with zero reported issues.

---

## 2. Running the Application

### State directory

The server writes its journal and market-data ring to `state/`. Create it before first run:

```bash
mkdir -p state
```

### Simulation mode — safe, no credentials required

```bash
./build/abex_server --mode simulation --config config/gateway.example.json
```

Wait for `ABEX complete setup ready`, then open <http://127.0.0.1:8080>.
Stop with `Ctrl-C` or `kill -TERM <pid>`.

### Live mode — OKX demo + Binance testnet

```bash
cp .env.example .env
# Fill in the five credential variables:
#   ABEX_OKX_API_KEY, ABEX_OKX_SECRET_KEY, ABEX_OKX_PASSPHRASE
#   ABEX_BINANCE_API_KEY, ABEX_BINANCE_SECRET_KEY
chmod 600 .env
./build/abex_server --config config/gateway.example.json
```

The default `config/gateway.example.json` points to the OKX demo environment
(`wss://wspap.okx.com:8443/ws/v5/private`) and the Binance testnet
(`wss://ws-api.testnet.binance.vision/ws-api/v3`).

### CLI

```bash
# Interactive session
./build/abex_cli --mode simulation --config config/gateway.example.json

# Single command
./build/abex_cli --mode simulation --state /tmp/abex.jsonl --command \
  'place --id o1 --venue OKX --symbol BTC-USDT --side BUY --type LIMIT --price 30000 --qty 0.1 --tif IOC'
```

### Run all 127 tests

```bash
ctest --preset debug --output-on-failure
```

### Automated demo (no credentials)

```bash
./scripts/start_demo.sh --auto
```

---

## 3. System Pipeline and Flow

### Process topology

```
┌─────────────────────────────────────────────────────────────────┐
│  abex_server (OMS process)                                      │
│                                                                 │
│  Browser / REST client / CLI                                    │
│          │ common OrderRequest schema                           │
│          ▼                                                      │
│    GatewayApi  ──────────────────────────────────────────────┐  │
│          │ validate + deserialize                             │  │
│          ▼                                                    │  │
│    OrderGateway                                               │  │
│    ├─ idempotency check (clientOrderId / requestId)           │  │
│    ├─ RiskManager preflight                                   │  │
│    ├─ TokenBucket rate-limit check                            │  │
│    ├─ append intent ──► FileOrderStore (JSONL journal)        │  │
│    │                         └──► background fdatasync        │  │
│    ├─ OkxAdapter ──► REST place/cancel/amend                  │  │
│    │                 private WebSocket updates ──┐            │  │
│    └─ BinanceAdapter ──► signed WS commands      │            │  │
│                          user-data stream ───────┤            │  │
│                                                  ▼            │  │
│                              SpscExecutionLane (per venue)    │  │
│                                                  │            │  │
│                                                  ▼            │  │
│                              OrderStateMachine.apply()        │  │
│                                                  │            │  │
│                              persist + notify observers ──────┘  │
│                                                  │               │
│                              REST snapshots / WebSocket / UI     │
└─────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────┐
│  abex_market_data (child process, supervised by abex_server)     │
│                                                                  │
│  OKX public WebSocket ──► normalize ──┐                          │
│  Binance public WebSocket ──────────► ├──► MarketDataRingWriter  │
│  REST polling fallback ─────────────► ┘         │               │
│                                        mmap ring file            │
└──────────────────────────────────────────────────────────────────┘
                                               │
                                               ▼
                              MarketDataRingReader (in abex_server)
                                               │
                              MarketDataBook ──► risk / REST / WS / UI
```

### Order command pipeline (place)

```
validate schema
    │
    ▼
resolve idempotency  ──► duplicate clientOrderId + same fingerprint → replay cached result
    │                    duplicate clientOrderId + different fields  → reject
    ▼
RiskManager.check_new_with_position()
    ├─ max order size
    ├─ max notional  (MARKET uses current mapped quote)
    └─ position limit (conservative: open orders + realized fills)
    │
    ▼
TokenBucket.try_acquire()  ──► reject with RATE_LIMITED if exhausted
    │
    ▼
create Order{status=UNKNOWN, pendingAction=New}
    │
    ▼
FileOrderStore.append_order(intent_only=true)   ← journal before I/O
    │
    ▼
IExchangeAdapter.place()
    │
    ├─ OKX: POST /api/v5/trade/order  (REST)
    └─ Binance: order.place over signed WebSocket
    │
    ▼
apply AdapterResult → update Order status
    │
    ▼
FileOrderStore.commit_order()  ← full snapshot
    │
    ▼
notify observers → WebSocket clients / CLI
```

### Execution report ingestion

```
venue WebSocket frame
    │
    ▼
OkxAdapter / BinanceAdapter  (I/O thread)
    │  normalize to ExecutionReport
    ▼
SpscExecutionLane.submit()   ← lock-free enqueue
    │
    ▼
OrderGateway execution consumer thread
    │
    ▼
locate_order_locked()  ← by exchangeOrderId or clientOrderId alias
    │
    ▼
OrderStateMachine.apply()
    ├─ duplicate event_id → no-op
    ├─ stale sequence → merge fills, no status regression
    ├─ historical generation (Binance cancel-replace) → fills only
    └─ normal → update status, fills, average price
    │
    ▼
persist + notify
```

### Recovery on startup

```
FileOrderStore.load_latest()
    │  latest complete record per clientOrderId
    ▼
restore exchange alias indexes
    │
    ▼
IExchangeAdapter.query_open_orders()  (per venue)
    │  ownership boundary: only journal-owned orders considered
    ▼
reconcile: apply venue truth to local state
    │
    ▼
query individually any journaled non-terminal order absent from snapshot
    │
    ▼
periodic reconciliation every 30 s + after each reconnect
```

---

## 4. Class Design

### Layer overview

```
domain          ──  Order, OrderStateMachine, Decimal, ExecutionReport
application     ──  OrderGateway, RiskManager, MarketDataBook, SpscExecutionLane
ports           ──  IExchangeAdapter, IOrderStore
infrastructure  ──  OkxAdapter, BinanceAdapter, SimulatedExchangeAdapter,
                    FileOrderStore, MarketDataRingWriter/Reader,
                    ReconnectingWebSocket, TokenBucket, HttpClient
bootstrap       ──  GatewayRuntime, ConfigLoader, Environment
interfaces      ──  GatewayApi, HttpServer, CommandProcessor, JsonViews
```

Dependencies point strictly inward: infrastructure → application → domain.
No domain or application code has any HTTP, filesystem, or exchange-SDK dependency.

---

### Domain layer

#### `Order` (domain/order.hpp)

The canonical venue-neutral order record. Holds all lifecycle state for one logical order,
including Binance cancel-replace generation aliases so late fills from superseded physical
orders can be merged without regressing the active order's status.

Key fields:
- `client_order_id` — client-assigned, immutable, idempotency key for placement
- `exchange_order_id` + `exchange_order_id_aliases` — current and historical venue IDs
- `status` / `pending_action` — current lifecycle state and in-flight operation
- `filled_quantity` / `cumulative_quote` / `average_fill_price` — monotonic fill accounting
- `processed_event_ids` — deduplication set for execution reports
- `processed_requests` — idempotency map for amend/cancel `requestId`s

#### `OrderStateMachine` (domain/order_state_machine.hpp)

A pure static class with a single `apply(Order&, ExecutionReport)` entry point.
All state transitions are deterministic and tested with 20,000 randomized inputs.

Race policies encoded in the state machine:
- Full fill beats a concurrent cancel — `FILLED` is terminal and cannot be reopened
- Stale sequence (out-of-order delivery) — fills are merged but status cannot regress
- Historical generation (Binance replacement) — fills contributed, lifecycle not touched
- Duplicate `event_id` — silent no-op, `ApplyDisposition::Duplicate` returned

#### `Decimal` (domain/decimal.hpp)

Fixed-point 8-decimal arithmetic backed by a 64-bit integer. No binary floating-point
is used in any monetary field, order identity check, or risk decision. Overflow is
detected at construction. Formatting uses a caller-supplied stack buffer with no heap
allocation (p99 29 ns).

#### `ExecutionReport` (domain/execution_report.hpp)

Normalized execution event produced by adapters. Contains `event_id` for deduplication,
`sequence` for gap detection, `cumulative_filled` for monotonic fill accounting, and
`status` for lifecycle transitions. Adapters are the only code that reads raw venue JSON.

---

### Application layer

#### `OrderGateway` (application/order_gateway.hpp)

The central application service and composition root for the order path. Owns:
- the per-venue adapter map
- the single state mutex protecting all order and position state
- two `SpscExecutionLane` instances (one per venue)
- the `RiskManager` and `MarketDataBook`
- the `AsyncOrderObserverQueue` for non-blocking WebSocket fan-out
- the reconciliation `jthread`

The lock discipline is strict: the state mutex is never held across network I/O,
journal writes, JSON encoding, or observer callbacks. The two-phase persist pattern
(`prepare_persist` inside lock, `commit_persist` outside) keeps serialization off
the critical section.

#### `RiskManager` (application/risk_manager.hpp)

Stateless preflight evaluator. Checks per-instrument `maxOrderSize`, `maxNotional`,
and `positionLimit` before every placement and amendment. MARKET orders use the current
mapped quote for notional estimation. All rejections are persisted as `REJECTED` orders
with machine-readable codes and human-readable reasons.

#### `SpscExecutionLane` (application/spsc_execution_lane.hpp)

Lock-free single-producer/single-consumer ring connecting the adapter I/O thread
(producer) to the gateway execution consumer. Separate lanes per venue prevent a
slow or reconnecting venue from blocking the other. Overflow increments
`dropped_events` and sets `reconciliation_required`.

#### `MarketDataBook` (application/market_data_book.hpp)

In-memory quote cache populated by the mmap ring reader. Provides `best_bid`,
`best_ask`, and freshness checks. Quotes older than `maximumAgeMs` (default 5 s)
are treated as unavailable and block MARKET order submission.

#### `SequenceTracker` (application/sequence_tracker.hpp)

Per-venue monotonic sequence validator. Detects gaps, out-of-order delivery, and
resets. Gaps degrade venue health and schedule reconciliation.

---

### Ports (interfaces)

#### `IExchangeAdapter` (ports/exchange_adapter.hpp)

The only boundary between `OrderGateway` and venue-specific code. Methods:
`place`, `cancel`, `amend`, `query`, `query_open_orders`, `query_balances`,
`query_instrument_rules`. `OrderGateway` contains zero venue JSON, signing,
or socket logic.

`AdapterResult` carries `outcome_uncertain` for network-timeout cases where the
venue may or may not have processed the request. Uncertain outcomes leave the order
`UNKNOWN` until reconciliation resolves them.

#### `IOrderStore` (ports/order_store.hpp)

Append-only journal interface. The two-phase API (`reserve_sequence` / `commit_order`)
lets the gateway snapshot and sequence an order inside the state mutex, then serialize
and write outside it — preserving journal ordering without holding the lock during I/O.

---

### Infrastructure layer

#### `OkxAdapter` (infrastructure/okx_adapter.hpp)

- Place / cancel / amend via authenticated REST (`/api/v5/trade/order` etc.)
- Execution updates via private WebSocket `orders` channel
- Startup reconciliation via `GET /api/v5/trade/orders-pending`
- HMAC-SHA256 request signing with ISO-8601 timestamps

#### `BinanceAdapter` (infrastructure/binance_adapter.hpp)

- All order commands via signed WebSocket API (`order.place`, `order.cancel`,
  `order.cancelReplace`)
- Execution updates via user-data stream (`executionReport`)
- `rateLimits` fields in every response continuously resynchronize the local
  `TokenBucket` capacity, consumption, and refill rate
- Cancel-replace generation tracking: each physical replacement gets an alias
  so fills from the superseded order are merged into the canonical order

#### `SimulatedExchangeAdapter` (infrastructure/simulated_exchange_adapter.hpp)

Drop-in replacement for both live adapters in `--mode simulation`. Fills orders
immediately from the current mapped market quote. Uses the same `IExchangeAdapter`
interface so the entire application path — risk, journal, state machine, observers —
is exercised identically. No credentials required.

#### `FileOrderStore` (infrastructure/file_order_store.hpp)

Append-only JSONL journal with:
- Exclusive process-level file lock (prevents split-brain writers)
- Per-record CRC32 checksum and monotonic sequence number
- Torn-tail detection and repair on startup
- Optional background `fdatasync` worker that coalesces sync requests,
  keeping filesystem-sync latency off the command path

#### `ReconnectingWebSocket` (infrastructure/reconnecting_websocket.hpp)

TLS WebSocket transport with:
- Hostname verification
- Bounded exponential backoff (configurable min/max delay)
- Write serialization (one in-flight frame at a time)
- Subscription and authentication replay after each reconnect

#### `TokenBucket` (infrastructure/rate_limiter.hpp)

Lock-free Generic Cell Rate Algorithm (GCRA) implementation. One 64-bit CAS per
admission. `static_assert` guarantees the atomic is always lock-free. Binance
`rateLimits` response fields call `synchronize()` to continuously correct local
capacity and refill rate against venue truth.

#### `MarketDataRingWriter` / `MarketDataRingReader` (infrastructure/market_data_ring.hpp)

Fixed-layout POSIX memory-mapped ring buffer shared between `abex_market_data`
(writer) and `abex_server` (reader). Each slot uses a seqlock protocol so readers
detect concurrent writes and retry without a process-shared mutex. Generation
tracking detects publisher restarts and invalidates stale quotes.

---

### Bootstrap and interfaces

#### `GatewayRuntime` (bootstrap/gateway_runtime.hpp)

Composition root. Constructs the full object graph in dependency order:
config → credentials → journal → market-data ring → risk → adapters →
`OrderGateway` → workers → HTTP/WebSocket listener.

#### `GatewayApi` (server/gateway_api.hpp)

REST handler. Validates and deserializes the common schema, rejects unknown fields,
calls `OrderGateway`, and serializes normalized responses. Publishes an OpenAPI 3.1
document at `/api/v1/openapi.json`.

#### `HttpServer` (server/http_server.hpp)

Boost Beast async HTTP and WebSocket server. Manages the `/ws/v1/orders` stream:
sends `orders.snapshot`, `market.snapshot`, `system.snapshot` on connect, then
streams `order.updated`, `market.updated`, `system.event`. Bounded 256-message
outbound queue per connection; slow consumers receive `resync.required`.

#### `JsonViews` (presentation/json_views.hpp)

All serialization to client-facing JSON is centralized here. No adapter or gateway
code constructs client JSON directly.

---

## 5. Benchmark Results

Full methodology and raw distributions are in [docs/BENCHMARKING.md](docs/BENCHMARKING.md).

Environment: Release build, GCC 13.4, WSL2, Intel Core Ultra 7 155H, not CPU-pinned.

### Hot-path primitives (p99)

| Workload | p99 | Mechanism |
|---|---:|---|
| Market quote lookup | 31 ns | Four-slot seqlock read |
| Order state-machine transition check | 90 ns | Allocation-free path |
| Decimal stack formatting | 29 ns | No heap allocation |
| Risk preflight check | 48 ns | In-memory only |
| mmap quote read | 47 ns | Shared-memory snapshot |

### Inter-thread transport (p99)

| Workload | p99 | Mechanism |
|---|---:|---|
| SPSC execution lane | 514 ns | Single producer-to-consumer handoff |
| Two-venue execution ingestion | 598 ns | Independent per-venue rings |
| Burst execution lanes | 666 ns | Contended burst ingestion |

### Journal (p99)

| Workload | Samples | p99 | Notes |
|---|---:|---:|---|
| Append, background sync disabled | 5,000 | 14,835 ns | Serialized complete-record write |
| Append burst, sync disabled | 20,000 | 15,085 ns | Sustained workload |
| Append, background sync enabled | 100 | 76,949 ns | Append + worker notification; caller does not wait for `fdatasync` |

### Gateway placement (p99)

| Workload | Samples | p99 | Notes |
|---|---:|---:|---|
| Single caller, sync disabled | 1,000 | 92,117 ns | Simulated venues |
| Burst submission | workload | 110,429 ns | Simulated burst |
| Four concurrent callers | workload | 268,103 ns | Multi-producer contention |
| Idempotent replay | workload | 2,243 ns | Cached result, no venue call |
| Position read under writes | workload | 2,440 ns | Atomic snapshot |

These are local component measurements. They exclude public-network latency,
venue matching-engine latency, and TLS negotiation.

---

## 6. Assumptions

### Scope

- **Authentication and TLS termination are out of scope** per the exercise brief.
  The server binds to `127.0.0.1` by default. A production deployment requires
  an authenticated reverse proxy, mTLS, and secret rotation outside this repository.

- **Single-instance only.** The journal uses an exclusive file lock. Multi-instance
  HA, leader fencing, and replicated WAL are not implemented.

- **Three symbols.** Risk limits and the market-data ring are configured for
  `BTC-USDT` and `ETH-USDT`. Adding a symbol requires a config entry and a
  `SymbolIndex` slot (compile-time constant `kSymbolCount`).

### Exchange behaviour

- **OKX demo environment** behaves like production for order lifecycle but may
  return different instrument rules or reject certain order sizes.

- **Binance testnet** rejects `userDataStream.subscribe` with a signature error
  (a known testnet limitation). The adapter uses a failover pattern: it marks the
  user-data stream as subscribed regardless of the subscribe response, so execution
  reports still arrive via the authenticated WebSocket session.

- **Binance cancel-replace** is used for amendments. Each replacement creates a new
  physical `exchangeOrderId`. The adapter tracks generation aliases so fills from
  superseded orders are merged into the canonical client order.

- **OKX market orders** receive an immediate follow-up REST query after placement
  because the private WebSocket fill may arrive before the REST acknowledgement.

### Persistence

- `durableWrites=true` (the default) provides **append-before-route** ordering with
  **bounded asynchronous** `fdatasync`. A process or machine failure inside the
  outstanding sync window can lose recently appended records that were already routed.
  This is a deliberate trade-off: storage-flush latency is kept off the command path.
  Strict stable-storage-before-route requires synchronous commit or a replicated WAL.

### Market data

- The market-data process is supervised by `abex_server` in the default configuration.
  If the child fails to publish a first quote within 30 seconds, startup is aborted.
  In `--no-market-data` mode the server starts without quotes; MARKET orders will be
  rejected until the external publisher provides a fresh quote.

### Timestamps

- ABEX does not use `RDTSC`. Binance timestamps are anchored to the venue's `time`
  response, advanced with `steady_clock`, corrected by the measured request midpoint,
  and biased backward by the larger of the configured safety margin and measured
  half-RTT uncertainty.

---

## 7. Future Improvements

### Correctness and durability

- **Synchronous journal option.** Add a `durableWrites=sync` mode that calls
  `fdatasync` on the command path for deployments that require stable-storage-before-route
  semantics, accepting the latency cost explicitly.

- **Journal compaction.** The journal is append-only indefinitely. Production deployments
  need crash-safe snapshot/compaction with atomic file replacement, directory syncing,
  retained audit evidence, and a fallback to the previous generation.

- **Dead-letter handling.** Execution reports that cannot be correlated to any known
  order (e.g. orders placed by another process on the same account) are currently
  silently ignored. A dead-letter queue with alerting would improve operational visibility.

### Resilience

- **Stronger sequence recovery.** Current gap detection degrades health and schedules
  reconciliation. A more aggressive path would request a replay window from the venue
  before falling back to full reconciliation.

- **Multi-crash soak testing.** The crash-recovery test suite covers discrete scenarios.
  A prolonged chaos harness (random kills, network partitions, clock jumps) would
  validate the recovery invariants under sustained adversarial conditions.

- **Venue outage simulation.** The benchmark harness does not model prolonged venue
  outage, saturated client WebSockets, or multi-hour journal growth.

### Performance

- **CPU pinning and NUMA awareness.** The execution consumer and reconciliation worker
  would benefit from dedicated isolated cores in a production deployment.

- **Pre-serialized JSON.** The observer fan-out currently serializes the order once
  per notification cycle. Pre-serializing to a shared buffer and reference-counting
  it across WebSocket sessions would reduce allocation under high client counts.

- **Lock-free order index.** The primary state mutex is held for all order lookups.
  A concurrent hash map (e.g. `tbb::concurrent_hash_map`) would reduce contention
  under many concurrent REST callers.

### Operational

- **Metrics export.** Prometheus or OpenTelemetry counters for queue depth, drop rate,
  reconciliation latency, journal sync lag, and rate-limit events would enable
  capacity alerts and defined SLOs.

- **Multi-instance fencing.** Horizontal availability requires a distributed lock or
  leader-election protocol to prevent two gateway instances from owning the same
  journal and issuing conflicting venue commands.

- **Secret management integration.** Credentials are currently loaded from environment
  variables. A production deployment should integrate with a secrets manager
  (AWS Secrets Manager, HashiCorp Vault, etc.) with automatic rotation and audit logging.

- **Authenticated API ingress.** The built-in HTTP server is a local/demo interface.
  Production requires authenticated TLS ingress, origin policy, request-size limits,
  and log redaction at a trusted reverse proxy.

---

*For the full architecture invariants and concurrency model see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
For benchmark methodology and raw distributions see [docs/BENCHMARKING.md](docs/BENCHMARKING.md).
For build and operating instructions see [README.md](README.md).*
