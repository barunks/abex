# ABEX Architecture

This document describes the current ABEX exchange-gateway architecture: its process boundaries,
order lifecycle, concurrency model, persistence guarantees, recovery behavior, and operational
trade-offs. It is an implementation guide, not a catalogue of historical designs.

For build and operating instructions, see [README.md](../README.md). For measurement methodology
and current results, see [BENCHMARKING.md](BENCHMARKING.md).

## 1. Purpose and design goals

ABEX presents one order-management model over OKX and Binance Spot. The design prioritizes:

- deterministic order-state transitions;
- a journaled intent before external venue mutation;
- idempotent client retries;
- explicit venue-specific mapping at adapter boundaries;
- bounded queues and visible backpressure;
- low-contention reads for market data and operational state;
- safe simulation without exchange credentials; and
- independently testable domain, application, infrastructure, and interface layers.

ABEX is a take-home/reference gateway, not a complete production trading platform. It does not
provide multi-node consensus, an HA journal, smart order routing, portfolio margin, or a full
account-wide trade ledger.

## 2. System context

```text
CLI / REST / browser UI
          │
          ▼
  OrderGateway application service
    │       │          │
    │       │          └──► observers ──► WebSocket clients
    │       └──► FileOrderStore (checksummed JSONL journal)
    └──► VenueAdapter (OKX or Binance; live or simulated)
                 │
                 ▼
         per-venue SPSC execution lane
                 │
                 ▼
        order state + positions + audit state

abex_market_data ──► public venue feeds ──► mmap quote ring
                                              │
                                              ▼
                              risk checks / REST / WebSocket / UI
```

The browser server can supervise `abex_market_data` as a child process. The CLI and market-data
publisher can also run independently for service-manager or terminal-based operation.

## 3. Layering and dependency direction

Dependencies point inward:

```text
interfaces ──► infrastructure ──► application ──► domain
```

| Layer | Responsibility | Representative code |
|---|---|---|
| Domain | Orders, states, transitions, values, validation | `include/abex/domain` |
| Application | Use cases, risk, orchestration, ports | `include/abex/application` |
| Infrastructure | Journaling, venue transports, mmap, configuration | `include/abex/infrastructure`, `include/abex/bootstrap` |
| Interfaces | CLI, HTTP, WebSocket, browser assets | `src/cli`, `src/server`, `web` |

Domain code has no HTTP, filesystem, JSON, exchange-SDK, or UI dependency. Application code talks
to interfaces for storage, venues, clocks, and market data. Infrastructure implements those ports;
entry points assemble the object graph.

## 4. Runtime topology

### `abex_server`

The server owns the HTTP/WebSocket interface and a `GatewayRuntime`. Unless `--no-market-data` is
specified, it starts the market-data child, waits for an initial quote for up to 30 seconds, and
then serves the UI and API. Failure to obtain the first quote is visible during startup rather than
silently permitting a stale-data trading session.

### `abex_cli`

The CLI constructs the same gateway runtime without the browser interface. It supports an
interactive session and a single-command mode, making it useful for scripts and direct inspection.

### `abex_market_data`

The publisher consumes public OKX and Binance market data and writes normalized snapshots to the
mmap ring. When both public WebSocket URLs are configured it uses streaming feeds; otherwise it
falls back to REST polling. This process uses public endpoints and does not require trading keys.

## 5. Composition and configuration

`GatewayRuntime` is the composition root for the order path. Construction proceeds in dependency
order:

1. load and validate JSON or YAML configuration;
2. load environment-only credentials for live mode;
3. open and recover the journal;
4. open the market-data ring and quote feed;
5. construct risk services and live or simulated adapters;
6. construct `OrderGateway`; and
7. start feed, execution, reconciliation, and interface workers.

Configuration is non-secret and belongs in `config/gateway.example.json`. API keys, signing secrets,
and the OKX passphrase are resolved from environment-variable names and must not be committed.
Simulation mode deliberately replaces authenticated transports with deterministic simulated adapters.

## 6. Canonical order model

Clients and application services use one venue-neutral schema containing:

- `clientOrderId` for client ownership and placement idempotency;
- `requestId` for amend/cancel idempotency;
- venue, symbol, side, type, time-in-force, price, and quantity;
- cumulative fill quantity and average fill price;
- venue order identifier when acknowledged; and
- current state, timestamps, rejection detail, and journal sequence.

Prices and quantities use fixed-point decimal values. Floating-point arithmetic is excluded from
order identity, validation, and risk decisions.

Venue adapters are the only components allowed to translate this model into exchange-specific
field names, signatures, enums, timestamp formats, and response/error codes. This keeps exchange
details out of the state machine and client APIs.

## 7. Order command pipeline

Placement follows this order:

```text
validate request
  └─► resolve idempotency
       └─► read current market/account inputs
            └─► risk and rate-limit checks
                 └─► create pending order state
                      └─► append complete journal record
                           └─► route to venue adapter
                                └─► ingest acknowledgement/execution report
                                     └─► validate transition
                                          └─► persist and notify observers
```

Amend and cancel follow the same principles: validate ownership and state, resolve `requestId`,
journal the intended mutation, then perform venue I/O. A repeated identical request returns the
recorded result. Reusing an idempotency identifier with different content is rejected.

No order-state mutex is held across network I/O. State is revalidated when an asynchronous result
returns because fills, cancels, rejects, and amendments can race.

## 8. State machine and race policy

All changes pass through the domain transition table. Representative progression is:

```text
PENDING_NEW ──► NEW ──► PARTIALLY_FILLED ──► FILLED
     │            │              │
     └─► REJECTED └─► CANCELED ◄─┘
```

Pending amend/cancel states are used where the venue operation is asynchronous. Terminal states
cannot return to a live state. Cumulative fill quantity is monotonic and cannot exceed order
quantity. Duplicate or stale execution reports are ignored or reconciled without regressing state.

The policy for common races is deterministic:

- fill before cancel acknowledgement: apply the fill first, then accept only a still-valid cancel;
- complete fill during cancel: `FILLED` wins and the later cancel cannot reopen the order;
- duplicate execution: deduplicate using venue identifiers and cumulative quantities;
- late acknowledgement: correlate it to the existing client order and revalidate the transition;
- conflicting retry: reject it rather than issuing a second external mutation.

## 9. Risk and preflight

Risk is evaluated before journaled venue submission. Checks include schema and precision validation,
market-data freshness, venue health, order notional and quantity limits, open-order exposure, local
rate limits, and available balance where the live account path supplies it.

Market orders use a conservative price derived from the current quote for preflight. Passing local
risk does not guarantee venue acceptance: balances, rules, and prices can change between the check
and the exchange request. Venue rejections are normalized and persisted as order outcomes.

## 10. Persistence contract

### Journal format

`FileOrderStore` uses an append-only JSONL journal. Every line is a complete serialized record with
a sequence and checksum. Writes use an exclusive process lock and an internal write mutex, so
concurrent command producers cannot interleave record bytes.

The key ordering guarantee is:

> A complete journal record is appended before ABEX initiates the corresponding venue I/O.

With `journal.durableWrites=true`, appends notify a dedicated sync worker. The worker coalesces
generations and calls `fdatasync` in the background. Command callers do not wait for each sync.
Shutdown drains the latest requested generation before closing the journal.

This is append-before-route with bounded asynchronous syncing, not stable-storage-before-route.
A machine or process failure in the outstanding sync window can lose recent records that were
already routed. This trade-off deliberately removes storage flush latency from the order command
path. Deployments requiring strict stable-storage-before-route need synchronous commit or an
equivalent replicated WAL and must accept its latency/availability cost.

Setting `durableWrites=false` disables background `fdatasync`; it is intended for benchmarks and
controlled development, not as the documented operational default.

### Recovery

On startup the store:

1. acquires exclusive ownership of the journal;
2. reads records sequentially;
3. validates framing, checksum, and sequence;
4. retains the latest complete state for each owned order and request;
5. ignores or repairs an incomplete final append; and
6. rebuilds in-memory idempotency, order, and audit indexes.

Corruption in the middle of the journal is not treated like a torn tail and must be surfaced. The
journal defines the local ownership boundary: reconciliation must not import unrelated account-wide
orders as if ABEX had created them.

### Compaction

The journal is append-only during normal operation. Productionizing long-lived deployments requires
snapshot/compaction with atomic replacement, directory syncing, retained audit evidence, and a
crash-safe fallback to the previous generation. Compaction must never run concurrently with an
uncoordinated writer.

## 11. Execution ingestion and backpressure

Each venue has a bounded single-producer/single-consumer execution lane. Adapter callbacks enqueue
normalized execution reports; the gateway consumer applies them to state. Separate lanes prevent a
busy or reconnecting venue from creating head-of-line blocking for the other venue.

Bounded queues make overload explicit. Queue depth, drops, reconnects, sequence gaps, retries, and
rate-limit events are exported through system diagnostics. Overflow is an operational fault to
alert and reconcile, not permission to silently discard state-changing reports.

Local throttling uses per-venue rate-limit state and operation-specific costs. A venue `429` or
equivalent response updates the local backoff view. Throttling reduces avoidable failures but does
not replace venue-side enforcement.

## 12. Concurrency model

| Execution context | Ownership |
|---|---|
| Caller/HTTP threads | Validate commands, resolve idempotency, prepare state, append journal, invoke adapter |
| Venue transport threads | Parse responses/streams and enqueue normalized reports |
| Per-venue execution consumers | Apply reports and publish state changes |
| Reconciliation worker | Query live venue state and repair owned-order divergence |
| Journal sync worker | Coalesce generations and perform background `fdatasync` |
| Market-data process/threads | Consume public feeds and publish mmap snapshots |
| WebSocket sessions | Deliver bounded client updates |

Synchronization is selected by access pattern:

- a primary state mutex protects compound order, idempotency, and position invariants;
- a separate journal mutex serializes complete append operations;
- SPSC rings connect one producer and one consumer without a shared queue mutex;
- seqlock-style snapshots serve read-heavy quote and health data;
- observer lists use copy-on-write publication so callbacks run without a registration lock; and
- counters use atomics where no multi-field invariant is required.

The lock order is fixed by implementation. Blocking venue calls, observer callbacks, JSON encoding,
and filesystem syncing do not run while the main state lock is held. Any new path acquiring more
than one lock must preserve the established order and include a concurrency test.

## 13. Market-data design

The publisher normalizes each venue quote into a fixed-layout record in a memory-mapped ring. A
sequence protocol lets readers detect a concurrent write and retry without a process-shared mutex.
The gateway selects fresh input for risk and presentation; stale or unavailable data fails closed
for commands that require a reliable price.

The mmap boundary keeps public-feed parsing and reconnect behavior out of the order process. It also
allows the CLI, server, and diagnostic tools to read the same snapshot without duplicate exchange
subscriptions.

## 14. Client interfaces

REST exposes health, system diagnostics, orders, positions, quotes, and place/amend/cancel commands.
WebSocket sessions publish order, execution, position, market-data, and system events. The CLI calls
the same application service rather than implementing a second order path.

Interface handlers validate and translate transport data but do not own business rules. Slow
WebSocket clients have bounded output queues; they cannot block execution ingestion indefinitely.
Authentication and TLS termination are expected at a trusted reverse proxy for deployment—the
built-in server is a local/demo interface and must not be exposed directly to an untrusted network.

## 15. Reconciliation and restart behavior

Recovery reconstructs local intent and state; reconciliation compares only ABEX-owned live orders
with venue truth. It runs after startup and periodically after transport recovery. Differences are
applied through the same state-transition and persistence path as streaming reports.

The system is designed for at-least-once observation and idempotent mutation, not exactly-once
network delivery. Client and request identifiers make retries safe across ambiguous transport
failures. Exchange support for client identifiers is used wherever available, but operators must
still investigate unresolved commands and execution sequence gaps.

## 16. Startup and shutdown

Startup orders dependencies from persistence outward: journal recovery, market data, adapters,
gateway workers, then client interfaces. Shutdown reverses that relationship:

1. stop accepting new client work;
2. stop market-data and venue producers;
3. drain execution consumers and reconciliation work;
4. stop gateway workers and observer delivery;
5. drain the journal sync generation; and
6. close storage and transport resources.

The supervised server forwards termination to its market-data child, waits up to five seconds, and
uses a forced kill only if graceful termination fails. `SIGINT` and `SIGTERM` are the supported
operator signals.

## 17. Reliability and observability

Operational diagnostics include journal sequence/sync state, transport health, retries, reconnects,
rate-limit events, queue pressure, execution gaps, reconciliation outcomes, process restarts, and
logging failures. Health means more than an accepting TCP socket: dependencies and stale market
data are reported separately.

Structured logs must exclude credentials and signing material. Order identifiers and request
identifiers provide correlation across API, journal, adapter, execution, and reconciliation events.

## 18. Security boundary

- Secrets are environment-only and never stored in configuration or the journal.
- Simulation is the safe default for demonstrations and CI.
- Live mode should use venue keys restricted to required trading permissions, without withdrawal.
- The journal and environment file require restrictive filesystem permissions.
- A production deployment needs authenticated APIs, TLS, origin policy, request-size limits,
  network controls, log redaction, and secret rotation outside this repository.

## 19. Performance model

The design keeps read-heavy operations cheap through fixed-point values, mmap snapshots, seqlocks,
atomics, and pre-serialized or deferred JSON where appropriate. Writes intentionally cross stronger
boundaries: state validation, complete journal append, venue routing, and asynchronous report
application.

Latency must be evaluated by stage. Nanosecond quote reads do not imply nanosecond order placement,
and simulated placement does not predict Internet exchange latency. The latest reproducible results,
sample sizes, environment, and durability semantics are in [BENCHMARKING.md](BENCHMARKING.md).

## 20. Known limitations and production roadmap

The most important extensions for a production deployment are:

1. authenticated TLS API ingress and authorization;
2. a strict durable or replicated WAL option for stable-storage-before-route deployments;
3. crash-safe journal snapshotting, compaction, archival, and restore tooling;
4. stronger venue-specific sequence recovery and dead-letter handling;
5. dedicated load, soak, chaos, and disconnect/reconnect qualification;
6. metrics export with capacity alerts and defined SLOs;
7. supervised multi-process packaging and secret-management integration; and
8. multi-instance ownership/leader fencing if horizontal availability is required.

These limitations do not invalidate the current single-instance reference design; they define the
boundary between a robust take-home implementation and an operated production trading service.

## 21. Verification map

| Architectural property | Primary verification |
|---|---|
| State transitions and race policy | Domain/state-machine tests and execution-race tests |
| Idempotent place/amend/cancel | Retry and conflicting-payload tests |
| Complete append before venue I/O | Journal ordering and fault-injection tests |
| Torn-tail recovery | Restart and tail-repair tests |
| Exclusive journal ownership | Store-lock tests |
| Queue behavior and venue isolation | SPSC, burst, and two-venue tests |
| Market-data consistency | mmap/seqlock and freshness tests |
| HTTP and WebSocket contracts | Server/interface tests |
| Concurrency safety | ThreadSanitizer build and contention tests |
| Memory and undefined behavior | AddressSanitizer/UBSan builds |
| Performance characteristics | Release harness described in `BENCHMARKING.md` |

The normal submission gate is the full 127-test suite. Sanitizer and benchmark invocations are
documented in [README.md](../README.md).
