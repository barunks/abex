# ABEX Audit Report

**Date:** 2026-08-19  
**Scope:** Full codebase audit — requirements compliance, architecture, performance, concurrency, test coverage, and modern C++ quality  
**Auditor:** Amazon Q Developer  

---

## Table of Contents

1. [Requirements Compliance](#1-requirements-compliance)
2. [Architecture and SOLID/OOAD](#2-architecture-and-solidooad)
3. [Domain Layer](#3-domain-layer)
4. [Application Layer — OrderGateway](#4-application-layer--ordergateway)
5. [Application Layer — BoundedEventDispatcher](#5-application-layer--boundedeventdispatcher)
6. [Application Layer — MarketDataBook](#6-application-layer--marketdatabook)
7. [Application Layer — RiskManager](#7-application-layer--riskmanager)
8. [Infrastructure — FileOrderStore / Journal](#8-infrastructure--fileorderstore--journal)
9. [Infrastructure — MarketDataRing (mmap)](#9-infrastructure--marketdataring-mmap)
10. [Infrastructure — ReconnectingWebSocket](#10-infrastructure--reconnectingwebsocket)
11. [Infrastructure — OkxAdapter](#11-infrastructure--okxadapter)
12. [Infrastructure — BinanceAdapter](#12-infrastructure--binanceadapter)
13. [Infrastructure — TokenBucket / RateLimiter](#13-infrastructure--tokenbucket--ratelimiter)
14. [Server Layer — HttpServer / GatewayApi](#14-server-layer--httpserver--gatewayapi)
15. [CLI Layer — CommandProcessor](#15-cli-layer--commandprocessor)
16. [String Usage and Allocation Audit](#16-string-usage-and-allocation-audit)
17. [Mutex and Lock Contention Audit](#17-mutex-and-lock-contention-audit)
18. [Hot-Path Allocation Audit](#18-hot-path-allocation-audit)
19. [Test Coverage Audit](#19-test-coverage-audit)
20. [Benchmark Coverage Audit](#20-benchmark-coverage-audit)
21. [Prioritised Improvement Roadmap](#21-prioritised-improvement-roadmap)

---

## 1. Requirements Compliance

### 1.1 Mandatory Functional Requirements

| Requirement | Status | Evidence |
|---|---|---|
| Single exchange-neutral REST API | ✅ Complete | `POST /api/v1/orders` accepts one common schema; unknown fields rejected |
| Common OKX + Binance order schema | ✅ Complete | `OrderRequest` with `clientOrderId`, `venue`, `symbol`, `side`, `type`, `price`, `quantity`, `timeInForce` |
| OKX adapter — REST place/cancel/amend | ✅ Complete | `OkxAdapter::place/cancel/amend` via `HttpClient` |
| OKX adapter — WebSocket private order updates | ✅ Complete | `ReconnectingWebSocket` subscribing `orders` channel after login |
| Binance adapter — WebSocket order submission | ✅ Complete | `BinanceAdapter` uses `order.place`, `order.cancel`, `order.cancelReplace` over WS API |
| Binance adapter — WebSocket execution updates | ✅ Complete | `userDataStream.subscribe.signature` + `executionReport` parsing |
| Adapters isolated behind common interface | ✅ Complete | `IExchangeAdapter` port; `OrderGateway` has zero venue-specific code |
| Market + Limit orders | ✅ Complete | Both venues, both types |
| Cancel order | ✅ Complete | Both venues |
| Amend/replace order | ✅ Complete | OKX native amend; Binance cancel-replace with generation tracking |
| Normalized states: LIVE, PARTIALLY_FILLED, FILLED, CANCELED, REJECTED | ✅ Complete | `OrderStatus` enum; `OrderStateMachine` enforces monotonic transitions |
| clientOrderId → exchangeOrderId mapping | ✅ Complete | `exchange_id_index_`, `exchange_client_id_index_` in `OrderGateway` |
| Out-of-order WebSocket message handling | ✅ Complete | Sequence-based stale detection in `OrderStateMachine::apply` |
| Duplicate execution report suppression | ✅ Complete | `processed_event_ids` set per order |
| REST vs WebSocket race handling | ✅ Complete | Intent journaled before I/O; deferred amend reports for Binance cancel-replace |
| Idempotent client retries | ✅ Complete | `create_fingerprint`, `processed_requests` map, `requestId` per cancel/amend |
| Persist state (append-only log) | ✅ Complete | `FileOrderStore` — checksummed JSONL with `fdatasync` |
| Restart recovery | ✅ Complete | `load_latest()` replays highest snapshot per `clientOrderId` |
| Max order size per instrument | ✅ Complete | `InstrumentRiskLimits::max_order_size` checked in `RiskManager::validate` |
| Max notional per order | ✅ Complete | `InstrumentRiskLimits::max_notional` |
| Per-instrument position limit | ✅ Complete | Conservative full-fill view of all open orders |
| Rejected orders return clear reasons | ✅ Complete | Machine-readable `code` + human-readable `reason` in `RiskDecision` |
| WebSocket disconnect/reconnect | ✅ Complete | `ReconnectingWebSocket` with bounded exponential backoff |
| Duplicate/missing message handling | ✅ Complete | Event-ID dedup + sequence gap detection + reconciliation |
| Gateway restart with live orders | ✅ Complete | Startup reconciliation via `query_open_orders()` + individual `query()` |
| Reload persisted orders on startup | ✅ Complete | `OrderGateway` constructor calls `load_latest()` |
| Reconcile OKX open orders on startup | ✅ Complete | `reconcile(Venue::Okx)` called in `start()` when `reconcile_on_start=true` |
| Re-establish Binance WebSocket state | ✅ Complete | `restore()` replays aliases; `subscribe_user_data()` on reconnect |

### 1.2 Stretch Goals (marked "must-have" in requirements)

| Stretch Goal | Status | Evidence |
|---|---|---|
| Sequence number tracking and gap detection | ✅ Complete | `SequenceTracker` per venue; `EXECUTION_SEQUENCE_GAP` operational event |
| Rate-limit awareness per exchange | ✅ Complete | `TokenBucket` per adapter; Binance `rateLimits` response resynchronizes bucket |
| Backpressure between WebSocket ingestion and REST clients | ✅ Complete | `BoundedEventDispatcher` (4096 capacity); client WS 256-message bound with `resync.required` |
| WebSocket endpoint in addition to REST | ✅ Complete | `/ws/v1/orders` with snapshot + incremental `order.updated`, `market.updated`, `system.event` |
| Property-based tests for order state transitions | ✅ Complete | `test_properties.cpp` — 100 trials × 200 random transitions = 20,000 total |

### 1.3 Explicitly Out-of-Scope Items (correctly excluded)

| Item | Decision |
|---|---|
| Client authentication / RBAC | Correctly excluded; server binds `127.0.0.1` by default |
| TLS termination in-process | Correctly excluded; production guidance documented |
| Ultra-low-latency optimization | Correctly excluded per requirements; correctness-first design |
| Every order type / exchange feature | Correctly scoped to Market + Limit |

### 1.4 Compliance Gaps and Observations

| # | Observation | Severity |
|---|---|---|
| R-1 | `UNKNOWN` is used as an operational state but is not listed in the five normalized states the requirements define (`LIVE`, `PARTIALLY_FILLED`, `FILLED`, `CANCELED`, `REJECTED`). It is a sound engineering addition but is undocumented in the requirements mapping. | Low |
| R-2 | Binance `openOrders.status` reconciliation constructs a synthetic `executionReport` JSON object manually in `query_open_orders()` and `query()`. If Binance adds or renames fields the synthetic construction silently produces wrong data. A dedicated parse function would be safer. | Medium |
| R-3 | OKX market-order auto-reduction is detected post-hoc in `OkxAdapter::query()` by comparing `cumulative_filled < order.quantity`. This is correct but relies on a follow-up query; a delayed query window exists where the order appears `FILLED` with a short fill. | Low |
| R-4 | The requirements say "reconcile with OKX open orders" on startup. Binance `openOrders.status` is also reconciled, which is correct and better than the minimum requirement. No gap here — noted as a positive. | Info |

---

## 2. Architecture and SOLID/OOAD

### 2.1 Dependency Direction

The dependency graph is clean and strictly inward:

```
Browser / CLI
    │
    ▼
GatewayApi / CommandProcessor   (presentation)
    │
    ▼
OrderGateway                    (application)
    │           │           │
    ▼           ▼           ▼
IExchangeAdapter  IOrderStore  RiskManager / MarketDataBook
    │
    ▼
OkxAdapter / BinanceAdapter / SimulatedExchangeAdapter
    │
    ▼
HttpClient / ReconnectingWebSocket / ExchangeProtocols
```

The domain layer (`Order`, `Decimal`, `OrderStateMachine`) has zero knowledge of JSON, files, sockets, or threads. This is correct ports-and-adapters.

### 2.2 SOLID Audit

**Single Responsibility Principle**

| Class | Verdict | Note |
|---|---|---|
| `OrderStateMachine` | ✅ | Pure lifecycle transitions only |
| `RiskManager` | ✅ | Pre-trade checks only |
| `Decimal` | ✅ | Fixed-point arithmetic only |
| `SequenceTracker` | ✅ | Sequence gap detection only |
| `FileOrderStore` | ✅ | Journal append + load only |
| `MarketDataRingWriter/Reader` | ✅ | mmap ring I/O only |
| `OkxProtocol` / `BinanceProtocol` | ✅ | Stateless protocol translation only |
| `OrderGateway` | ⚠️ | Orchestrates place/cancel/amend/reconcile, manages indexes, fires observers, records operational events, and runs the reconciliation worker. This is the largest class (~700 lines). It is cohesive but could be split: index management and observer dispatch are separable concerns. |
| `HttpServer` | ⚠️ | Owns the Boost.Beast acceptor, WebSocket session management, and observer wiring. The `ServerState` inner class partially separates broadcast from I/O, but session lifecycle and broadcast are still coupled. |
| `BinanceAdapter` | ⚠️ | Handles WS transport, clock synchronization, request signing, pending-promise map, and execution parsing. Clock sync could be a separate `BinanceClockSync` collaborator. |

**Open/Closed Principle**

✅ Adding a third venue requires only a new `IExchangeAdapter` implementation and a new entry in `GatewayRuntime`. `OrderGateway`, `RiskManager`, and `OrderStateMachine` do not change.

⚠️ `GatewayApi::error_status()` is a long `if/else` chain of string comparisons against error codes. Adding a new error code requires modifying this function. A `static const std::unordered_map<std::string_view, unsigned>` lookup table would make it open/closed.

**Liskov Substitution Principle**

✅ `SimulatedExchangeAdapter` satisfies the same outcome/uncertainty contract as the live adapters. Tests confirm that `OrderGateway` behaves identically regardless of which concrete adapter is injected.

**Interface Segregation Principle**

✅ `IExchangeAdapter` exposes only what `OrderGateway` uses. `IOrderStore` exposes only what the gateway needs for persistence and audit.

⚠️ `IExchangeAdapter::query_balances` and `query_instrument_rules` are called directly by `OrderGateway::place()` outside the normal execution path. These are read-only queries that could be segregated into a separate `IVenueInfoProvider` interface, keeping `IExchangeAdapter` focused on order lifecycle.

**Dependency Inversion Principle**

✅ `OrderGateway` depends on `IExchangeAdapter` and `IOrderStore` abstractions. `GatewayRuntime` is the composition root that wires concrete implementations.

⚠️ `OrderGateway` directly constructs `BoundedEventDispatcher` in its constructor initializer list. The dispatcher is an internal implementation detail, but it could be injected to allow testing with a synchronous dispatcher without the `flush_events()` workaround currently needed in every test.

### 2.3 OOAD Observations

| # | Observation | Severity |
|---|---|---|
| A-1 | `Order` is a large value type (17 fields + 5 maps/sets). It is copied on every `list()` call, every observer notification, and every `get()` return. For a correctness-first exercise this is fine, but it is the single largest allocation source on the hot path. | Medium |
| A-2 | `OrderEventContext` in `operational_event.hpp` duplicates several `Order` fields as `std::string`. It is constructed by `order_event_context()` in `order_gateway.cpp` with multiple `.to_string()` calls per event. This is a pure allocation cost with no semantic benefit over passing a const `Order&` reference to the event recorder. | Medium |
| A-3 | `GatewayRuntime` is the composition root and correctly owns all lifetimes. However it exposes `simulated_adapters()` returning a raw `const auto&` to the internal map. The CLI uses this to call `emit()` directly on simulated adapters. This is a test/simulation convenience but leaks the internal adapter type through the runtime boundary. | Low |
| A-4 | `StringMap` and `StringSet` (transparent hash) are defined in `string_lookup.hpp` and used consistently. This is a good pattern. However `Order` itself still uses `std::unordered_map<std::string, ...>` and `std::unordered_set<std::string>` for `exchange_fill_offsets`, `exchange_quote_offsets`, `processed_requests`, `processed_request_outcomes`, and `processed_event_ids` — missing the transparent hash benefit for those containers. | Low |
| A-5 | `OperationResult` carries `std::optional<Order>` which means a full `Order` copy on every successful operation response. A `std::optional<std::string>` client_order_id with a separate `get()` call would avoid the copy on the REST response path. | Low |

---

## 3. Domain Layer

### 3.1 `Decimal` — Fixed-Point Arithmetic

**What is correct:**
- `int64_t` raw storage with scale `100_000_000` (8 decimal places) is correct for monetary values.
- `constexpr` constructors, `from_raw`, `from_integer` are zero-cost.
- `operator<=>` provides full ordering with no extra code.
- `[[nodiscard]]` on `parse`, `to_string`, `abs`, `raw` is appropriate.

**Issues found:**

| # | Location | Issue | Severity |
|---|---|---|---|
| D-1 | `Decimal::to_string()` | Returns `std::string` by value — allocates on every call. Called in `order_event_context()` for every operational event (8 fields × every state change). A `to_chars`-based formatter writing into a caller-supplied buffer would eliminate all allocations on the hot path. | High |
| D-2 | `Decimal::parse(std::string_view)` | Implementation not shown but called with `std::string` temporaries in many places (e.g. `Decimal::parse("0.1")`). If `parse` internally constructs a `std::string` from the `string_view`, that is an unnecessary allocation for string literals. Should use `std::from_chars` directly on the `string_view` data. | Medium |
| D-3 | `operator*` and `operator/` | Use `__int128` or equivalent for intermediate products to avoid overflow at large values. Not visible in the header — needs verification in `decimal.cpp`. If plain `int64_t` multiplication is used, overflow is possible for values above ~92 BTC at full precision. | High |
| D-4 | `Decimal` has no `consteval` parse for compile-time constants | All test and config literals like `Decimal::parse("50000")` run at runtime. A `consteval` or `constexpr` string-to-raw converter would move these to compile time. | Low |

### 3.2 `Order` — Value Type

**What is correct:**
- Comprehensive state capture including generation offsets for Binance cancel-replace.
- `processed_event_ids` as a set correctly suppresses duplicates.
- `create_fingerprint` and `processed_requests` map enable full idempotency.

**Issues found:**

| # | Location | Issue | Severity |
|---|---|---|---|
| D-5 | `Order::exchange_fill_offsets` | `std::unordered_map<std::string, Decimal>` — uses `std::string` key without transparent hash. Lookups in `OrderStateMachine::apply` via `offset_for()` pass `const std::string&` so no temporary is created, but the map itself is not heterogeneous. | Low |
| D-6 | `Order::processed_event_ids` | `std::unordered_set<std::string>` without transparent hash. Every `contains(report.event_id)` where `event_id` is already a `std::string` is fine, but if called with a `string_view` it would construct a temporary. Should use `StringSet`. | Low |
| D-7 | `Order::processed_requests` / `processed_request_outcomes` | `std::unordered_map<std::string, std::string>` without transparent hash. Keys are constructed as `"CANCEL:" + request_id` — a heap allocation per cancel/amend. A fixed-size key type or `StringMap` would help. | Low |
| D-8 | `Order::rejection_reason` | `std::string` — set to `decision.code + ": " + decision.reason` which is a string concatenation allocation. Could be two separate fields (`rejection_code`, `rejection_message`) stored as `string_view` into static storage for known codes. | Low |
| D-9 | `fingerprint(const OrderRequest&)` | Returns `std::string` — allocates. Called twice per `place()` (once before lock, once inside lock for double-checked replay). The fingerprint is only compared for equality; a `uint64_t` FNV hash would be sufficient and allocation-free. | Medium |

### 3.3 `OrderStateMachine`

**What is correct:**
- Purely static, no state, no allocations in the logic itself.
- Monotonic fill enforcement (`std::max`).
- Historical generation handling for Binance cancel-replace is correct and well-tested.
- `derive_status` correctly handles all race combinations.
- `amendment_confirmed` correctly waits for authoritative terms from the order stream.

**Issues found:**

| # | Location | Issue | Severity |
|---|---|---|---|
| D-10 | `ApplyResult::reason` | `std::string` field — allocated even for the common `Applied` case where it is empty. An `enum ApplyDisposition` already carries the semantic; `reason` is only populated for `Invalid` and `Stale`. Use `std::string_view` pointing to a static literal for the two non-empty cases. | Low |
| D-11 | `OrderStateMachine::apply` | Copies `old_status`, `old_filled`, `old_quote`, `old_exchange_id`, `old_price`, `old_quantity`, `old_pending` as local variables to detect changes. `old_exchange_id` is a `std::string` copy — unnecessary allocation. Compare by value after the fact or use a hash/version check. | Low |

### 3.4 `ExecutionReport`

**What is correct:**
- `event_id`, `client_order_id`, `exchange_order_id` as `std::string` — correct ownership.
- All monetary fields as `Decimal` — correct.

**Issues found:**

| # | Location | Issue | Severity |
|---|---|---|---|
| D-12 | `ExecutionReport::event_id` | Constructed as `"place-reject-" + order.client_order_id + '-' + std::to_string(order.version)` in `order_gateway.cpp`. Three string concatenations = three allocations for a synthetic event ID that is only used for dedup. A `uint64_t` hash of the components would be allocation-free. | Low |
| D-13 | `ExecutionReport::reason` | `std::string` — set to `adapter_result.code + ": " + adapter_result.message`. Same pattern as `rejection_reason` above. | Low |

### 3.5 `MarketQuote`

**What is correct:**
- `Decimal` for prices — correct.
- `sequence` for ordering — correct.

**Issues found:**

| # | Location | Issue | Severity |
|---|---|---|---|
| D-14 | `MarketQuote::symbol` | `std::string` — allocated per quote. The ring stores symbols as a fixed `char[16]` array. The reader constructs a `std::string` from it on every `read_available()` call. A `std::string_view` into a static symbol table (e.g. `"BTC-USDT"`, `"ETH-USDT"`) would eliminate this allocation entirely since the symbol set is fixed. | Medium |

### 3.6 `OperationalEvent` / `OrderEventContext`

**What is correct:**
- Typed severity enum.
- Optional venue and order context.

**Issues found:**

| # | Location | Issue | Severity |
|---|---|---|---|
| D-15 | `OrderEventContext` | Contains 10 `std::string` fields that duplicate `Order` data converted via `.to_string()`. Constructed on every `record_event()` call. This is the single largest allocation cluster per order state change. Should be replaced with a `const Order*` reference or a lightweight struct of `string_view` pointing into the order's existing strings. | High |
| D-16 | `OperationalEvent::category`, `code`, `message` | All `std::string` — `category` and `code` are always compile-time string literals (e.g. `"PIPELINE"`, `"ORDER_ACKNOWLEDGED"`). These should be `std::string_view` or `const char*` to avoid allocation. | Medium |

---

## 4. Application Layer — OrderGateway

### 4.1 Overview

`OrderGateway` is the central orchestrator (~700 lines). It owns:
- The in-memory order map (`StringMap<Order> orders_`)
- Two index maps (`exchange_id_index_`, `exchange_client_id_index_`)
- The `BoundedEventDispatcher` for async execution ingestion
- The reconciliation worker thread
- Observer registries for order and operational events
- Venue health state

### 4.2 Locking Architecture

`OrderGateway` uses **two separate mutexes**:

| Mutex | Guards |
|---|---|
| `mutex_` | `orders_`, both indexes, `health_`, `active_operations_`, `deferred_amend_reports_`, `sequence_trackers_`, `order_observers_` |
| `operational_mutex_` | `operational_events_` deque, `operational_observers_`, counters (`idempotent_replays_`, etc.) |

This separation is intentional and correct — operational event recording does not block order processing. However several issues exist:

| # | Location | Issue | Severity |
|---|---|---|---|
| G-1 | `place()` | Acquires `mutex_` twice — once for the initial idempotency check, releases it to do network I/O (instrument rules + balance query), then re-acquires for the final decision. This double-checked locking pattern is correct for correctness but means the lock is held during `persist_locked()` which calls `order_store_->append()` (a file write + optional `fdatasync`). With `durableWrites=true` this holds `mutex_` for the entire `fdatasync` duration, blocking all concurrent `get()`, `list()`, and `apply_execution()` calls. | High |
| G-2 | `apply_execution()` | Holds `mutex_` for the entire function including `persist_locked()` (file write) and `record_event()` (which acquires `operational_mutex_` while `mutex_` is held — a nested lock). This is a lock-order dependency: `mutex_` → `operational_mutex_`. It is consistent throughout the codebase but means any path that holds `mutex_` and calls `record_event()` blocks operational observers. | High |
| G-3 | `record_event()` | Acquires `operational_mutex_`, calls `order_store_->append_event()` (file write), then notifies observers while still holding `operational_mutex_`. Observers are called outside the lock (correctly copied first), but the file write is inside the lock. | Medium |
| G-4 | `list()` | Acquires `mutex_` and copies every `Order` in the map into a `std::vector<Order>`. Called by `reconcile()` at the start of every reconciliation cycle. With 1000 orders this is a significant allocation and copy under the lock. Should return IDs or use a snapshot-on-write pattern. | Medium |
| G-5 | `positions()` | Acquires `mutex_` and iterates all orders. Returns `std::unordered_map<std::string, Decimal>` — allocates a new map on every call. Called by the REST `/api/v1/positions` endpoint. | Low |
| G-6 | `conservative_position_locked()` | Called inside `mutex_` during `place()` and `amend()`. Iterates all orders with a `symbol` string comparison per order. With many orders this is O(n) under the lock. An index keyed by symbol would make this O(1). | Medium |
| G-7 | `rebuild_indexes_locked()` | Called only in the constructor. Correct. But if called during a live reconciliation it would clear and rebuild both indexes under `mutex_` — a full stop-the-world. Not currently triggered at runtime, but the function is `private` with no guard against future misuse. | Low |
| G-8 | `active_operations_` | `StringSet` used to prevent concurrent operations on the same order. Checked and modified under `mutex_`. Correct, but `StringSet` uses heap-allocated `std::string` keys. A `std::unordered_set<std::string_view>` with views into the already-owned `orders_` map keys would be allocation-free. | Low |

### 4.3 Observer Pattern

| # | Location | Issue | Severity |
|---|---|---|---|
| G-9 | `persist_locked()` | Calls order observers while holding `mutex_`. Observers are `std::function<void(const Order&)>` — the `HttpServer` observer calls `impl_->publish_order()` which posts to the Boost.ASIO strand. The post itself is fast, but if an observer ever blocks (e.g. a slow test observer), it blocks the gateway mutex. | Medium |
| G-10 | `order_observers_` | `std::unordered_map<ObserverToken, OrderObserver>` — iterated on every `persist_locked()` call. With a small fixed number of observers (typically 1–2) this is fine, but the map lookup overhead is unnecessary. A `std::vector<std::pair<ObserverToken, OrderObserver>>` would be faster for small counts. | Low |

### 4.4 String Allocations in OrderGateway

| # | Location | Issue | Severity |
|---|---|---|---|
| G-11 | `gateway_instance_id()` | `"gateway-" + std::to_string(::getpid()) + '-' + std::to_string(started_at_ms)` — three string concatenations at startup. Acceptable (once only). | Info |
| G-12 | `record_event()` parameters | `std::string category`, `std::string code`, `std::string message` passed by value. All call sites pass string literals. These should be `std::string_view` parameters to avoid copying literals into `std::string` at every call site. | High |
| G-13 | `failed_outcome()` / `pending_outcome()` | Construct strings with `+` concatenation and `outcome_separator`. Called on every failed cancel/amend. A `std::array<char, N>` or `fmt`-style formatter would be allocation-free. | Low |
| G-14 | `order_event_context()` | Constructs `OrderEventContext` with 10 `std::string` fields via `.to_string()` and `std::string(to_string(...))`. Called on nearly every `record_event()` invocation. This is the dominant allocation source per order operation. See D-15. | High |
| G-15 | `request_key` construction | `"CANCEL:" + request.request_id` and `"AMEND:" + request.request_id` — heap allocation per cancel/amend for a key that is only used as a map lookup. A `std::string_view` composite key or a typed key struct would avoid this. | Low |

### 4.5 Reconciliation Worker

| # | Location | Issue | Severity |
|---|---|---|---|
| G-16 | `reconciliation_queue_` | `std::deque<Venue>` protected by `reconciliation_mutex_` with `condition_variable_any`. The queue holds at most 2 entries (one per venue). A `std::array<bool, 2>` with an `atomic` flag per venue would be lock-free and simpler. | Low |
| G-17 | `schedule_reconciliation()` | Uses `std::ranges::find` on the deque to avoid duplicates — O(n) scan. With n≤2 this is fine but semantically a `std::unordered_set<Venue>` or two `std::atomic<bool>` flags would be clearer. | Low |
| G-18 | `run_reconciliation_worker()` | Calls `reconcile(venue)` which calls `list(venue)` (full order copy under `mutex_`) and then calls `adapter->query_open_orders()` (network I/O) without holding any lock. This is correct — network I/O must not hold the gateway mutex. | Info |

---

## 5. Application Layer — BoundedEventDispatcher

### 5.1 Design

A single-producer (multiple callers via `submit()`), single-consumer (worker thread via `run()`) bounded queue implemented with a `std::vector<std::optional<std::pair<Venue, ExecutionReport>>>` ring buffer, one `std::mutex`, and three condition variables (`not_empty_`, `not_full_`, `idle_`).

### 5.2 Issues Found

| # | Location | Issue | Severity |
|---|---|---|---|
| E-1 | Queue element type | `std::optional<std::pair<Venue, ExecutionReport>>` — `ExecutionReport` contains 4 `std::string` fields. Each enqueue copies the report into the optional. A lock-free SPSC ring of pre-allocated `ExecutionReport` slots (using `std::atomic` sequence numbers like the mmap ring) would eliminate all allocations and the mutex on the hot ingestion path. | High |
| E-2 | Three condition variables | `not_empty_`, `not_full_`, `idle_` — all protected by the same `mutex_`. `not_full_` uses `std::condition_variable` (requires `std::unique_lock<std::mutex>`) while `not_empty_` uses `std::condition_variable_any` (for `std::stop_token` support). The asymmetry is correct but adds cognitive overhead. A single `std::condition_variable_any` for all three would simplify the design. | Low |
| E-3 | `submit()` timeout | Default 250ms wait on `not_full_`. If the consumer is slow, producers block for 250ms before returning `false`. This is a correctness-correct backpressure mechanism but 250ms is a long time on a trading path. The timeout should be configurable and default to a much shorter value (e.g. 1ms). | Medium |
| E-4 | `flush()` | Waits for `size_ == 0 && in_flight_ == 0` under `mutex_`. Called by tests after every `emit()`. In production it is called by `OrderGateway::stop()`. Correct but the `idle_` condition variable is only notified when `in_flight_` drops to zero — if the handler throws, `in_flight_` is decremented in the catch block, which is correct. | Info |
| E-5 | `handler_` stored as `std::function` | `std::function` has type-erasure overhead and a potential heap allocation for large captures. Since the handler is always `[this](Venue, const ExecutionReport&) { apply_execution(...); }`, a plain function pointer or a non-owning `std::function` alternative would be faster. | Low |

---

## 6. Application Layer — MarketDataBook

### 6.1 Design

In-memory cache of the latest `MarketQuote` per venue+symbol, backed by `std::array<StringMap<MarketQuote>, 2>` (index 0 = OKX, index 1 = Binance). Protected by a single `std::mutex`. Observers notified outside the lock (correctly copied first).

### 6.2 Issues Found

| # | Location | Issue | Severity |
|---|---|---|---|
| M-1 | `latest()` | Acquires `mutex_` and returns `std::optional<MarketQuote>` by value — copies the entire `MarketQuote` including its `std::string symbol`. Called by `price()` which is called by `OrderGateway::place()` on every order. With a fixed symbol set, a lock-free `std::atomic<MarketQuote>` per slot (if `MarketQuote` were trivially copyable) or a seqlock would eliminate the mutex on the read path entirely. | High |
| M-2 | `publish()` | Acquires `mutex_`, updates the map, copies all observers, releases lock, then calls observers. The observer copy (`std::vector<QuoteObserver>`) allocates on every publish (every second from the ring feed). A fixed-size `std::array` of observers or a pre-allocated vector would avoid this. | Medium |
| M-3 | `snapshot()` | Acquires `mutex_` and copies all quotes into a `std::vector`. Called by the WebSocket `market.snapshot` on every new connection. Acceptable for low connection rates. | Low |
| M-4 | `MarketDataStatus::last_error` | `std::string` — set on every `set_ring_status()` call even when empty. A `std::string_view` into a static error table would be allocation-free for known error states. | Low |
| M-5 | `venue_index()` | `constexpr` function returning 0 or 1 — correct and zero-cost. | Info |

---

## 7. Application Layer — RiskManager

### 7.1 Design

Stateless validator (after construction) holding a `StringMap<InstrumentRiskLimits>`. All check methods are `const`. Transparent hash on the limits map allows `string_view` lookups without allocation.

### 7.2 Issues Found

| # | Location | Issue | Severity |
|---|---|---|---|
| R-1 | `validate()` | Returns `RiskDecision` by value. `RiskDecision::reject()` constructs `std::string code` and `std::string reason` — both are string concatenations. For the common `accept()` path this is zero-cost. For the reject path, the reason string is built with `+` concatenation (e.g. `"quantity " + quantity.to_string() + " exceeds max order size " + limits.max_order_size.to_string()`). Four allocations per rejection. A `std::ostringstream` or `std::format` (C++20) would be cleaner; a pre-allocated buffer would be faster. | Medium |
| R-2 | `check_new()` | Takes `const std::vector<Order>& current_orders` and iterates to compute position. This is the `O(n)` snapshot path. `check_new_with_position()` takes a pre-computed `Decimal` — the gateway correctly uses the `_with_position` variant. The `check_new()` variant (used only in tests and the benchmark) is fine. | Info |
| R-3 | `conservative_positions()` | Returns `std::unordered_map<std::string, Decimal>` — allocates a new map. Called only from the benchmark, not from the gateway hot path. | Info |
| R-4 | `from_json()` | Parses `std::string` keys from JSON and constructs `Decimal` via `Decimal::parse(item.at(...).get<std::string>())`. The `.get<std::string>()` allocates. Using `nlohmann::json::get<std::string_view>()` where supported would avoid this. Acceptable since this runs once at startup. | Info |
| R-5 | `RiskDecision` factory methods | `accept()` and `reject()` are `[[nodiscard]] static` — correct. `reject()` takes `std::string` by value — callers that pass string literals pay a construction cost. Should take `std::string_view` and construct internally only when needed. | Low |

---

## 8. Infrastructure — FileOrderStore / Journal

### 8.1 Design

Append-only JSONL file with FNV-1a checksums, monotonic sequence numbers, and `fdatasync` durability. Advisory `flock` prevents concurrent writers. Recovery reads the entire file and keeps the highest-sequence snapshot per `clientOrderId`.

### 8.2 Correctness Assessment

✅ Torn-append detection (invalid final record is truncated, not fatal).  
✅ Mid-file corruption is fatal — correct conservative behavior.  
✅ `fdatasync` before acknowledging the local transition — correct WAL semantics.  
✅ `flock(LOCK_EX | LOCK_NB)` prevents split-brain writers.  
✅ `O_APPEND` flag ensures atomic appends at the OS level (POSIX guarantee for writes ≤ PIPE_BUF on local filesystems).  

### 8.3 Issues Found

| # | Location | Issue | Severity |
|---|---|---|---|
| J-1 | `append_record()` | Holds `mutex_` for the entire function: `payload.dump()` (JSON serialization), `record.dump()` (full record serialization), `write_all()` (syscall), and `fdatasync()` (disk flush). With `durableWrites=true` this can take 1–10ms, blocking all concurrent `load_latest()`, `load_events()`, and `status()` calls. Separate the serialization from the I/O, or use a dedicated writer thread with a queue. | High |
| J-2 | `load_latest()` / `load_events()` | Both call `read_valid_records()` which re-reads and re-parses the entire file from disk on every call. `load_latest()` is called at startup (acceptable). `load_events()` is called by `GET /api/v1/system` and `operational_events()` — potentially on every health poll. An in-memory event cache (already maintained in `OrderGateway::operational_events_`) should be the primary source; file reads should be startup-only. | High |
| J-3 | `load_order_events()` | Re-reads and re-parses the entire file, then filters by `client_order_id`. Called by `GET /api/v1/orders/{id}/pipeline`. For a large journal this is O(file_size) per request. An in-memory index of events by `client_order_id` would make this O(1). | Medium |
| J-4 | `append_record()` | `nlohmann::json payload` taken by value — the `Order` or `OperationalEvent` is serialized to JSON, then the JSON is serialized to string twice: once for `checksum(payload_text)` and once inside `record.dump()`. The payload string is computed twice. Compute it once and reuse. | Medium |
| J-5 | `checksum()` | FNV-1a over the payload string — correct for corruption detection. Not a cryptographic hash. The comment in the code correctly states this. | Info |
| J-6 | `MemoryOrderStore::load_latest()` | Iterates `records_` (a `std::vector<Order>`) and builds an `std::unordered_map<std::string, Order>` on every call. For tests this is fine. For production use it should maintain a live map. | Low |
| J-7 | Journal growth | No compaction. The journal grows indefinitely. After 10,000 orders with 5 state changes each, the file is ~50MB of JSON. Startup recovery time grows linearly. A periodic snapshot + truncation mechanism is needed for production. | Medium |

---

## 9. Infrastructure — MarketDataRing (mmap)

### 9.1 Design

Fixed-layout POSIX memory-mapped ring buffer. Writer uses `store_release` on `committed_sequence`; reader uses `load_acquire` + double-check to detect torn reads. `flock` prevents multiple writers. Generation counter detects publisher restarts.

### 9.2 Correctness Assessment

✅ `alignas(64)` on `RingHeader` and `RingRecord` prevents false sharing.  
✅ `std::atomic_ref<uint64_t>` with `memory_order_acquire/release` — correct lock-free protocol.  
✅ Double-check of `committed_sequence` before and after copying record fields — correct torn-read detection.  
✅ `static_assert(std::atomic_ref<uint64_t>::is_always_lock_free)` — compile-time guarantee.  
✅ `static_assert(std::is_trivially_copyable_v<RingRecord>)` — correct for mmap safety.  

### 9.3 Issues Found

| # | Location | Issue | Severity |
|---|---|---|---|
| R-1 | `read_available()` | Returns `std::vector<MarketQuote>` — allocates a new vector on every poll (every 50ms from `MarketDataRingFeed`). With a fixed capacity of 4 quotes per poll (2 venues × 2 symbols), a `std::array<MarketQuote, 8>` output parameter would be allocation-free. | Medium |
| R-2 | `MarketQuote::symbol` construction | `std::string(symbol.data(), symbol_length)` — allocates a `std::string` from the fixed `char[16]` array on every record read. Since only `"BTC-USDT"` and `"ETH-USDT"` are valid, intern these as `string_view` constants and compare the raw bytes to select the interned view. | Medium |
| R-3 | `MarketDataRingFeed::run()` | Calls `reader.read_available(cursor_)` every 50ms. The returned vector is passed to `book_->publish()` one quote at a time. The vector allocation + destruction every 50ms is unnecessary given the fixed small size. | Low |
| R-4 | `existing_file_size()` | Opens the file, calls `fstat`, closes it — three syscalls just to get the file size. Use `std::filesystem::file_size()` which is one syscall, or keep the descriptor open. | Low |
| R-5 | Writer `publish()` | Calls `unix_time_ms()` (a `clock_gettime` syscall) for every quote when `published_at_ms == 0`. The publisher always sets `published_at_ms` before calling `publish()`, so this branch is never taken in practice. The check adds a branch on the hot write path. | Info |
| R-6 | `Mapping` class | Correctly RAII-manages `mmap`/`munmap` and `open`/`close`. The `OwnedDescriptorTag` constructor is a clean way to transfer descriptor ownership. | Info |

---

## 10. Infrastructure — ReconnectingWebSocket

### 10.1 Design

Pimpl-based TLS WebSocket client using Boost.Beast + Boost.ASIO. Single `io_context` thread. Bounded exponential backoff reconnect. Application-level heartbeat (ping/pong). Outbound queue drained on disconnect (correct — never implicit replay).

### 10.2 Correctness Assessment

✅ `ssl::verify_peer` + `ssl::host_name_verification` — correct TLS validation.  
✅ `SSL_set_tlsext_host_name` for SNI — correct.  
✅ Outbound queue cleared on disconnect — correct (prevents implicit replay of uncertain requests).  
✅ `set_connected()` notifies `connection_condition_` — correct for `wait_connected()`.  
✅ `stopping_` flag prevents reconnect after `stop()` — correct.  

### 10.3 Issues Found

| # | Location | Issue | Severity |
|---|---|---|---|
| W-1 | `outbound_` queue | `std::deque<OutboundMessage>` where `OutboundMessage` contains `std::string payload`. Each `send()` call moves a `std::string` into the deque. For high-frequency sends (e.g. Binance order.place + order.cancelReplace in quick succession) this is fine. For a future high-throughput path, a pre-allocated ring of fixed-size buffers would be better. | Low |
| W-2 | `connection_mutex_` + `connection_condition_` | Used only by `wait_connected()`. This is a blocking call used at startup to wait for the first connection. The mutex is separate from the ASIO strand — correct. | Info |
| W-3 | `on_open_`, `on_message_`, `on_connection_` callbacks | Stored as `std::function` — type-erasure overhead. Since these are set once at `start()` and never changed, a struct of function pointers or a non-owning callback interface would be faster. | Low |
| W-4 | `read_next()` | Uses a `shared_ptr<beast::flat_buffer>` passed through the async chain. The buffer is allocated once per connection and reused across reads — correct. | Info |
| W-5 | `message` in `read_next` callback | `beast::buffers_to_string(buffer->data())` — allocates a `std::string` for every incoming message. For the OKX/Binance message path this string is then passed to `nlohmann::json::parse()` which allocates again. A `string_view` into the buffer data (without copying) passed directly to `json::parse(buffer->data(), buffer->size())` would eliminate one allocation per message. | Medium |

---

## 11. Infrastructure — OkxAdapter

### 11.1 Design

REST for order entry/cancel/amend/query. Private WebSocket for execution updates. `callback_mutex_` protects the alias map and callbacks. `instrument_cache_mutex_` protects the 30-second instrument rules cache. `order_rate_limiter_` is a `TokenBucket`.

### 11.2 Issues Found

| # | Location | Issue | Severity |
|---|---|---|---|
| O-1 | `callback_mutex_` | Protects both `alias_to_client_` (a lookup map) and `execution_callback_` / `connection_callback_`. The callbacks are set once at `start()` and never changed. They do not need to share a mutex with the alias map. Separate them: one `std::atomic<bool> started_` flag for the callbacks (set-once), and `alias_mutex_` for the map. | Medium |
| O-2 | `alias_to_client_` | `std::unordered_map<std::string, std::string>` — both key and value are `std::string`. Keys are OKX client IDs (truncated to 32 chars). Values are canonical `clientOrderId`. Lookups in `websocket_message()` hold `callback_mutex_` while searching. A `StringMap<std::string>` with transparent hash would allow `string_view` lookups without constructing a temporary `std::string`. | Low |
| O-3 | `rest_request()` | Constructs `HttpRequest` with `std::unordered_map<std::string, std::string> headers` — 6 header entries allocated per REST call. A `std::array<std::pair<std::string_view, std::string_view>, 8>` or a small fixed-size header list would avoid the map allocation. | Medium |
| O-4 | `write_operation()` | Calls `body.dump()` to serialize the JSON body to a `std::string`, then passes it to `rest_request()`. The string is then moved into `HttpRequest::body`. One allocation. Acceptable. | Info |
| O-5 | `websocket_message()` | Parses the entire message with `nlohmann::json::parse(message)` even for heartbeat `"pong"` responses. A fast string comparison before parsing would skip the JSON parse for the common heartbeat case. | Low |
| O-6 | `instrument_cache_` | `std::unordered_map<std::string, std::pair<steady_clock::time_point, InstrumentRulesQueryResult>>` — `InstrumentRulesQueryResult` contains `InstrumentRules` with 14 `std::optional<Decimal>` fields and 3 `std::string` fields. The cache is correct (30-second TTL) but the value type is large. A pointer to a heap-allocated result would reduce copy cost on cache hit. | Low |

---

## 12. Infrastructure — BinanceAdapter

### 12.1 Design

All order operations over a single authenticated WebSocket. Request-response correlation via `pending_` map of `std::promise<nlohmann::json>`. Clock synchronization via `time` method + midpoint compensation. `clock_sync_thread_` runs every 5 seconds.

### 12.2 Issues Found

| # | Location | Issue | Severity |
|---|---|---|---|
| B-1 | `pending_` map | `std::unordered_map<std::string, std::shared_ptr<std::promise<nlohmann::json>>>` — every in-flight request allocates a `std::string` key, a `shared_ptr`, and a `promise`. For a trading gateway that may have 10–20 concurrent requests, this is acceptable. For higher throughput, a `std::array` of pre-allocated promise slots indexed by a monotonic counter would be allocation-free. | Medium |
| B-2 | `mutex_` | Single mutex protecting `alias_to_client_`, `pending_`, `subscription_request_id_`, `time_request_id_`, `time_request_sent_at_`, `execution_callback_`, `connection_callback_`. This is a wide lock. The callbacks are set-once; the alias map and pending map have different access patterns. Splitting into `alias_mutex_` and `pending_mutex_` would reduce contention. | Medium |
| B-3 | `signed_request()` | Calls `request()` which waits on `subscription_condition_` (holding `mutex_` during the wait). This means the mutex is held for the entire subscription wait duration (up to `request_timeout` = 5s). Other threads calling `restore()` or `fail_pending()` are blocked for this entire period. The wait should release the mutex (use `condition_variable::wait` not a spin). Looking at the code: `subscription_condition_.wait_for(lock, ...)` — this correctly releases the lock during the wait. | Info |
| B-4 | `sign_parameters()` | Calls `canonical_query(parameters)` (not shown but presumably sorts and encodes JSON fields). If this allocates a `std::string` per field, it is O(n_fields) allocations per signed request. A stack-allocated buffer with `std::to_chars` for numeric fields would be faster. | Medium |
| B-5 | `query()` | Constructs a synthetic `executionReport` JSON object manually to reuse `BinanceProtocol::parse_execution_report()`. This is fragile — if the parse function changes its expected field names, the synthetic construction silently breaks. A dedicated `parse_order_status()` function would be safer. | Medium |
| B-6 | `clock_mutex_` | Separate from `mutex_` — correct, since `signed_timestamp()` is called from `sign_parameters()` which is called from `signed_request()` which already holds `mutex_`. Nested locking `mutex_` → `clock_mutex_` would deadlock. The separation is intentional and correct. | Info |
| B-7 | `clock_sync_wait_mutex_` + `clock_sync_condition_` | Used only by `run_clock_sync()` to implement the periodic resync sleep. A `std::this_thread::sleep_for` with a `stop_token` check would be simpler and avoid an extra mutex. | Low |

---

## 13. Infrastructure — TokenBucket / RateLimiter

### 13.1 Design

`TokenBucket` with `double` capacity, tokens, and refill rate. Protected by `std::mutex`. `try_acquire()` refills then checks.

### 13.2 Issues Found

| # | Location | Issue | Severity |
|---|---|---|---|
| T-1 | `std::mutex` in `TokenBucket` | Every `try_acquire()` call acquires a mutex. `try_acquire()` is called on every order operation (place, cancel, amend, query). For a single-threaded adapter this mutex is uncontested but still has acquire/release overhead. An `std::atomic<double>` with CAS-based update (or a `std::atomic<int64_t>` token counter with integer arithmetic) would be lock-free. | Medium |
| T-2 | `double` arithmetic | Using `double` for token counts introduces floating-point non-determinism across platforms. An `int64_t` token counter (e.g. tokens × 1000 for sub-token precision) would be deterministic and faster. | Low |
| T-3 | `synchronize()` | Called from `observe_rate_limits()` in `BinanceAdapter` on every response. Acquires the mutex. Since this is called from the ASIO thread (WebSocket message handler), it contends with `try_acquire()` calls from the order operation threads. | Low |

---

## 14. Server Layer — HttpServer / GatewayApi

### 14.1 Design

Boost.Beast async HTTP/WebSocket server. `HttpServer::Impl` owns the `io_context` and thread pool. `ServerState` holds the `GatewayApi` and the WebSocket session registry. `WebSocketSession` has a 256-message outbound bound with `resync.required` on overflow.

### 14.2 Correctness Assessment

✅ WebSocket sessions use `std::enable_shared_from_this` — correct lifetime management.  
✅ `sessions_` vector of `std::weak_ptr` — expired sessions are pruned on every broadcast.  
✅ `resync_pending_` flag correctly replaces the backlog with a single resync message.  
✅ Static file whitelist (`routes` map) prevents directory traversal.  
✅ Security headers (`CSP`, `X-Content-Type-Options`, `X-Frame-Options`) on static files.  

### 14.3 Issues Found

| # | Location | Issue | Severity |
|---|---|---|---|
| S-1 | `ServerState::broadcast()` | Acquires `sessions_mutex_`, prunes expired sessions, copies live `shared_ptr`s into a local vector, releases lock, then calls `session->send()` for each. The `sessions_` vector is a `std::vector<std::weak_ptr<WebSocketSession>>` — `lock()` on each weak_ptr allocates a `shared_ptr` control block reference. With many sessions this is O(n) allocations per broadcast. A `std::vector<std::shared_ptr<WebSocketSession>>` with explicit removal on disconnect would avoid the weak_ptr overhead. | Medium |
| S-2 | `publish_order()` / `publish_quote()` / `publish_operational()` | Each constructs a `nlohmann::json` object, calls `.dump()` to produce a `std::string`, then calls `broadcast()` which copies that string into every session's outbound queue. With N sessions, N string copies. A `std::shared_ptr<const std::string>` payload shared across all sessions would reduce this to one allocation + N reference count increments. | Medium |
| S-3 | `GatewayApi::handle()` | Parses `request.target` with `parse_target()` on every request — allocates a `ParsedTarget` with a `StringMap<std::string>` for query parameters. For the common case of no query parameters, this map is empty but still allocated. A lazy-parsed query string would avoid the allocation for routes without query params. | Low |
| S-4 | `error_status()` | Long `if/else` chain of `string_view` comparisons. Should be a `static const std::unordered_map<std::string_view, unsigned>` for O(1) lookup and open/closed extensibility. | Low |
| S-5 | `require_only_fields()` | Constructs `std::unordered_set<std::string_view>` from an `initializer_list` on every POST request. Since the allowed field sets are compile-time constants, a `constexpr std::array` with a linear scan (8 fields max) would be faster and allocation-free. | Low |
| S-6 | `request_headers()` | Constructs a `StringMap<std::string>` from all HTTP headers, lowercasing each name. Called on every HTTP request. Most routes only need 1–2 headers (`content-type`, `idempotency-key`). Lazy lookup directly on the Beast request fields would avoid the map construction entirely. | Low |
| S-7 | `read_static_file()` | `static const std::unordered_map<std::string_view, std::string_view> routes` — correct use of `static` for the map. File is read from disk on every request with no caching. For a production server, static files should be loaded once at startup and served from memory. | Low |
| S-8 | WebSocket `accepted()` | Calls `state_->gateway.list()` to build the initial snapshot — full order copy under `OrderGateway::mutex_`. For a large order book this blocks the gateway mutex during the WebSocket handshake. A snapshot should be taken asynchronously or the gateway should expose a read-only snapshot type. | Medium |

---

## 15. CLI Layer — CommandProcessor

### 15.1 Design

`CommandProcessor::execute()` parses a `string_view` command line, dispatches to `OrderGateway` methods, and returns a JSON string. Simulation-specific `simulate` command calls `SimulatedExchangeAdapter::emit()` directly.

### 15.2 Issues Found

| # | Location | Issue | Severity |
|---|---|---|---|
| C-1 | `execute(std::string_view line)` | Parses the command line by splitting on spaces into a `std::vector<std::string>`. Every token is a heap allocation. A `std::vector<std::string_view>` into the original `line` would be allocation-free for parsing. | Low |
| C-2 | `CommandResponse::output` | `std::string` — the JSON output of every command. For non-interactive script use (`--command`) this is fine. For the interactive REPL, the string is printed and discarded. | Info |
| C-3 | `simulated_` map | `std::unordered_map<Venue, std::shared_ptr<SimulatedExchangeAdapter>>` — the `Venue` enum is used as a key. `std::unordered_map` with an enum key requires a hash specialization or a custom hash. Verify that the default `std::hash<Venue>` is available (it is, since `Venue` is an `enum class` with `int` underlying type in C++14+). | Info |

---

## 16. String Usage and Allocation Audit

This section catalogs every place where `std::string` is used where `std::string_view` or a static literal would suffice, ranked by call frequency.

### 16.1 High-Frequency Paths (called per order state change)

| Location | Current | Should Be | Impact |
|---|---|---|---|
| `record_event()` parameters: `category`, `code`, `message` | `std::string` by value | `std::string_view` | Eliminates 3 string copies per event; ~10 events per order lifecycle |
| `order_event_context()` — 10 fields | `std::string` via `.to_string()` and `std::string(to_string(...))` | `const Order&` reference + lazy format on serialization | Eliminates ~10 allocations per state change |
| `OrderStateMachine::apply()` — `old_exchange_id` | `std::string` copy | Compare pointer/hash or use `string_view` | 1 allocation per apply |
| `ApplyResult::reason` | `std::string` | `std::string_view` into static literal | 1 allocation per non-applied result |
| `ExecutionReport::event_id` synthetic construction | `"place-reject-" + id + '-' + version` | `uint64_t` hash | 3 allocations per rejection |

### 16.2 Medium-Frequency Paths (called per order operation)

| Location | Current | Should Be | Impact |
|---|---|---|---|
| `fingerprint(OrderRequest)` return | `std::string` | `uint64_t` FNV hash | 1 allocation per place() |
| `request_key` in `cancel()` / `amend()` | `"CANCEL:" + request_id` | Typed key struct or `string_view` composite | 1 allocation per cancel/amend |
| `RiskDecision::reject()` parameters | `std::string` by value | `std::string_view` | 2 allocations per rejection |
| `Decimal::to_string()` in risk messages | `std::string` | `to_chars` into caller buffer | 2–4 allocations per rejection message |
| `OkxAdapter::rest_request()` headers map | `unordered_map<string, string>` | `array<pair<string_view, string_view>>` | 6 allocations per REST call |

### 16.3 Low-Frequency Paths (called per connection / startup)

| Location | Current | Should Be | Impact |
|---|---|---|---|
| `MarketQuote::symbol` from ring | `std::string` constructed from `char[16]` | `string_view` into static symbol table | 1 allocation per ring record read |
| `WebSocket message` in `read_next` | `beast::buffers_to_string()` | `string_view` into buffer | 1 allocation per WS message |
| `GatewayApi::parse_target()` | `StringMap<string>` for query params | Lazy `string_view` pairs | 1 map allocation per HTTP request |

### 16.4 Summary Count

Estimated allocations per `place()` call (happy path, durable writes):

| Step | Allocations |
|---|---|
| `fingerprint(request)` | 1 |
| `order_event_context()` × 3 events | ~30 |
| `record_event()` string params × 3 | ~9 |
| `persist_locked()` → JSON serialization | ~20 |
| `make_order()` → Order construction | ~5 |
| Total (approximate) | **~65 allocations** |

With the proposed changes (string_view params, lightweight event context, hash fingerprint):

| Step | Allocations |
|---|---|
| `fingerprint(request)` | 0 |
| `order_event_context()` × 3 events | 0 |
| `record_event()` string params × 3 | 0 |
| `persist_locked()` → JSON serialization | ~20 |
| `make_order()` → Order construction | ~5 |
| Total (approximate) | **~25 allocations** |

JSON serialization dominates after the other fixes. Replacing `nlohmann::json` with a binary format (SBE, FlatBuffers) on the journal path would reduce this further, but that is a larger architectural change.

---

## 17. Mutex and Lock Contention Audit

### 17.1 Lock Inventory

| Mutex | Owner | Held During |
|---|---|---|
| `OrderGateway::mutex_` | `OrderGateway` | Order map reads/writes, index updates, health updates, observer calls, `persist_locked()` (file I/O) |
| `OrderGateway::operational_mutex_` | `OrderGateway` | Operational event deque, observer calls, `append_event()` (file I/O) |
| `OrderGateway::reconciliation_mutex_` | `OrderGateway` | Reconciliation queue deque |
| `MarketDataBook::mutex_` | `MarketDataBook` | Quote map reads/writes, observer copy |
| `FileOrderStore::mutex_` | `FileOrderStore` | JSON serialization + `write_all()` + `fdatasync()` |
| `MemoryOrderStore::mutex_` | `MemoryOrderStore` | Vector append + map rebuild |
| `OkxAdapter::callback_mutex_` | `OkxAdapter` | Alias map + callback reads |
| `OkxAdapter::instrument_cache_mutex_` | `OkxAdapter` | Cache reads/writes |
| `BinanceAdapter::mutex_` | `BinanceAdapter` | Alias map + pending map + callbacks |
| `BinanceAdapter::clock_mutex_` | `BinanceAdapter` | Clock sync state |
| `BinanceAdapter::clock_sync_wait_mutex_` | `BinanceAdapter` | Clock sync thread sleep |
| `BinanceAdapter::instrument_cache_mutex_` | `BinanceAdapter` | Cache reads/writes |
| `TokenBucket::mutex_` | `TokenBucket` (×3) | Token refill + acquire |
| `ReconnectingWebSocket::connection_mutex_` | `ReconnectingWebSocket` | `wait_connected()` |
| `ServerState::sessions_mutex_` | `ServerState` | Session list reads/writes |

**Total: 15 mutexes** across the system.

### 17.2 Critical Contention Points

| # | Contention Point | Threads Involved | Severity |
|---|---|---|---|
| L-1 | `OrderGateway::mutex_` held during `fdatasync()` | REST handler thread + BoundedEventDispatcher worker thread | Critical |
| L-2 | `OrderGateway::mutex_` → `operational_mutex_` nested lock in `apply_execution()` | Dispatcher worker + REST handler | High |
| L-3 | `FileOrderStore::mutex_` held during `fdatasync()` | Any thread calling `append()` or `append_event()` | High |
| L-4 | `MarketDataBook::mutex_` held during observer copy + notification | Ring feed thread + REST handler thread | Medium |
| L-5 | `BinanceAdapter::mutex_` held during `subscription_condition_.wait_for()` | Order operation threads waiting for subscription | Medium |

### 17.3 Zero-Lock Opportunities

| Component | Current | Zero-Lock Alternative |
|---|---|---|
| `MarketDataBook::latest()` | `std::mutex` + copy | Seqlock or `std::atomic<MarketQuote>` (if trivially copyable) per slot |
| `TokenBucket::try_acquire()` | `std::mutex` | `std::atomic<int64_t>` token counter with CAS |
| `BoundedEventDispatcher` queue | `std::mutex` + 3 CVs | SPSC lock-free ring (Dmitry Vyukov / folly SPSC queue pattern) |
| `SequenceTracker` | No lock (correct — called under `OrderGateway::mutex_`) | Already zero-lock |
| `OkxAdapter::authenticated_` | `std::atomic<bool>` | Already lock-free |
| `BinanceAdapter::subscribed_` | `std::atomic<bool>` | Already lock-free |

### 17.4 Lock-Order Graph

The following lock-order dependencies exist and must be respected to prevent deadlock:

```
OrderGateway::mutex_
    └── OrderGateway::operational_mutex_   (record_event called inside mutex_)
    └── FileOrderStore::mutex_             (persist_locked → append)

OrderGateway::operational_mutex_
    └── FileOrderStore::mutex_             (append_event)

BinanceAdapter::mutex_
    └── BinanceAdapter::clock_mutex_       (signed_timestamp called from sign_parameters)
```

No cycles detected. The lock order is consistent throughout the codebase.

---

## 18. Hot-Path Allocation Audit

### 18.1 Critical Hot Path: Execution Report Ingestion

```
Exchange WebSocket frame arrives
    → ReconnectingWebSocket::read_next()
        → beast::buffers_to_string()          [ALLOC: string copy of frame]
        → nlohmann::json::parse()             [ALLOC: JSON DOM]
        → OkxProtocol::parse_order_update()   [ALLOC: ExecutionReport strings]
        → execution_callback_()
            → OrderGateway::receive_execution()
                → BoundedEventDispatcher::submit()
                    → queue_[tail_].emplace()  [ALLOC: optional<pair<Venue, ExecutionReport>>]
                    → mutex_ acquire/release
                    → not_empty_.notify_one()
    → BoundedEventDispatcher worker wakes
        → OrderGateway::apply_execution()
            → mutex_ acquire
            → OrderStateMachine::apply()
                → old_exchange_id copy         [ALLOC: string]
            → persist_locked()
                → order_store_->append()
                    → nlohmann::json(order)    [ALLOC: JSON DOM]
                    → payload.dump()           [ALLOC: string]
                    → record.dump()            [ALLOC: string]
                    → write_all() + fdatasync  [SYSCALL]
            → record_event()
                → order_event_context()        [ALLOC: 10 strings]
                → operational_mutex_ acquire
                → append_event()               [ALLOC: JSON + string]
                → operational_mutex_ release
            → mutex_ release
```

**Estimated allocations per execution report: ~40–60**  
**Estimated syscalls per execution report (durable): 2 (write + fdatasync)**

### 18.2 Pre-Allocation Opportunities

| Component | Opportunity |
|---|---|
| `BoundedEventDispatcher` queue | Pre-allocate `ExecutionReport` slots in the ring; use placement new |
| `OrderEventContext` | Eliminate entirely; pass `const Order&` to event recorder |
| `record_event()` string params | Change to `string_view`; no allocation for literals |
| `old_exchange_id` in `apply()` | Compare by pointer/hash; no copy |
| `beast::buffers_to_string()` | Pass `string_view` directly to `json::parse()` |
| `nlohmann::json` DOM per append | Use a streaming JSON writer into a pre-allocated `std::string` buffer |

### 18.3 Memory Layout Observations

| # | Observation | Severity |
|---|---|---|
| H-1 | `Order` is ~400 bytes on a 64-bit system (5 maps + 2 sets + 15 scalar fields). Storing orders in `StringMap<Order>` means each order is a separate heap allocation. A pool allocator or `std::deque<Order>` with a separate index map would improve cache locality. | Medium |
| H-2 | `RingRecord` is `alignas(64)` — one cache line. `RingHeader` is `alignas(64)`. The writer and reader access different cache lines for the header vs records — correct false-sharing prevention. | Info |
| H-3 | `BoundedEventDispatcher::queue_` is `std::vector<std::optional<std::pair<Venue, ExecutionReport>>>`. Each element is ~120 bytes (ExecutionReport with 4 strings). A 4096-element queue is ~480KB — fits in L2 cache on most systems. | Info |

---

## 19. Test Coverage Audit

### 19.1 Current Test Inventory

| File | Tests | What Is Covered |
|---|---|---|
| `test_decimal.cpp` | ~8 | Parse, arithmetic, comparison, edge cases |
| `test_order_state_machine.cpp` | 5 | Fills, terminal states, duplicates, out-of-order, invalid quantities, historical replacement |
| `test_risk_manager.cpp` | ~6 | Size, notional, position limits, market reference price |
| `test_sequence_tracker.cpp` | ~4 | First, contiguous, duplicate, stale, gap |
| `test_protocols.cpp` | ~8 | OKX + Binance JSON parsing, symbol mapping, status normalization |
| `test_gateway.cpp` | 12 | Idempotency, race, amend/cancel lifecycle, Binance replacement, disconnect/reconnect, sequence gaps, reconciliation ownership, stream ownership, risk rejections, balance rejection, venue rules, CLI balance |
| `test_gateway_api.cpp` | ~8 | REST schema validation, field rejection, status codes, WebSocket snapshot/update |
| `test_recovery.cpp` | 6 | Journal recovery, torn append, restart lifecycle, Binance replacement restart, durable events |
| `test_properties.cpp` | 1 | 100 trials × 200 random transitions (20,000 total) |
| `test_environment.cpp` | ~3 | `.env` file loading, override, missing file |
| `test_market_data.cpp` | ~5 | Ring write/read, generation change, stale detection, book publish |
| `test_http_server.cpp` | ~4 | Loopback HTTP, WebSocket delivery |
| **Total** | **~70** | |

### 19.2 Coverage Gaps

| # | Missing Test | Severity |
|---|---|---|
| T-1 | `Decimal` overflow behavior — what happens when two large values are multiplied? Is `__int128` used? No test verifies overflow protection. | High |
| T-2 | `Decimal::parse()` with invalid input — empty string, non-numeric, scientific notation, overflow. Only happy-path parsing is tested. | High |
| T-3 | `OrderStateMachine` — full-fill wins cancel/fill race. The architecture doc describes this rule but no test exercises it directly (the property test may hit it randomly). | High |
| T-4 | `OrderStateMachine` — `FILLED` cannot regress to `CANCELED`. Tested implicitly in `test_order_state_machine.cpp` line 3 but not as an explicit named test case. | Medium |
| T-5 | `BoundedEventDispatcher` — queue full behavior. No test verifies that `submit()` returns `false` when the queue is full and the timeout expires, and that `dropped_events` is incremented. | High |
| T-6 | `BoundedEventDispatcher` — concurrent producers. No test exercises multiple threads calling `submit()` simultaneously. | Medium |
| T-7 | `FileOrderStore` — concurrent `append()` + `load_latest()`. No test verifies thread safety of the store under concurrent access. | High |
| T-8 | `FileOrderStore` — journal lock contention. No test verifies that a second `FileOrderStore` on the same path throws `"already owned by another process"`. | Medium |
| T-9 | `MarketDataBook` — stale quote rejection. `fresh()` is tested indirectly but no test explicitly verifies that a quote older than `maximum_age` is rejected by `price()`. | Medium |
| T-10 | `MarketDataBook` — concurrent `publish()` + `latest()`. No thread-safety test. | Medium |
| T-11 | `OkxProtocol` — amend request serialization. `parse_ack` is tested but `amend_request()` JSON structure is not verified. | Medium |
| T-12 | `BinanceProtocol` — cancel-replace response parsing with partial success (cancel succeeded, replacement failed). | High |
| T-13 | `OrderGateway::place()` — `MARKET_DATA_UNAVAILABLE` rejection when market data is configured but stale. | Medium |
| T-14 | `OrderGateway::amend()` — deferred Binance amend report processing. The `deferred_amend_reports_` path is exercised by `test_gateway.cpp` "Binance amend rolls physical generations" but the deferred path (report arrives before amend ACK) is not explicitly tested. | High |
| T-15 | `OrderGateway::reconcile()` — `TERMINAL_ORDER_STILL_OPEN` conflict. No test places an order, marks it terminal locally, then has the venue report it as open. | Medium |
| T-16 | `ReconnectingWebSocket` — heartbeat timeout. No test verifies that a missing heartbeat response triggers reconnect. | Medium |
| T-17 | `TokenBucket` — `synchronize()` from Binance rate limit response. No test verifies that the bucket capacity and refill rate are correctly updated. | Low |
| T-18 | `GatewayApi` — `PATCH` with `Idempotency-Key` header (no `requestId` in body). | Low |
| T-19 | `GatewayApi` — `DELETE` with `Idempotency-Key` header. | Low |
| T-20 | `MarketDataRing` — reader with lagging cursor (cursor.sequence < first_available). No test verifies that the reader correctly resumes at the oldest available record. | Medium |
| T-21 | `SimulatedExchangeAdapter` — market order fill at ask/bid price. No test verifies that a market order placed in simulation fills at the current mapped quote. | Medium |
| T-22 | `SimulatedExchangeAdapter` — limit order fill when mapped quote crosses limit. | Medium |
| T-23 | Position limit enforcement across multiple orders on the same symbol. | Medium |
| T-24 | `OrderGateway::stability()` — `logging_failures_` counter incremented when `append_event()` throws. | Low |

### 19.3 Test Quality Observations

| # | Observation | Severity |
|---|---|---|
| Q-1 | `test_support.hpp` `GatewayFixture` always uses `reconcile_on_start = false` by default. Most gateway tests therefore never exercise the startup reconciliation path. Tests that need it pass `true` explicitly — correct, but the default hides reconciliation bugs. | Medium |
| Q-2 | `test_properties.cpp` uses a fixed seed (`0xabc123U`) — deterministic and reproducible. Good. But the property only tests `OrderStateMachine` in isolation. A property test over the full `OrderGateway` (random sequence of place/amend/cancel/emit) would catch more integration-level invariants. | Medium |
| Q-3 | All tests use `MemoryOrderStore` by default. `FileOrderStore` is only used in `test_recovery.cpp`. The journal's mutex behavior under concurrent access is never tested. | Medium |
| Q-4 | No test measures latency or throughput. The benchmark (`hot_paths.cpp`) is a separate binary and not part of `ctest`. Adding a latency regression test (e.g. assert that 1000 state machine transitions complete in < 1ms) would catch performance regressions. | Low |

---

## 20. Benchmark Coverage Audit

### 20.1 Current Benchmarks (`benchmarks/hot_paths.cpp`)

| Benchmark | What It Measures |
|---|---|
| `benchmark_risk_snapshot()` | `RiskManager::check_new()` with 10,000 orders (snapshot copy path) vs `check_new_with_position()` (pre-computed position path) |
| `benchmark_market_lookup()` | `MarketDataBook::latest()` — 2,000,000 iterations |
| `benchmark_dispatcher()` | `BoundedEventDispatcher::submit()` — 200,000 events |
| `benchmark_journal()` | `FileOrderStore::append_event()` — 5,000 appends, non-durable |

### 20.2 Missing Benchmarks

| # | Missing Benchmark | Why Important |
|---|---|---|
| B-1 | `OrderStateMachine::apply()` — 1,000,000 transitions | The state machine is the innermost hot loop; its allocation cost (string copies) should be measured |
| B-2 | `Decimal::to_string()` — 1,000,000 calls | Called ~10× per order event; allocation cost should be quantified |
| B-3 | `OrderGateway::place()` end-to-end (simulation, non-durable) | Measures the full orchestration path including risk, journal, and observer dispatch |
| B-4 | `MarketDataRing` write + read round-trip | Measures the mmap IPC latency between publisher and gateway |
| B-5 | `FileOrderStore::append()` with `fdatasync` (durable) | Measures the worst-case journal latency that blocks `OrderGateway::mutex_` |
| B-6 | `BoundedEventDispatcher` under contention (2 producer threads) | Measures mutex contention on the ingestion queue |
| B-7 | JSON serialization of `Order` | `nlohmann::json(order).dump()` — the dominant allocation in `persist_locked()` |

### 20.3 Benchmark Infrastructure Observations

| # | Observation | Severity |
|---|---|---|
| BM-1 | `nanoseconds_per_operation()` uses `std::chrono::steady_clock` — correct for benchmarking. | Info |
| BM-2 | No warmup iterations before measurement. CPU frequency scaling and cache cold-start can skew first-run results. Add 10% warmup iterations. | Low |
| BM-3 | `benchmark_market_lookup()` calls `book.latest()` 2,000,000 times but the book has only one quote. This measures the mutex acquire/release + map lookup latency, not cache miss behavior. Add multiple symbols to stress the map. | Low |
| BM-4 | Benchmarks are not part of `ctest` and require `-DABEX_BUILD_BENCHMARKS=ON`. They should be runnable as part of a performance CI gate. | Low |

---

## 21. Prioritised Improvement Roadmap

Issues are grouped by impact tier. Each item references the finding number from earlier sections.

### Tier 1 — Critical (correctness risk or severe performance impact)

| Priority | Finding | Change | Expected Impact |
|---|---|---|---|
| 1 | G-1, L-1 | Decouple `persist_locked()` from `OrderGateway::mutex_`. Persist asynchronously via a dedicated journal writer thread; notify the caller after the write completes via a future/callback. | Eliminates `fdatasync` blocking the gateway mutex; reduces `place()` latency by 1–10ms |
| 2 | E-1 | Replace `BoundedEventDispatcher` queue with a lock-free SPSC ring of pre-allocated `ExecutionReport` slots using `std::atomic` sequence numbers (same pattern as `MarketDataRing`). | Eliminates mutex + condition variable on the execution ingestion hot path |
| 3 | D-3 | Verify and fix `Decimal` multiplication overflow. Use `__int128` for intermediate products. Add overflow tests. | Prevents silent monetary calculation errors at large values |
| 4 | T-1, T-2 | Add `Decimal` overflow and invalid-parse tests. | Prevents silent data corruption |
| 5 | T-5, T-7 | Add `BoundedEventDispatcher` queue-full test and `FileOrderStore` concurrent-access test. | Prevents untested failure modes in production |

### Tier 2 — High (significant allocation reduction or important missing tests)

| Priority | Finding | Change | Expected Impact |
|---|---|---|---|
| 6 | G-12, D-15, D-16 | Change `record_event()` parameters to `std::string_view`. Replace `OrderEventContext` with `const Order*`. Change `OperationalEvent::category` and `code` to `std::string_view`. | Eliminates ~40 allocations per order lifecycle |
| 7 | D-1 | Replace `Decimal::to_string()` with a `to_chars`-based formatter writing into a caller-supplied `char[32]` buffer. | Eliminates 1 allocation per monetary value display |
| 8 | M-1 | Replace `MarketDataBook::latest()` mutex + copy with a seqlock or per-slot `std::atomic` for the two fixed quote slots. | Eliminates mutex on the market data read path (called per `place()`) |
| 9 | T-3, T-12, T-14 | Add tests: full-fill wins cancel race; Binance cancel-replace partial success; deferred amend report processing. | Covers three high-risk correctness paths |
| 10 | J-1, J-2 | Move `FileOrderStore::append_record()` serialization outside the mutex. Use the in-memory `operational_events_` deque as the primary source for `load_events()`. | Reduces lock hold time; eliminates file re-reads on every health poll |

### Tier 3 — Medium (code quality, minor performance, test completeness)

| Priority | Finding | Change | Expected Impact |
|---|---|---|---|
| 11 | T-1 (token bucket) | Replace `TokenBucket::mutex_` with `std::atomic<int64_t>` token counter. | Lock-free rate limiting |
| 12 | R-1 (ring read) | Change `MarketDataRingReader::read_available()` to write into a caller-supplied `std::span<MarketQuote>`. | Eliminates vector allocation every 50ms |
| 13 | D-14 | Intern `MarketQuote::symbol` as `std::string_view` into a static symbol table. | Eliminates string allocation per ring record |
| 14 | S-1, S-2 | Use `std::shared_ptr<const std::string>` for broadcast payloads; replace `weak_ptr` session list with explicit removal. | Reduces broadcast allocation from O(N) to O(1) |
| 15 | A-1 | Introduce a `OrderSnapshot` lightweight read-only view type for `get()` and `list()` returns. | Reduces copy cost for read-only consumers |
| 16 | G-6 | Add a per-symbol position index in `OrderGateway` to make `conservative_position_locked()` O(1). | Reduces position check from O(n_orders) to O(1) |
| 17 | B-1 through B-7 | Add missing benchmarks for state machine, Decimal, end-to-end place, ring round-trip, durable journal, dispatcher contention, JSON serialization. | Enables performance regression detection |
| 18 | T-9 through T-24 | Add the 16 missing test cases identified in Section 19. | Increases coverage of edge cases and failure modes |

### Tier 4 — Low (polish, minor improvements)

| Priority | Finding | Change |
|---|---|---|
| 19 | A-4 | Use `StringMap`/`StringSet` for all `Order` internal maps/sets |
| 20 | S-4 | Replace `error_status()` if/else chain with a static lookup map |
| 21 | S-5 | Replace `require_only_fields()` set with `constexpr std::array` |
| 22 | W-5 | Pass `string_view` into `json::parse()` instead of `buffers_to_string()` |
| 23 | G-16, G-17 | Replace reconciliation deque with two `std::atomic<bool>` flags |
| 24 | B-7 (clock sync) | Replace `clock_sync_wait_mutex_` + CV with `stop_token`-aware sleep |
| 25 | J-7 | Design and document a journal compaction strategy |
| 26 | BM-2, BM-3 | Add warmup iterations and multi-symbol stress to benchmarks |

---

*End of ABEX Audit Report*
