# Requirements audit

Audit date: 2026-08-19. Source of truth: `project_requirements.txt`.

## Executive result

| Requirement group | Result | Primary evidence |
|---|---|---|
| One exchange-neutral REST API | Complete | `GatewayApi`, OpenAPI document, strict common-field validation |
| Common OKX/Binance order schema | Complete | `OrderRequest`, REST schema, shared JSON views |
| Venue-specific translation | Complete | `OkxProtocol`, `BinanceProtocol`, isolated live adapters |
| Unified response/state model | Complete | `Order`, `ExecutionReport`, `OrderStateMachine` |
| Failures, retries, restart recovery | Complete for exercise scope | durable intents/audit events, idempotency ledger, unknown outcomes, startup reconciliation |
| Risk controls | Complete | size, notional, and conservative position checks before routing |
| REST, WebSocket, CLI, UI | Complete | `abex_server`, `/ws/v1/orders`, `abex_cli`, `web/` |
| Mapped market-data pipeline | Complete | standalone publisher, mmap ring, gateway reader, REST/WebSocket/UI |
| Runtime live/simulation mode | Complete | `--mode live|simulation`; live default; no C++ conditional compilation |
| Required tests and examples | Complete | 55 tests, CLI demo, REST/curl demo |
| Secure auth/key management | Intentionally out of scope | environment-only venue keys; loopback server default; production guidance below |
| Ultra-low-latency optimization | Intentionally out of scope | correctness-first journal, JSON, clear runtime ports |

The implementation satisfies the objective in deterministic simulation and contains both live
venue implementations. Live authenticated end-to-end tests are deliberately not automated because
they depend on mutable external accounts and would place/cancel real sandbox orders.

## Step-by-step audit

### 1. Client-facing interface

- `POST /api/v1/orders` accepts exactly `clientOrderId`, `venue`, `symbol`, `side`, `type`,
  `price`, `quantity`, and `timeInForce`.
- The same request reaches either venue based only on `venue`. Unknown properties are rejected;
  clients cannot pass `tdMode`, Binance method names, or other exchange-specific controls.
- List/get, amend, cancel, positions, health, reconciliation, and OpenAPI routes use one versioned
  `/api/v1` namespace.
- API responses and WebSocket events use the same `order_view`, so transport changes cannot change
  domain state.
- The REST router is transport-independent and tested directly. Boost.Beast supplies the bounded,
  asynchronous network boundary.
- `GET /api/v1/system` and the browser identify REST command/snapshot traffic, client WebSocket
  updates, mmap market ingress, journal durability, and current process stability separately.

Result: complete. Tests `REST API accepts one common schema for both venues` and `REST API rejects
exchange-specific fields` prove the principal contract.

### 2. Exchange-facing isolation and translation

- `IExchangeAdapter` is the application port. `OrderGateway` has no OKX/Binance JSON, signing,
  endpoint, or socket logic.
- OKX uses REST for place/cancel/amend/query and its authenticated private WebSocket `orders`
  channel for updates.
- Binance uses signed WebSocket API methods `order.place`, `order.cancel`, `order.cancelReplace`,
  and `order.status`, plus the user-data execution stream.
- `OkxProtocol` maps canonical symbols, order types, time-in-force, IDs, quantities, and statuses.
  `BinanceProtocol` performs its distinct mappings. Both produce `AdapterResult` and
  `ExecutionReport` rather than exposing venue JSON upstream.
- Protocol mapping and normalization have focused unit tests independent of network availability.

Result: complete for the requested order subset.

Transport selection is not exposed to clients because lines 54–61 prescribe it per venue. The UI
selects only `venue`; browser commands use gateway REST and browser updates use the gateway client
WebSocket. The CLI is an in-process administrative presentation adapter over the same application
service.

### 3. Supported order flow

- Market and limit creation are supported on both adapters.
- Cancel is a common operation. Amend supports price and/or total quantity.
- Binance uses cancel-replace while OKX uses native amend. A Binance canonical order preserves every
  physical exchange-order generation with fill/quote offsets; late fills from the canceled
  generation contribute to totals without canceling the active replacement.
- An OKX amend ACK is request acceptance only. Requested terms remain pending until the private
  order stream or an authoritative query reports the new `px`/`sz`.
- Public states are `LIVE`, `PARTIALLY_FILLED`, `FILLED`, `CANCELED`, and `REJECTED`. `UNKNOWN` plus
  `pendingAction` models operational uncertainty without inventing a venue result.

Result: complete.

### 4. Unified order management and determinism

- Durable orders contain stable client IDs, current/historical exchange IDs, replacement aliases,
  generation fill offsets, and pending amend targets.
- A create fingerprint makes `clientOrderId` idempotent. Identical retries replay; different payloads
  return `IDEMPOTENCY_CONFLICT`.
- Amend/cancel use durable `requestId` fingerprints and persist original success/failure/unknown
  outcomes, preventing a retry from causing a second venue action.
- The new-order intent is appended before exchange I/O, so a WebSocket execution arriving before an
  acknowledgement remains correlatable.
- Processed event IDs suppress duplicates. Cumulative fills never decrease. Stale sequence reports
  can contribute a previously unknown fill but cannot regress lifecycle state. Full fill wins a
  cancel/fill race.
- Fixed-point `Decimal` avoids binary floating-point drift in monetary values.

Result: complete. Gateway race/idempotency/replacement tests and the 20,000-transition randomized invariant
test cover these rules.

### 5. Gateway-side risk

- Schema and positive-value checks run first.
- Maximum quantity and notional are configured per canonical instrument.
- Current venue metadata is checked before order I/O: trading status, minimum/maximum quantity,
  quantity increments, price range/tick, and applicable minimum/maximum notional.
- Market notional uses the fresh mapped venue ask for buys and bid for sells. It fails closed when
  that quote is missing or stale.
- Position checks include realized fills plus worst-case remaining quantity on open orders.
- New orders query authoritative available balance before venue I/O. BUY checks quote currency at
  limit/current ask and SELL checks base currency; unavailable balance data fails closed and
  shortfalls are durably rejected with explicit amounts and account identity where exposed.
- The order ticket requests only the selected route's funding currency (USDT for BUY, BTC/ETH for
  SELL), combines balance and venue rules into a routeable quantity range with a 0.5% reserve, and
  disables routing with a corrective message when any preflight fails. The standalone all-currency
  UI panel and broad CLI query are intentionally disabled.
- Local rejections are persisted as normalized `REJECTED` orders and include machine-readable codes
  and human-readable reasons; they never reach an adapter.

Result: complete at the approximate position scope permitted by the requirements.

### 6. Market data and runtime mode

- `abex_market_data` runs as a separate process and publishes one-second BTC-USDT and ETH-USDT
  OKX/Binance top-of-book ticks into a fixed-capacity memory-mapped ring-buffer file.
- By default the primary `abex_server` supervises this separate publisher and waits for its first
  quote before declaring the complete OMS/UI setup ready. `--no-market-data` preserves independently
  managed process deployment.
- The gateway reads the mapped file, not the public venue endpoints. REST and WebSocket views expose
  the same in-memory snapshot and ring generation/sequence health.
- The UI displays per-venue bids/asks and best executable buy/sell prices. Its limit-price hint is
  seeded from the chosen venue and side.
- Simulation market orders fill at ask/bid; resting limits fill when later mapped ticks cross.
- `--mode live|simulation` dynamically selects adapters. Live is the parameter default in both
  executables and composition root. The source tree has zero C++ `#if`, `#ifdef`, or `#ifndef`
  directives; CMake conditions remain only for build targets, tests, compiler warnings, and
  sanitizers.

Result: complete.

### 7. Recovery and resilience

- Every mutation appends a complete order snapshot to a checksummed JSON-lines journal. Durable mode
  performs `fdatasync` before the mutation is acknowledged, and a retained file lock rejects a
  concurrent OMS writer.
- Typed operational records persist process start/restart, request intent/acknowledgement, retry,
  disconnect/reconnect, unknown outcome, backpressure, sequence-gap, and reconciliation evidence.
  The UI and `/api/v1/system` show the same durable history and logging-failure counters.
- Recovery selects the newest valid record for each client ID. A torn final append is ignored;
  earlier corruption is fatal rather than silently accepted.
- Startup restores aliases and venue state, starts private streams, enumerates venue open orders,
  ignores account orders outside the durable journal's ownership boundary, and queries journaled
  non-terminal orders absent from the open snapshot. Unrelated private-stream reports are ignored
  under the same rule.
- Reconciliation repeats after reconnect and every configured `reconciliationIntervalMs` (30 seconds
  by default), covering live feeds whose order channels do not expose contiguous sequence numbers.
- Disconnects, timeouts, adapter exceptions, missing updates, sequence gaps, and dropped ingestion
  events set reconciliation-required health. An uncertain operation becomes `UNKNOWN`; it is not
  falsely rejected or canceled.
- The reconnecting TLS WebSocket transport verifies the peer, uses bounded exponential backoff,
  serializes writes, and re-establishes subscriptions/authentication.

Result: complete with conservative behavior.

### 8. Stretch goals marked must-have in the document

- Sequence tracking and gap detection: implemented without pretending non-contiguous venue IDs are
  sequences.
- Rate-limit awareness: conservative per-adapter token buckets and clear local failures; Binance
  response `rateLimits` resynchronize capacity, consumption, and refill rate.
- Backpressure: bounded exchange-event ingestion plus a bounded client WebSocket queue. A slow UI
  gets `resync.required` and reloads REST state.
- WebSocket client endpoint: `/ws/v1/orders` sends initial order, market, and system snapshots plus
  normalized order, market, and operational updates.
- Property-style testing: deterministic randomized lifecycle test with 20,000 transitions.

Result: complete.

### 9. Deliverables and UI

- Source is structured by domain, application, ports, infrastructure, presentation/server,
  bootstrap, and executable composition roots.
- README documents build/run, schema, adapter differences, retries, recovery, security boundary,
  examples, and limitations.
- The 55-test suite covers values, risk, transitions, physical replacement generations, nested
  venue failures, protocol typing, races, idempotency, recovery,
  randomized invariants, mmap publication/consumption, quote-driven fills, REST, static UI hosting,
  and order/market/system WebSocket delivery.
- `examples/demo.sh` exercises the CLI. `examples/rest_demo.sh` gives runnable curl calls for new,
  amend, and cancel on both venues.
- The responsive UI supports new orders, amend/cancel, search/filter, normalized status, risk
  metrics, worst-case positions, venue health/reconciliation, reconnect, polling fallback, explicit
  transport boundaries, and a persistent restart/retry/alert timeline.

Result: complete.

## OOAD and SOLID review

- Single responsibility: transitions, risk, persistence, venue protocols, transports, REST routing,
  socket hosting, and UI rendering are separate units.
- Open/closed: another adapter or store implements a port; the order state machine does not change.
- Liskov substitution: simulation and live adapters obey the same outcome/uncertainty contract.
- Interface segregation: exchange and store ports expose only operations used by the application.
- Dependency inversion: `OrderGateway` depends on `IExchangeAdapter` and `IOrderStore`, while the
  composition root selects concrete implementations.

The design uses runtime polymorphism only at infrastructure boundaries and ordinary value
types/static protocol functions inside the deterministic core.

## `[[nodiscard]]` audit

The project can compile and run if `[[nodiscard]]` is removed. The attribute is not an optimization
and has no runtime cost; it asks the compiler to warn when a meaningful result is discarded.

The current uses are appropriate on operation outcomes, parse/conversion results, risk decisions,
transport sends, persistence loads, state-machine results, getters returning snapshots, and resource
status. Those return values commonly determine whether state is safe to advance. Intentional ignores
are explicit with `(void)`, making review easier. It is not applied to side-effect-only lifecycle
methods such as `start`, `stop`, `append`, `restore`, `flush_events`, and observer removal.

Decision: retain it. Removing it would not make the code faster and would weaken detection of ignored
errors such as a failed WebSocket enqueue or rejected adapter operation. A house style could remove
it from trivial scalar accessors, but that is cosmetic and provides no performance benefit.

## Virtual interfaces versus CRTP

The two virtual ports are `IExchangeAdapter` and `IOrderStore`. A CRTP alternative is technically
possible:

```cpp
template<class Derived>
class StaticExchangeAdapter {
public:
    Venue venue() const noexcept { return self().venue_impl(); }
    AdapterResult place(const Order& order) { return self().place_impl(order); }
private:
    Derived& self() noexcept { return static_cast<Derived&>(*this); }
};

using Adapter = std::variant<OkxAdapter, BinanceAdapter, SimulatedExchangeAdapter>;
// std::visit dispatches a common operation to the selected concrete adapter.
```

That would force `OrderGateway` to become a template over a fixed adapter set or introduce a
`variant` visitor/type-erasure layer. It would make runtime configuration, heterogeneous ownership,
isolated test doubles, and future dynamically selected venues more cumbersome. It also increases
template coupling and build/binary size.

The virtual call happens once at a network/persistence boundary where JSON, TLS, system calls, and
exchange latency dominate by orders of magnitude. It is neither the fill-state transition loop nor
an ultra-low-latency market-data path. The protocol translators are already static and non-virtual,
which places compile-time dispatch where it is useful.

Decision: keep the virtual ports. If profiling later finds an in-process, per-message hot adapter
boundary, use a hybrid: CRTP for that concrete codec/engine behind one stable type-erased port. A
full conversion is not justified by this exercise or its non-ultra-low-latency scope.

## Authentication and key-management boundary

Secure client authentication and secure venue-key management are explicitly not required, so they
were not invented as partial security. Venue secrets are accepted only through environment
variables and are never included in configuration, persistence, API responses, or logs. The supplied
document contains exposed-looking credentials; rotate them and never commit them elsewhere.

The unauthenticated HTTP server binds to `127.0.0.1` by default and should remain on a trusted host.
A production design would add TLS/mTLS or OAuth-backed service identity, per-account authorization,
idempotency ownership, request/audit identity, a vault/HSM or workload identity for venue secrets,
rotation, locked/redacted memory, IP allow-listing, and least-privilege trade keys.

## Remaining risks and accepted limitations

- Authenticated live end-to-end behavior is not CI-tested and may be affected by exchange API or
  account configuration changes.
- The local journal has no compaction and is a single-process store.
- Position risk remains approximate. Balance service data is authoritative at query time but is
  not an atomic venue reservation, so concurrent account activity can still race preflight.
- Public top-of-book data is one-second REST polling, not a full-depth low-latency venue feed.
- Binance cancel-replace partial success relies on updates/reconciliation to settle both identities.
- The API is synchronous around bounded venue acknowledgements; production scale may prefer an
  accepted-operation resource and asynchronous completion.
- The UI is an operator demonstration, not a terminal with entitlements and approvals.
