# Architecture, Engineering Decisions, Performance, and Journal Design

## Table of contents

1. [Layer model and dependency direction](#layer-model-and-dependency-direction)
2. [Process topology](#process-topology)
3. [Client-facing APIs](#client-facing-apis)
4. [Market-data process boundary](#market-data-process-boundary)
5. [Common order schema](#common-order-schema)
6. [Explicit venue mapping](#explicit-venue-mapping)
7. [State machine and race rules](#state-machine-and-race-rules)
8. [Idempotency](#idempotency)
9. [Risk pipeline](#risk-pipeline)
10. [Persistence and recovery](#persistence-and-recovery)
11. [Resilience and backpressure](#resilience-and-backpressure)
12. [Rate-limiter design](#rate-limiter-design)
13. [Threading model](#threading-model)
14. [Synchronization design](#synchronization-design)
15. [Lock-free inventory](#lock-free-inventory)
16. [Synchronization trade-offs and pros/cons](#synchronization-trade-offs-and-proscons)
17. [Performance engineering](#performance-engineering)
18. [Benchmark results](#benchmark-results)
19. [Journal compaction design](#journal-compaction-design)
20. [OOAD and SOLID review](#ooad-and-solid-review)
21. [Authentication and key-management boundary](#authentication-and-key-management-boundary)
22. [Known limitations and scaling path](#known-limitations-and-scaling-path)

---

## Layer model and dependency direction

ABEX follows a ports-and-adapters (hexagonal) design. Dependencies always point inward toward the
domain. No layer imports from a layer outside it.

```
┌─────────────────────────────────────────────────────────────────┐
│  Presentation                                                   │
│  Browser UI ── REST/WebSocket ──┐                               │
│  CLI ───────────────────────────┤                               │
│                                 ▼                               │
│  Application                                                    │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  OrderGateway  ──  RiskManager  ──  OperationalEventWriter│  │
│  └──────────┬──────────────────────────────────┬────────────┘   │
│             │ IExchangeAdapter port             │ IOrderStore port│
│  Infrastructure                                │                │
│  ┌──────────┴──────────┐              ┌────────┴──────────┐     │
│  │ OkxAdapter          │              │ FileOrderStore    │     │
│  │ BinanceAdapter      │              └───────────────────┘     │
│  │ SimulatedAdapter    │                                        │
│  └─────────────────────┘                                        │
│                                                                 │
│  Domain  (zero external dependencies)                           │
│  Order · Decimal · OrderStateMachine · RiskDecision             │
└─────────────────────────────────────────────────────────────────┘
```

The domain layer knows nothing about JSON, files, sockets, threads, or exchanges. `OrderGateway`
coordinates use cases through `IExchangeAdapter` and `IOrderStore`. Exchange adapters translate
protocols; the risk manager makes risk decisions; the state machine owns lifecycle rules; each
presentation adapter only parses and presents use cases.

A shared `GatewayRuntime` is the composition root used by both executables. Live and simulated
exchange adapters are compiled into the same binary. `--mode live|simulation` selects them at
startup; trading mode never depends on preprocessor state. The source tree has zero `#if`,
`#ifdef`, or `#ifndef` directives.

---

## Process topology

```
┌──────────────────────────────────────────────────────────────────┐
│  abex_server  (primary OMS process)                              │
│                                                                  │
│  ┌────────────┐  ┌──────────────┐  ┌──────────────────────────┐ │
│  │OrderGateway│  │ REST/WS API  │  │ Browser UI (static files)│ │
│  │ + Journal  │  │ (Boost.Beast)│  │ web/index.html           │ │
│  └─────┬──────┘  └──────────────┘  └──────────────────────────┘ │
│        │                                                         │
│  ┌─────┴──────────────────────────────────────────────────────┐  │
│  │  IExchangeAdapter                                          │  │
│  │  ┌──────────────────┐  ┌──────────────────────────────┐   │  │
│  │  │ OkxAdapter       │  │ BinanceAdapter               │   │  │
│  │  │ REST place/cancel│  │ WS order.place/cancel/replace│   │  │
│  │  │ WS private orders│  │ WS user-data executionReport │   │  │
│  │  └──────────────────┘  └──────────────────────────────┘   │  │
│  └────────────────────────────────────────────────────────────┘  │
│                                                                  │
│  mmap ring reader ◄── state/market-data.ring                     │
│  (supervised child pipe) ◄── abex_market_data readiness byte     │
└──────────────────────────────────────────────────────────────────┘
                                    ▲
                                    │ mmap write
┌──────────────────────────────────────────────────────────────────┐
│  abex_market_data  (separate publisher process)                  │
│  OKX public REST + Binance public REST → ring-buffer file        │
│  No credentials. No OMS. No HTTP server.                         │
└──────────────────────────────────────────────────────────────────┘
```

`abex_server` owns exchange connectivity, the order journal, REST/WebSocket APIs, and the browser
UI. By default it forks/execs `abex_market_data` as a supervised child and waits on a one-byte
readiness channel for its first successful quote. `--no-market-data` disables supervision for
systemd, containers, or manual multi-process operation.

---

## Client-facing APIs

`GatewayApi` is independent of sockets, making routing and status mapping deterministic and
unit-testable. `HttpServer` adapts Boost.Beast requests to it and serves only a fixed whitelist of
UI assets. Unknown order fields are rejected, preventing exchange-specific concepts from leaking
through the client boundary. The OpenAPI 3.1 document is available at `/api/v1/openapi.json`.

CLI commands call the same `OrderGateway` application service in-process. Both paths produce
identical normalized JSON views.

The WebSocket at `/ws/v1/orders` starts with `orders.snapshot`, `market.snapshot`, and
`system.snapshot`, then sends `order.updated`, `market.updated`, and `system.event`. Each
connection has a 256-message outbound bound. A slow consumer receives `resync.required` and
reloads authoritative REST snapshots. A slow socket never blocks exchange-state processing.

---

## Market-data process boundary

The child is an unauthenticated publisher with no OMS, exchange-order, HTTP, or UI ownership.
Every second it fetches top-of-book bid/ask for BTC-USDT and ETH-USDT from both venues
concurrently so one slow venue does not delay the other.

The publisher is the only writer to a fixed-layout POSIX memory-mapped ring file. An advisory file
lock prevents a second writer. Each slot is copied first and committed with a release-store
sequence; the reader uses acquire loads and a second sequence check to reject torn or overwritten
slots. A generation value detects publisher restarts; a lagging reader resumes at the oldest
record still retained by the configured capacity.

`GatewayRuntime` tails this file without making market-data network requests. It builds the
current in-memory book, exposes it at `/api/v1/market-data`, and emits it over the client
WebSocket. A five-second default maximum age makes stale data non-executable.

---

## Common order schema

The client model contains `clientOrderId`, venue, canonical `BASE-QUOTE` symbol, side, type,
optional limit price, base-asset quantity, and time-in-force. All monetary fields are `Decimal`,
a signed fixed-point integer scaled to eight decimal places.

Internally an order also records:

- Current and historical physical exchange identifiers
- Per-generation fill/quote offsets (for Binance cancel-replace)
- Filled and quote quantities, average fill price
- Authoritative and pending amend terms
- A monotonic local version counter
- The last meaningful venue sequence number
- Processed event IDs (duplicate suppression)
- Request fingerprints (idempotency ledger)

Internal bookkeeping is persisted but excluded from client responses.

---

## Explicit venue mapping

| Common concept | OKX | Binance Spot |
|---|---|---|
| Symbol | `BTC-USDT` | `BTCUSDT` |
| Client ID | `clOrdId` (≤32 alphanumeric) | `newClientOrderId` |
| Limit/GTC | `ordType=limit` | `type=LIMIT,timeInForce=GTC` |
| Limit/IOC | `ordType=ioc` | `type=LIMIT,timeInForce=IOC` |
| Limit/FOK | `ordType=fok` | `type=LIMIT,timeInForce=FOK` |
| Market buy quantity | `sz` with `tgtCcy=base_ccy` | `quantity` |
| Place | REST `/trade/order` | WS `order.place` |
| Cancel | REST `/trade/cancel-order` | WS `order.cancel` |
| Amend | REST `/trade/amend-order` | WS `order.cancelReplace` |
| Updates | private `orders` channel | signed user-data `executionReport` |
| Open orders | REST `/trade/orders-pending` | WS `openOrders.status` |

Binance quantity-reduction can use `order.amend.keepPriority`, but the common amend contract also
allows price changes and increases. ABEX therefore uses cancel-replace consistently. A generated
exchange-client alias is correlated back to the stable client ID and persisted. This sacrifices
queue priority but gives deterministic cross-venue semantics.

OKX client IDs longer than 32 characters or containing non-alphanumeric characters are mapped to
a deterministic `abx<prefix><fnv1a-hex>` form. The mapping is stable across restarts.

---

## State machine and race rules

Public states are `LIVE`, `PARTIALLY_FILLED`, `FILLED`, `CANCELED`, `REJECTED`, and operational
`UNKNOWN`. `pendingAction` distinguishes an accepted local intent from venue confirmation.

```
                  ┌─────────────────────────────────────────┐
                  │              OrderStateMachine           │
                  │                                         │
  place()  ──►  LIVE ──► PARTIALLY_FILLED ──► FILLED (terminal)
                  │              │
                  ▼              ▼
               CANCELED (terminal)
                  │
                  ▼
               REJECTED (terminal)
                  │
                  ▼
               UNKNOWN (operational; resolved by reconciliation)
```

Merge rules enforced by `OrderStateMachine::apply`:

- A duplicate `eventId` is a no-op; version is not incremented.
- Cumulative filled quantity never decreases.
- A lower sequence may contribute previously unseen fill quantity but cannot overwrite a newer
  lifecycle decision.
- `FILLED` is final and cannot regress to canceled or live.
- A full cumulative fill wins a cancel/fill race.
- A superseded Binance generation may add a late fill, but its `CANCELED` state cannot cancel the
  active replacement. Physical cumulative quantities are translated through persisted offsets.
- An amend acknowledgement never changes visible price/quantity by itself.
- A terminal order may retain later-discovered partial execution, but its terminal state remains
  unless the full quantity is proven filled.
- The create intent is journaled before exchange I/O, making an execution report that beats the
  acknowledgement correlatable by `clientOrderId`.
- An acknowledgement timeout is not treated as rejection; the order becomes `UNKNOWN` and requires
  reconciliation.

---

## Idempotency

Create idempotency is keyed by `clientOrderId` and a canonical request fingerprint. An exact retry
returns the existing order; reusing the ID with different fields returns `IDEMPOTENCY_CONFLICT`.

Cancel and amend accept a `requestId`. The order snapshot persists each request ID and fingerprint.
The same ID and payload is replayed without a second venue call; reuse with a different payload is
rejected.

Idempotency is enforced at two points:

1. Before the persistence lock is acquired — fast path for already-processed requests.
2. Inside the persistence lock after re-checking — prevents TOCTOU races under concurrent callers.

---

## Risk pipeline

Checks run before the create intent reaches an adapter, in this order:

```
1. Schema and positive-value validation
        │
        ▼
2. Market-data availability check (MARKET orders require a fresh quote)
        │
        ▼
3. Configured per-instrument maximum quantity and notional
        │
        ▼
4. Venue instrument rules (status, min/max qty, step, price range/tick, notional)
        │
        ▼
5. Conservative position limit
   — full-fill view of every open order plus realized fills on terminal orders
        │
        ▼
6. Authoritative venue available balance
   — BUY: quote currency at limit/current ask
   — SELL: base currency
   — query failure → BALANCE_UNAVAILABLE (fail closed)
   — shortfall → INSUFFICIENT_AVAILABLE_BALANCE (durably rejected)
```

Local rejections are persisted as normalized `REJECTED` orders with machine-readable codes and
human-readable reasons; they never reach an adapter.

The position model is intentionally approximate and nets buys against sells. Balance preflight is
authoritative at query time but cannot atomically reserve venue funds; the venue remains final
authority under concurrent account activity and price movement.

---

## Persistence and recovery

### Journal format

The OMS journal stores typed complete order snapshots and operational audit events in one
JSON-lines stream. Every record has:

- Schema version
- Monotonic record number
- Timestamp (ms since epoch)
- FNV-1a corruption checksum
- Record type (`order` or `event`)
- Full payload

Durable mode calls `fdatasync` before acknowledging the local transition. Order intent and its
request identity are durable before venue I/O. A retained advisory lock permits only one process
to own the journal, preventing split-brain writers.

### Recovery sequence

```
1. Load latest complete snapshot for each clientOrderId
2. Restore exchange ID aliases to adapter index
3. Start private WebSocket streams (OKX) / user-data streams (Binance)
4. Enumerate venue open orders
   ├── Correlate with durable journal (ownership boundary)
   ├── Ignore account-wide orders not in journal
   └── Apply execution reports for owned orders
5. Query individually: journaled non-terminal orders absent from open snapshot
6. Mark unresolvable orders UNKNOWN (never falsely cancel)
7. Emit GATEWAY_STARTED or GATEWAY_RESTARTED operational event
```

A malformed last record is treated as a torn append; corruption before the final record is fatal.
The same reconciliation process runs after reconnect and on a 30-second configurable interval.

### Operational event timeline

Operational records cover process starts/restarts, durable intents, acknowledgements, rejections,
unknown outcomes, idempotent retries and conflicts, disconnects/reconnects, sequence gaps,
backpressure, and reconciliation results. Each carries a process instance ID and optional
venue/order/request identity. They survive restart, are returned by `/api/v1/system`, and are
streamed to the UI.

---

## Resilience and backpressure

### Transport layer

The TLS WebSocket transport verifies hostnames, answers control frames, serializes writes,
reconnects with bounded exponential backoff, replays subscriptions/authentication after each
connection, and reports connected only after private authentication or user-data subscription
succeeds.

### Execution ingestion

Execution ingestion uses a bounded SPSC ring per venue adapter. Producers wait briefly when it is
full; persistent pressure increments `droppedEvents` and forces reconciliation. Local token
buckets fail fast before known order budgets are exceeded. Binance's returned `REQUEST_WEIGHT`
counters continuously synchronize its bucket.

### Client WebSocket output

Client WebSocket output is independently bounded at 256 messages per connection. It preserves the
buffer owned by an active asynchronous write, drops excess intermediate updates, and emits one
resynchronization instruction after the write completes. REST remains the authoritative recovery
path.

### Sequence tracking

Sequence numbers are tracked when an adapter can supply a contiguous sequence. Gaps degrade venue
health and demand reconciliation.

---

## Rate-limiter design

### Purpose

The rate limiter enforces per-venue request budgets before any venue I/O occurs. It prevents the
gateway from exceeding exchange-imposed request-weight limits, which would cause HTTP 429 responses
or temporary IP bans. It is also the mechanism by which Binance's authoritative `rateLimits`
response fields continuously resynchronize the local budget.

### Algorithm: lock-free GCRA

`TokenBucket` implements the Generic Cell Rate Algorithm (GCRA) using a single `atomic<int64_t>`
theoretical-arrival-time (TAT) cell. All configuration values are converted once to integer
micro-tokens and nanoseconds; the hot path performs one 64-bit CAS and deterministic integer
arithmetic with no heap allocation.

```
GCRA invariant:
  TAT = max(TAT_prev, now) + increment
  admit if TAT_prev ≤ now + tolerance
  where:
    increment = cost_units × period_ns / units_per_token
    tolerance = (capacity_units − cost_units) × period_ns / units_per_token
```

The `static_assert` that `atomic<int64_t>::is_always_lock_free` is a compile-time guarantee;
the limiter never falls back to a mutex.

### Token cost model

Each gateway operation that reaches an adapter consumes tokens according to its exchange weight:

```
place():
  query_balances()        → 1 token  (authoritative balance preflight)
  adapter->place()        → 1 token  (venue order submission)
  Total per placement     = 2 tokens

cancel():
  adapter->cancel()       → 1 token

amend():
  adapter->amend()        → 1 token

query_instrument_rules(): → 0 tokens (cached; no try_acquire)
```

This means a `request_burst` of 2 is the minimum that allows a single placement to succeed.
A burst of 1 is consumed by `query_balances` before `adapter->place()` is reached, causing the
placement itself to be rejected at the balance-check stage.

### Rejection path

When `try_acquire` returns false, the gateway rejects the order locally before any venue I/O:

```
try_acquire() → false
  │
  ├─ order persisted as REJECTED with code RATE_LIMITED
  ├─ pending_action = None  (no venue contact; outcome is certain)
  └─ returned to caller as a durable, idempotent rejection
```

The rejection is persisted to the journal and participates in the normal idempotency ledger. An
exact retry of a rate-limited order replays the persisted rejection without consuming any tokens
or reaching the adapter.

### Synchronization with venue rate-limit responses

Binance returns authoritative `rateLimits` fields in every WebSocket API response. The adapter
calls `synchronize_rate_limiter(capacity, available, tokens_per_second)` after each response,
which delegates to `TokenBucket::synchronize`. This reconfigures the TAT cell to reflect the
venue's current view of the budget:

```cpp
void synchronize(double capacity, double available, double tokens_per_second) noexcept {
    // Clamps available to [0, capacity], recomputes TAT from the new available balance.
    configure(capacity, std::clamp(available, 0.0, capacity), tokens_per_second);
}
```

This means a gateway that was locally exhausted is immediately unblocked when the venue reports
full capacity — without waiting for the local refill timer to expire. OKX does not return
per-response weight counters; its bucket refills by elapsed time only.

### Configuration

`SimulatedExchangeAdapter::Config` exposes `request_burst` and `requests_per_second`. The live
adapters use venue-documented weight limits. The simulated adapter accepts arbitrary values for
testing, including `throw_on_place` to inject adapter exceptions independently of rate limiting.

### Test coverage

| Test | What it verifies |
|---|---|
| `token bucket enforces burst and synchronized availability` | GCRA admit/deny, `synchronize` semantics |
| `token bucket CAS admits no more than capacity under contention` | 8-thread CAS correctness, no over-admission |
| `rate-limited place is persisted as REJECTED and never reaches the adapter` | Gateway rejection path, adapter isolation |
| `rate-limited rejection is idempotently replayed on retry` | Idempotency of persisted rate-limit rejections |
| `synchronize restores token bucket capacity and allows subsequent placement` | `synchronize()` unit semantics |
| `Binance rateLimits synchronize restores gateway placement after exhaustion` | End-to-end resync via `synchronize_rate_limiter()` |
| `adapter exception on place produces UNKNOWN outcome and reconciliation flag` | `throw_on_place` → `ADAPTER_EXCEPTION` → `Unknown`/`Reconcile` |

---

## Threading model

### Thread inventory

`OrderGateway` owns or interacts with the following threads at runtime:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ Thread                    Owner / Origin          Role                      │
├─────────────────────────────────────────────────────────────────────────────┤
│ Caller thread(s)          REST workers / CLI      place / cancel / amend    │
│ OKX I/O thread            OkxAdapter              WS read + REST calls      │
│ Binance I/O thread        BinanceAdapter          WS read/write             │
│ OKX SPSC lane worker      SpscExecutionLane[0]    apply_execution (OKX)     │
│ Binance SPSC lane worker  SpscExecutionLane[1]    apply_execution (Binance) │
│ OperationalEventWriter    OperationalEventWriter  journal audit append      │
│ ReconciliationWorker      OrderGateway            periodic reconciliation   │
│ Asio io_context threads   HttpServer (×N)         REST/WS request handling  │
│ File watcher thread       HttpServer              inotify UI asset reload   │
│ Market-data ring poller   GatewayRuntime          mmap ring tail + publish  │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Data flow between threads

```
Caller thread
  │  place() / cancel() / amend()
  │  ├─ acquire persistence_mutex_ + mutex_
  │  ├─ mutate order map, positions
  │  ├─ persist_order() [journal append / fdatasync]
  │  ├─ release both locks
  │  ├─ notify_order_observers()  ← lock-free atomic load
  │  └─ record_event() → OperationalEventWriter queue

OKX/Binance I/O thread
  │  execution callback fires
  └─ SpscExecutionLane::submit()  ← lock-free ring push + semaphore release

SPSC lane worker
  │  semaphore acquire → try_pop()
  └─ apply_execution()
       ├─ acquire persistence_mutex_ + mutex_
       ├─ OrderStateMachine::apply()
       ├─ adjust positions
       ├─ persist_order()
       └─ release locks → notify_order_observers() → record_event()

OperationalEventWriter jthread
  │  condition_variable wait → dequeue event
  ├─ IOrderStore::append_event()
  └─ complete_operational_event()
       ├─ acquire operational_mutex_  [deque push + observer copy]
       ├─ release operational_mutex_
       └─ fetch_add atomic counters  ← outside lock
          fire operational observers

ReconciliationWorker jthread
  │  condition_variable_any wait (reconciliation_mutex_)
  └─ reconcile(venue)
       ├─ adapter->query_open_orders()  [network, no lock held]
       └─ apply_execution() per report  [same path as SPSC worker]

Asio io_context threads
  └─ GatewayApi::handle_*()
       ├─ gateway_.place() / cancel() / amend()  [caller-thread path above]
       └─ gateway_.list_snapshots() / health()   [acquire mutex_ briefly]
```

### Lifetime and shutdown ordering

```
OrderGateway::stop()
  1. stop_reconciliation_worker()   — request_stop + notify + join
  2. adapter->stop() for each venue — stops I/O threads, closes sockets
  3. lane->flush() for each lane    — drains SPSC ring, joins lane worker
  4. record_event(GATEWAY_STOPPED)  — submits final audit record
  5. operational_event_writer_->flush() — drains writer queue, joins jthread

HttpServer::~HttpServer
  1. gateway_.flush_events()        — drains OperationalEventWriter before
  2. stop()                         —   impl_ is destroyed
       ├─ asio::post(Listener::stop) — dispatched into io_context strand
       ├─ io_context.stop()
       ├─ join io_context threads
       └─ stop_file_watcher()        — writes wake byte to pipe, joins thread
```

Shutdown ordering is critical. The SPSC lane workers must be joined before the
`OperationalEventWriter` jthread, because lane workers call `record_event` which submits to the
writer queue. The writer must be flushed before `impl_` is destroyed because the completion
callback holds a reference to `operational_mutex_` and the observer list.

---

## Synchronization design

### Synchronization principle

The rule is simple: **atomics own flags and counters; locks own data structures.**
Anything that is a single integer or boolean is `atomic`. A mutex is only justified
when multiple fields must change together consistently.

### Single mutex_ — order state gate

All mutable order state is guarded by a single `mutable std::mutex mutex_`.
Journal writes and audit event submission happen **outside** the lock:

```cpp
// Inside mutex_: state mutation + sequence reservation (one atomic fetch_add)
auto pp = prepare_persist(order, intent_only);   // returns {intent_only, seq}
// mutex_ released
commit_persist(order, pp.first, pp.second);      // serialize + write(), no lock
notify_order_observers(order);                   // atomic load, no lock
record_event2(...);                              // one OperationalEventWriter lock
```

`prepare_persist` reserves a sequence number under `mutex_` with a single
`atomic::fetch_add`. `commit_persist` serializes the snapshot and calls
`commit_order`, which performs a lock-free `write()` syscall (`O_APPEND` makes
it kernel-atomic for records under `PIPE_BUF`). Sequence ordering is correct
because the number was reserved before any concurrent mutation.

```
┌──────────────────────┬──────────────────────────────────────────────────────┐
│ Lock / Atomic        │ What it protects                                     │
├──────────────────────┼──────────────────────────────────────────────────────┤
│ mutex_               │ orders_, exchange_id_index_,                         │
│                      │ exchange_client_id_index_,                           │
│                      │ conservative_positions_, deferred_amend_reports_,    │
│                      │ active_operations_, sequence_trackers_,              │
│                      │ health_last_error_                                   │
├──────────────────────┼──────────────────────────────────────────────────────┤
│ AtomicVenueHealth    │ connected, ever_connected, reconciliation_required,  │
│ (atomic fields)      │ sequence_gaps, dropped_events — no lock needed       │
├──────────────────────┼──────────────────────────────────────────────────────┤
│ order_observer_mutex_│ Writer gate for copy-on-write observer list swap     │
│                      │ (add/remove only; notify reads atomically, no lock)  │
├──────────────────────┼──────────────────────────────────────────────────────┤
│ operational_mutex_   │ operational_events_ deque, operational_observers_,   │
│                      │ logging_failures_, last_logging_error_               │
├──────────────────────┼──────────────────────────────────────────────────────┤
│ FileOrderStore       │ index_mutex_: latest_orders_, recent_events_,        │
│ index_mutex_         │ recent_order_events_ — cold read paths only          │
├──────────────────────┼──────────────────────────────────────────────────────┤
│ reconciliation_mutex_│ reconciliation_worker_ jthread lifecycle +           │
│                      │ condition_variable_any wait/notify                   │
└──────────────────────┴──────────────────────────────────────────────────────┘
```

### Lock acquisition order and hold durations

No lock nesting occurs in the gateway. `mutex_` is never held while acquiring
any other lock. The store's `index_mutex_` is independent and never nested with
`mutex_`.

```
place() / cancel() / amend()  ← caller thread or Asio worker
│
├─ [mutex_] acquired
│    hash map lookups, idempotency check, state read          ~10–50 ns
│  [mutex_] released
│
├─ instrument_rules / balance queries  ← outside all locks, network I/O
│
├─ [mutex_] acquired
│    order map insert/update, position arithmetic             ~50–140 ns
│    prepare_persist() → atomic fetch_add (sequence reserve)  ~5 ns
│  [mutex_] released
│
├─ commit_persist()  ← outside all locks
│    JsonSerializer::write_order()                            ~7,500 ns
│    commit_order() → write() syscall, O_APPEND, no lock      ~3,700 ns
│
├─ notify_order_observers()  ← outside all locks
│    atomic load of observer shared_ptr                       ~2 ns
│    observer callbacks fired (no lock held)
│
└─ record_event2()  ← outside all locks
     build two OperationalEvent structs
     [OperationalEventWriter::mutex_] — one acquisition for both events  ~20 ns

apply_execution()  ← SPSC lane worker thread
│  [mutex_]  (same pattern as above)
│  commit_persist() → notify_order_observers() → record_event()
└─ atomic counters updated outside [operational_mutex_]

receive_execution() drop path  ← adapter I/O thread (lane full)
│  ++health_[venue].dropped_events          ← atomic, no lock
│  health_[venue].reconciliation_required   ← atomic store, no lock
└─ [mutex_] acquired only for health_last_error_ string write

connection_changed()  ← adapter I/O thread
│  state.connected / ever_connected / reconciliation_required ← atomic, no lock
└─ [mutex_] acquired only for last_error string read/write

complete_operational_event()  ← OperationalEventWriter jthread
│  [operational_mutex_] acquired
│    deque push, observer list copy                           ~20 ns
│  [operational_mutex_] released
└─ fetch_add(idempotent_replays_ / reconciliations_ / alerts_)  ← no lock
   fire operational observers  ← no lock
```

### Copy-on-write order observer list

`order_observers_` is an `atomic<shared_ptr<const vector<pair<token, fn>>>>`.

- **Read path** (`notify_order_observers`): single `load(acquire)`, iterate the snapshot. No lock,
  no allocation. An observer calling `remove_order_observer` during iteration is safe — it stores
  a new list atomically; the current iteration holds its own `shared_ptr` to the old list and
  completes normally.
- **Write path** (`add_order_observer`, `remove_order_observer`): load → copy → mutate → store
  under `order_observer_mutex_`. The mutex serializes concurrent writers against each other; it is
  never held across observer calls.
- **Token allocation**: `next_observer_token_` is `atomic<ObserverToken>`, incremented with
  `fetch_add(relaxed)` before the mutex is acquired, so token generation does not extend the
  critical section.

### Atomic counters outside operational_mutex_

`idempotent_replays_`, `reconciliations_`, and `alerts_` are `atomic<uint64_t>`. They are
incremented with `fetch_add(relaxed)` in `complete_operational_event` **after** `operational_mutex_`
is released, and read with `load(relaxed)` in `stability()` **before** `operational_mutex_` is
acquired. This removes three counter increments from the lock's critical section.

`logging_failures_` and `last_logging_error_` remain under `operational_mutex_` because they are
always written together (a string and a counter at the same two sites); making the counter atomic
would not eliminate the lock from those write sites.

---

## Lock-free inventory

| Component | Mechanism | Notes |
|---|---|---|
| Market data reads | Seqlock on 4 `alignas(64)` `QuoteSlot` structs with `atomic` fields | 6 ns, zero mutex |
| Execution ingestion | `SpscExecutionLane`: lock-free ring, `counting_semaphore` for parking | One `atomic_flag` CAS + slot write + `semaphore::release` per submit |
| Token bucket admission | Single 64-bit CAS (GCRA) | `static_assert` that `atomic<int64_t>` is always lock-free |
| Order observer reads | `atomic<shared_ptr<const vector>>` load | Notify path: one acquire load, no mutex |
| Observer token generation | `atomic<ObserverToken>` `fetch_add` | Outside `order_observer_mutex_` |
| Operational counters | `atomic<uint64_t>` `fetch_add` / `load` | Outside `operational_mutex_` |
| Venue health flags/counters | `atomic<bool>` / `atomic<uint64_t>` in `AtomicVenueHealth` | `connected`, `reconciliation_required`, `sequence_gaps`, `dropped_events` — no lock on hot drop/connect paths |
| Market data observer list | `atomic<shared_ptr<const ObserverEntries>>` | `MarketDataBook` — same COW pattern |
| Reconciliation scheduling | `atomic<uint8_t>` bitmask `fetch_or` | Venue bits set without holding `reconciliation_mutex_` |
| Gateway started flag | `atomic<bool>` | Guards start/stop idempotency |

---

## Synchronization trade-offs and pros/cons

### Single mutex_ (order state gate)

**Pro**: One lock for all order state. No acquisition ordering rules, no nesting, no
deadlock risk between gateway mutexes. Journal writes and audit event submission happen
outside the lock, so the critical section covers only in-memory mutation (~50–140 ns).
The two-phase persist design (`prepare_persist` inside lock, `commit_persist` outside)
preserves WAL ordering without holding the lock across I/O.

**Con**: All callers (place, cancel, amend, apply_execution, list, get) still serialize
on one lock. Under high concurrent REST load, contention grows linearly with thread count.
Sharding by symbol remains the natural next step if profiling shows contention.

### AtomicVenueHealth (flags and counters)

**Pro**: `connected`, `ever_connected`, `reconciliation_required`, `sequence_gaps`, and
`dropped_events` are `atomic`. The `receive_execution` drop path and `connection_changed`
touch only atomics — no lock acquired on those hot paths. The TSAN hang that previously
occurred when the drop path acquired `mutex_` while the lane worker held it is eliminated
by design: there is no shared lock between those two threads for health updates.

**Con**: `last_error` is a `std::string` and cannot be atomic. It is kept in a separate
`health_last_error_` map under `lock_.state`, written only on rare error paths.

### lock_.state (order map)

**Pro**: Simple, correct, covers all multi-field order invariants atomically. Hold time is
~50–140 ns — in the noise at any throughput this gateway targets.

**Con**: All callers (place, cancel, amend, apply_execution, list, get) serialize on one
lock. Under high concurrent REST load, contention would grow linearly with thread count.

**Why not sharded**: The current single-account OMS has two venues and a handful of concurrent
callers. Sharding by symbol is the natural next step if profiling shows contention, but
adds complexity (cross-shard position reads, reconciliation) not justified without measurement.

### Two-phase persist (WAL ordering without holding the lock across I/O)

**Pro**: Journal records appear in mutation order because the sequence number is reserved
under `mutex_` before any concurrent mutation. Serialization and `write()` happen outside
the lock, so `mutex_` hold time is ~50–140 ns regardless of journal mode. `commit_order`
is lock-free on the hot path — `O_APPEND` + `write()` is kernel-atomic for records under
`PIPE_BUF`.

**Con**: Two lock acquisitions per operation on the uncontended path (gateway `mutex_` +
store `index_mutex_` for cold reads). In practice `index_mutex_` is not acquired on the
hot path — `commit_order` holds no lock.

**Why not async WAL**: An async WAL (group commit, io_uring) would reduce per-order durable
latency but requires a more complex recovery protocol. The current design is correct and
auditable; async WAL is listed as a future measurement-led stage.

### order_observer_mutex_ (copy-on-write writer gate)

**Pro**: The hot path (`notify_order_observers`) is completely lock-free — one atomic load. The
mutex is only held during add/remove, which are cold-path operations (server startup/shutdown).

**Con**: Each add/remove allocates a new `vector` (copy-on-write). For a system with a fixed small
number of observers (typically 1–2: the WebSocket broadcaster and the CLI), this is negligible.

**Why not RCU**: C++20 has no standard RCU. The `atomic<shared_ptr>` COW pattern achieves the
same read-side guarantee with standard library primitives and is TSAN-verified.

### operational_mutex_ (audit deque)

**Pro**: Protects the bounded `deque<OperationalEvent>` and the operational observer map, both of
which require consistent multi-field access.

**Con**: `complete_operational_event` is called from the `OperationalEventWriter` jthread for
every audit record. The lock hold time is short (~20 ns for deque push + observer copy), but it
is on the audit path for every order event.

**Improvement applied**: The three numeric counters (`idempotent_replays_`, `reconciliations_`,
`alerts_`) were moved to `atomic<uint64_t>` and their increments moved outside the lock, reducing
the critical section.

**Why not SPSC ring**: The `OperationalEventWriter` already serializes writes through its own
jthread. Replacing `operational_mutex_` with a second SPSC ring would eliminate the lock from the
completion callback but add ring-management complexity. The current hold time does not justify it.

### reconciliation_mutex_ (background worker)

**Pro**: Standard `condition_variable_any` + `stop_token` pattern for a background jthread.
Completely off the order hot path.

**Con**: None material. The reconciliation worker runs at most once per 30 seconds per venue.

### SpscExecutionLane (lock-free ring)

**Pro**: Zero mutex in the submission path. The adapter I/O thread does one `atomic_flag` CAS,
one slot write, and one `semaphore::release`. The lane worker does one `semaphore::acquire` and
one slot read. `alignas(64)` on head and tail prevents false sharing.

**Con**: The `counting_semaphore` parks the consumer when the ring is empty, which adds a
syscall on the first item after an idle period. For a continuously active venue stream this is
irrelevant; for a low-rate simulation it adds ~1–5 µs wake latency.

**Why not busy-spin**: Busy-spinning the consumer would waste a core for no latency benefit at
the throughput levels this gateway targets. The semaphore provides prompt wakeup without
continuous CPU burn.

---

## Performance engineering

### Implemented hot-path optimizations

- Domain enums expose static `string_view` names and parse ASCII case-insensitively without
  allocating an uppercase copy.
- Owning string maps use transparent hashing; request/report views perform lookups without
  constructing a temporary `std::string`.
- Risk checks calculate a lightweight conservative position while the gateway lock is held. They
  no longer deep-copy every `Order` including its event and idempotency sets.
- Market quotes are partitioned by venue and use transparent symbol lookup. A read no longer
  builds and hashes a temporary `VENUE:SYMBOL` key.
- Execution ingestion uses one fixed-capacity preallocated SPSC ring per venue. Ring operations
  are atomic; counting semaphores park an idle consumer or full producer.
- The journal reuses its exclusively locked descriptor, serializes each payload once, and builds
  the record around those bytes. Latest orders and bounded event indexes are cached in memory.
  `fdatasync` remains enabled when durable writes are configured.
- The gateway `mutex_` is released before journal write/sync. WAL ordering is preserved by
  reserving the sequence number under the lock before releasing it (two-phase persist).
- `FileOrderStore::commit_order` is lock-free on the hot path: `O_APPEND` + `write()` is
  kernel-atomic for records under `PIPE_BUF`; `latest_orders_` is not updated on the hot path
  (it is built once at construction and read only at startup).
- `record_event2` batches two consecutive audit events under a single `OperationalEventWriter`
  mutex acquisition, halving mutex round-trips on the place/cancel/amend happy paths.
- Market data uses four fixed seqlock slots; the ring reader writes into a reusable span;
  conservative positions are maintained incrementally per symbol.
- The token limiter is a lock-free integer GCRA; request admission performs one 64-bit CAS.
- The synchronous HTTP transport keeps a four-handle connection pool. OKX REST and public market
  requests can reuse DNS/TCP/TLS state.
- Binance query signing iterates the JSON object's existing canonical key order rather than copying
  every key/value into a second tree. OKX ISO-8601 timestamps use a fixed stack buffer rather than
  an allocating stream formatter.
- `notify_order_observers` was changed from mutex-guarded vector copy to a single atomic load of
  the copy-on-write observer list, eliminating `order_observer_mutex_` from the notification path.
- `idempotent_replays_`, `reconciliations_`, and `alerts_` were changed to `atomic<uint64_t>` and
  their increments moved outside `operational_mutex_`, reducing the audit lock's critical section.
- Venue health flags and counters (`connected`, `ever_connected`, `reconciliation_required`,
  `sequence_gaps`, `dropped_events`) were moved to `AtomicVenueHealth` with `atomic` fields,
  eliminating all lock acquisition from the `receive_execution` drop path and `connection_changed`.
- The two order-state mutexes were consolidated into a single `mutex_`. The WAL ordering gate
  (`persistence_mutex_`) was replaced by the two-phase persist design, which reserves the sequence
  number atomically inside `mutex_` and writes outside it.

`std::string_view` is deliberately not stored in `Order`, `ExecutionReport`, adapter configuration,
HTTP work, or asynchronous queues. Those values cross call/thread boundaries and require ownership.

### Next measurement-led stages

1. Add production histograms for ingress queue delay, gateway-lock wait/hold time, journal append
   and sync time, adapter acknowledgement latency, and end-to-end order state latency.
2. Evaluate group commit or an `io_uring` journal only if production histograms show disk
   throughput is limiting; preserve intent durability and mutation-to-WAL ordering.
3. Implement the crash-safe checkpoint/segment design (see [Journal compaction design](#journal-compaction-design))
   after its fault-injection matrix is automated.
4. Replace JSON on the internal execution path with a typed/binary representation while retaining
   JSON at REST and venue boundaries.
5. Public top-of-book ingestion supports both one-second REST polling (default) and venue
   WebSocket streams (`okxPublicWebSocketUrl` / `binancePublicWebSocketUrl` in config).
   WebSocket mode reduces quote age from ~1 s to ~1–5 ms. Benchmark end-to-end freshness
   separately from order-routing latency if quote age becomes a risk-sizing concern.
6. Shard `mutex_` by symbol if profiling shows contention under concurrent REST load.

Use `std::chrono::steady_clock` for elapsed-time measurement. Do not use raw `RDTSC` for business
or venue timestamps; it is a cycle counter, not UTC, and requires architecture-specific
serialization, core migration handling, and calibration.

---

## Benchmark results

Build and run the standalone harness:

```bash
cmake --preset release -DABEX_BUILD_BENCHMARKS=ON
cmake --build --preset release --target abex_benchmark
./build-release/abex_benchmark
```

Results from the development host (medians, non-durable memory store unless noted):

| Workload | p99 | Mechanism |
|---|---:|---|
| Latest market quote lookup | 44 ns | Lock-free seqlock |
| Order state-machine report | 140 ns | Allocation-free when no new event ID |
| Decimal caller-buffer formatting | 90 ns | No returned string allocation |
| Two-venue SPSC execution lanes | 699 ns/event | Correct producer topology |
| Simulated end-to-end place (non-durable) | 88,527 ns/order | Two-phase persist; commit_order lock-free |
| Journal append (non-durable) | 20,284 ns | O_APPEND write(), no gateway lock held |
| Journal append (durable, `fdatasync`) | 95,006 ns | Filesystem-dependent (100-sample noise floor) |
| gateway_concurrent_4t (4 threads) | 482,705 ns | mutex_ contention; p50 improved -15–25% vs prior design |

The dominant cost in durable mode is `fdatasync` (~1.8 ms), not any mutex. Setting
`durableWrites=false` drops end-to-end place from ~1.8 ms to ~22 µs. The gateway `mutex_`
hold time is ~50–140 ns — in the noise at every throughput level this gateway targets.
See [docs/BENCHMARKING.md](BENCHMARKING.md) for the full table and before/after analysis.

These are microbenchmarks on a development host, not exchange round-trip promises. Pin and
load-isolate the process before using the numbers for capacity planning.

---

## Journal compaction design

The active journal grows indefinitely. An in-place rewrite can destroy both the old and new
recovery point if the host loses power between truncation, write, and metadata persistence.
Compaction therefore uses immutable checkpoints, append-only segments, and an atomically replaced
manifest.

### Recovery layout

```
state/
  orders.manifest                 active checkpoint and segment set
  orders.checkpoint.<sequence>    latest Order for every clientOrderId
  orders.segment.<first>.<last>   records after the checkpoint barrier
  orders.active                   current O_APPEND segment
  archive/                        optional immutable audit retention
```

Every checkpoint and segment retains the existing schema version, record sequence, payload
checksum, and strict mid-file corruption policy. Record sequences never restart after compaction.

### Compaction transaction

```
1. Acquire persistence_mutex_. Flush both SPSC execution lanes and the
   OperationalEventWriter queue.
2. Capture barrier sequence N and the latest complete Order snapshot for
   every clientOrderId.
3. Write orders.checkpoint.N.tmp with header, snapshots, operational-event
   retention window, and a footer containing record count and whole-file checksum.
4. fdatasync() the temporary checkpoint, rename to orders.checkpoint.N,
   then fsync() the containing directory.
5. Rotate orders.active to an immutable segment and open a new O_APPEND
   active file while the process still owns the exclusive journal lock.
6. Write orders.manifest.tmp naming the checkpoint, retained segments, next
   record sequence, and active segment. fdatasync() it, atomically rename to
   orders.manifest, and fsync() the directory again.
7. Release persistence_mutex_. Old files are now unreachable from the committed
   manifest and may be moved to archive/; delete only after the configured
   audit-retention period.
```

The maximum stop-the-world portion can later be reduced with copy-on-write snapshots, but the
initial implementation should favor an auditable barrier over cleverness.

### Startup and crash rules

- A `.tmp` file is never a recovery source and may be removed after startup validation.
- If no valid manifest exists, recovery falls back to the current single JSONL journal.
- If the manifest is valid, recovery loads the checkpoint and then only segments whose sequence is
  greater than `N`, rejecting gaps, overlap, or a sequence regression.
- A torn final record in the active segment is repaired exactly as today. Corruption in a
  checkpoint or immutable segment is fatal.
- The old manifest and files remain a valid recovery set until the new manifest rename and
  directory sync both complete.

### Retention policy

Compaction is not audit deletion. Order snapshots are retained indefinitely unless business policy
says otherwise. Operational events may use a time/count window in the online checkpoint, while old
immutable segments are compressed and archived for the regulatory retention period.

### Required fault-injection tests before enabling automatic compaction

Test process termination after each numbered transaction step, manifest checksum failure,
checkpoint checksum failure, missing/overlapping segments, a torn active append, disk-full
behavior, restart during archive movement, and concurrent read APIs during the barrier. Automatic
compaction must stay disabled until every crash point recovers either the complete old set or the
complete new set.

---

## OOAD and SOLID review

- **Single responsibility**: transitions, risk, persistence, venue protocols, transports, REST
  routing, socket hosting, and UI rendering are separate units.
- **Open/closed**: another adapter or store implements a port; the order state machine does not
  change.
- **Liskov substitution**: simulation and live adapters obey the same outcome/uncertainty contract.
- **Interface segregation**: exchange and store ports expose only operations used by the
  application.
- **Dependency inversion**: `OrderGateway` depends on `IExchangeAdapter` and `IOrderStore`; the
  composition root selects concrete implementations.

The design uses runtime polymorphism only at infrastructure boundaries and ordinary value
types/static protocol functions inside the deterministic core.

The two virtual ports are `IExchangeAdapter` and `IOrderStore`. A CRTP alternative would force
`OrderGateway` to become a template over a fixed adapter set or introduce a `variant`
visitor/type-erasure layer, making runtime configuration, heterogeneous ownership, isolated test
doubles, and future dynamically selected venues more cumbersome. The virtual call happens once at
a network/persistence boundary where JSON, TLS, system calls, and exchange latency dominate by
orders of magnitude. Protocol translators are already static and non-virtual, placing compile-time
dispatch where it is useful.

### `[[nodiscard]]` policy

`[[nodiscard]]` is applied to operation outcomes, parse/conversion results, risk decisions,
transport sends, persistence loads, state-machine results, getters returning snapshots, and
resource status. Intentional ignores are explicit with `(void)`. It is not applied to
side-effect-only lifecycle methods such as `start`, `stop`, `append`, `restore`, `flush_events`,
and observer removal.

---

## Authentication and key-management boundary

Authentication and key management are explicitly out of scope for this exercise (see
`project_requirements.txt`: "You do not need to implement authentication or key management
securely"). The current boundary: venue secrets are accepted only through environment variables
and are never included in configuration, persistence, API responses, or logs. The HTTP server
binds to `127.0.0.1` by default.

---

## Known limitations and scaling path

### Journal

The local journal grows indefinitely. The compaction design is fully specified in
[Journal compaction design](#journal-compaction-design) but not yet implemented. Automatic
compaction must stay disabled until the complete fault-injection matrix (torn write at each
transaction step, checksum failure, missing/overlapping segments, disk-full, concurrent reads
during barrier) passes. Until then, operators should rotate the journal file manually during a
planned maintenance window: stop the server, copy `state/orders.jsonl` to an archive path, and
restart — recovery loads the full file on startup regardless of size.

### Binance cancel-replace atomicity

Binance `order.cancelReplace` is not transactional. If the cancel leg succeeds and the replace
leg fails, the canonical order becomes canceled. A fill racing the compound request can make the
actual total filled quantity exceed the requested target. ABEX handles this by:

- Reporting the authoritative quantity from the venue's execution report, not the requested
  quantity.
- Emitting a `REPLACEMENT_QUANTITY_DRIFT` audit event when the discrepancy is detected.
- Persisting the generation offset so that a late fill on the canceled generation is correctly
  attributed and does not double-count.

The alternative — using `order.amend.keepPriority` for quantity-only reductions — would avoid
the atomicity gap for that subset of amends but cannot handle price changes or increases. Using
cancel-replace consistently gives deterministic cross-venue semantics at the cost of queue
priority and the atomicity gap described above.

### Market data freshness

Public top-of-book data can be consumed either via one-second REST polling (default) or via
venue WebSocket streams. The mode is selected by the presence of `marketData.okxPublicWebSocketUrl`
and `marketData.binancePublicWebSocketUrl` in the configuration:

- **REST mode** (default, no WS URLs configured): quotes are polled once per second via concurrent
  `std::async` fetches. Quote age can reach up to ~1 second plus network RTT before the
  five-second staleness threshold triggers.
- **WebSocket mode** (both WS URLs configured): `abex_market_data` runs two `ReconnectingWebSocket`
  instances — one subscribing to the OKX `tickers` channel, one to the Binance combined
  `bookTicker` stream. Each parsed quote is written to the ring immediately on arrival, reducing
  quote age from ~1 s to ~1–5 ms. Reconnection, backoff, and subscription replay are handled
  by `ReconnectingWebSocket` identically to the private order streams.

In both modes the ring-buffer design, the `abex_server` ring reader, and all downstream
consumers are unchanged. The five-second maximum age and the staleness block on MARKET orders
apply in both modes.

### Position model approximation

`conservative_positions_` is a `StringMap<Decimal>` protected by `mutex_`. It is intentionally
approximate: it nets buys against sells and takes a full-fill view of every open order. It does
not account for partial fills on concurrent orders or for orders placed by other processes on the
same account. The venue balance preflight is authoritative at query time but cannot atomically
reserve funds; the venue remains final authority under concurrent account activity.

Making `positions()` lock-free would require:

1. Exposing raw `int64_t` atomic operations on `Decimal` (it is an `int64_t` wrapper scaled to
   eight decimal places, so `fetch_add(delta.raw())` is arithmetically correct).
2. Adding a parallel `StringMap<atomic<int64_t>>` alongside `conservative_positions_`, pre-populated
   at construction for all configured symbols to guarantee structural stability (no insertions
   after startup).
3. Rewriting `adjust_position_locked`, `rebuild_indexes_locked`, and `positions()` to maintain
   both maps consistently.

This is a non-trivial redesign. The correct prerequisite is a metrics exporter (see Observability
limitation below) that shows `mutex_` contention on position reads under production load. Without
that measurement, the redesign is speculative. The `BENCHMARKING.md` baseline captures the
current p99 (37 µs under synthetic 4-reader / 200-writer contention) as the reference point for
any future profiling comparison.

### API synchrony

The REST API is synchronous around bounded venue acknowledgements. A caller blocks until the
adapter returns an outcome or an acknowledgement timeout fires. For a single-account OMS with
two venues this is acceptable. At higher concurrency, an accepted-operation resource with
asynchronous completion (polling or WebSocket push) would decouple client latency from venue RTT.

### Observability

There is no metrics exporter (Prometheus, StatsD, OpenTelemetry). Operational events and health
state are available at `/api/v1/system` and `/api/v1/health`, and streamed as `system.event`
WebSocket messages. Adding a metrics exporter requires implementing an `IOperationalObserver`
that translates event types to counter/gauge increments and exposes a `/metrics` endpoint — no
changes to the core are needed.

### Scaling path

1. **Shard `mutex_` by symbol** — each shard is an independent `orders_` map and position table.
   Contention drops proportionally with no protocol changes. Cross-shard position reads require
   a brief scan across shards, which is acceptable for the risk pipeline.
2. **Group commit on `persistence_mutex_`** — batch multiple WAL records into one `fdatasync`.
   Throughput scales with batch size; per-order durability guarantee is preserved by flushing
   before acknowledging the batch.
3. **Async WAL with `io_uring`** — reduces per-order durable latency from ~1.8 ms to ~50–200 µs
   at the cost of a more complex recovery protocol (completion ordering, partial-batch crash
   recovery). Implement only after the compaction design is stable.
4. **Replace `OperationalEventWriter` mutex+deque with a SPSC ring** — eliminates the mutex from
   the `record_event` submission path. `record_event2` already halves the round-trips on hot
   paths; a SPSC ring is not justified until journal throughput is measured as a bottleneck,
   which requires the metrics exporter first.
5. **Binary execution path** — replace JSON serialization on the internal execution path with a
   typed/binary representation (e.g. FlatBuffers or a hand-rolled fixed layout) while retaining
   JSON at REST and venue boundaries. Reduces per-event allocation and parse cost.
6. **WebSocket market-data ingestion** — implemented. Configure `marketData.okxPublicWebSocketUrl`
   and `marketData.binancePublicWebSocketUrl` to switch `abex_market_data` from REST polling to
   live venue WebSocket streams. Quote age drops from ~1 s to ~1–5 ms.
7. **Multi-account / multi-journal** — partition the journal by account ID and shard the gateway
   by account. Each account owns its own `persistence_mutex_`, position table, and adapter
   connections. No cross-account state is shared.
