# ABEX audit implementation report

**Date:** 2026-08-19  
**Input:** `AUDIT_REPORT.md` and `project_requirements.txt`  
**Scope:** correctness, concurrency, durability, allocation behavior, CLI/UI reads, tests, and benchmarks

## Final conclusion

All 26 items in the audit's prioritised roadmap are addressed. “Addressed” does not mean blindly
applying every proposed data type: several proposals were corrected because they would introduce
multiple producers into one SPSC queue, dangling views across asynchronous boundaries, hash-collision
idempotency, or uninterruptible shutdown sleeps.

| Tier | Roadmap items | Addressed | Main outcome |
|---|---:|---:|---|
| Tier 1 — critical | 5 | 5 | Durable I/O removed from global state lock; one SPSC lane per adapter; exact Decimal boundaries tested |
| Tier 2 — high | 5 | 5 | Typed event context, stack Decimal formatting, lock-free quote reads, journal caches, race tests |
| Tier 3 — medium | 8 | 8 | Atomic GCRA limiter, span ring reads, shared broadcasts, lightweight snapshots, O(1) positions, full benchmarks/tests |
| Tier 4 — low | 8 | 8 | Transparent maps, allocation-free scans/parsing, atomic reconciliation mask, compaction design, benchmark warmup |

## Execution dispatch: corrected topology

The original audit called the old dispatcher SPSC even though the gateway callback boundary was
multi-producer. A single replacement SPSC would therefore be incorrect. The implementation applies
single responsibility at the adapter boundary:

```text
OKX private WS I/O thread ───────> OKX SPSC lane ───────> OKX execution consumer
Binance private WS I/O thread ───> Binance SPSC lane ───> Binance execution consumer

OKX/Binance synchronous query ───> AdapterResult.authoritative_reports
                                   └──────── direct ordered apply; never enters a WS lane

Simulator callers ── execution_emit_mutex ──> venue SPSC lane
                  (arbitrary threads become one serialized logical producer)
```

Each lane owns a fixed-capacity, preallocated ring. Atomic head and tail counters occupy separate
cache lines. The producer waits at most the configured 1 ms on full capacity; counting semaphores
park a full producer or idle consumer, and atomic wait parks flush callers. An `atomic_flag` detects an accidental
second producer and fails closed into reconciliation instead of racing the producer-owned tail.

## Durability and lock ownership

The durability invariant remains: intent and request ID are on stable storage before venue I/O.
The global state mutex is no longer held during JSON write or `fdatasync()`.

```text
operation thread
  persistence-order gate
    state mutex: mutate + copy durable snapshot
    state mutex released
    FileOrderStore: append + optional fdatasync
  persistence-order gate released
  observers notified outside all state/durability locks

operational event
  bounded audit queue -> one OperationalEventWriter -> FileOrderStore
  completion -> cache/counters/observers outside gateway state lock
```

The persistence-order gate is intentionally serialized: it preserves mutation-to-WAL order while
allowing `get`, lightweight snapshots, market reads, and health reads to proceed during disk sync.
Callers still wait for their own durable order transition before receiving success.

| Previous contention | Current disposition |
|---|---|
| Gateway state mutex held across `fdatasync` | Removed; only the persistence-order gate spans sync |
| State mutex nested into operational mutex/file store | Removed by the audit writer |
| Observers invoked under state mutex | Removed; observer list has its own small-vector lock |
| Reconciliation copied all orders | Replaced by client-ID candidates and one query snapshot at a time |
| Position risk scanned all orders | Replaced by an incrementally maintained per-symbol index |

## Roadmap disposition chart

| # | Audit finding | Disposition | Evidence |
|---:|---|---|---|
| 1 | G-1/L-1 durable I/O under global lock | Adapted and implemented | Persistence-order gate plus `OperationalEventWriter`; global state unlocked before append/sync |
| 2 | E-1 lock-free SPSC | Corrected and implemented | One `SpscExecutionLane` per venue; explicit producer contracts and violation detection |
| 3 | D-3 Decimal overflow | Verified and strengthened | `__int128` arithmetic intermediates; checked add/sub/mul/div and signed limits |
| 4 | T-1/T-2 Decimal tests | Implemented | Exact min/max parse, arithmetic overflow, invalid syntax, compile-time literal, stack formatting |
| 5 | T-5/T-7 queue/store concurrency | Implemented | Full/second-producer test; concurrent append/cache-read and journal-owner tests |
| 6 | G-12/D-15/D-16 event allocation | Safely adapted | Context uses typed enums/Decimals; durable/async strings remain owned to prevent dangling views |
| 7 | D-1 Decimal formatter | Implemented | `format_to(span<char>)`, `append_to(string&)`, and `to_chars` whole-number formatting |
| 8 | M-1 market-data read lock | Implemented | Four fixed venue/symbol seqlock slots; consistent atomic snapshots |
| 9 | T-3/T-12/T-14 races | Implemented | Fill/cancel dominance, existing partial cancel-replace failure, explicit deferred-amend test |
| 10 | J-1/J-2 journal work/cache | Implemented | Payload serialized before I/O lock; latest/event/per-order-event caches built once and updated by sequence |
| 11 | Token-bucket mutex | Implemented | Lock-free integer-microtoken GCRA with one CAS and atomic synchronization |
| 12 | R-1 ring output allocation | Implemented | Caller-supplied `span<MarketQuote>` and reusable feed buffer |
| 13 | D-14 borrowed market symbol | Corrected by design | Owning symbol retained across async boundaries; fixed symbols use SSO and reader buffers are reused |
| 14 | S-1/S-2 WS fan-out | Adapted and implemented | One `shared_ptr<const string>` payload for all sessions; weak ownership retained to avoid cycles |
| 15 | A-1 lightweight order read | Implemented | `OrderSnapshot` excludes alias, request, offset, and dedup hash tables; UI/CLI/initial WS use it |
| 16 | G-6 O(1) position | Implemented | Incremental conservative position map, including amend/place/execution transitions |
| 17 | B-1…B-7 benchmarks | Implemented | State, Decimal, place, mmap ring, durable journal, two venue lanes, and JSON serialization |
| 18 | T-9…T-24 tests | Implemented/validated | Stale/concurrent quotes, protocols, stale MARKET reject, reconciliation, heartbeat, headers, ring lag, simulation, position, logging failure |
| 19 | A-4 transparent order maps | Implemented | All order-owned maps/sets use `StringMap`/`StringSet` |
| 20 | S-4 error status chain | Implemented with better small-N structure | `constexpr array` linear scan avoids node allocation and is faster for the small mapping |
| 21 | S-5 allowed fields | Implemented | Allocation-free linear scan of the provided static field list |
| 22 | W-5 WS input copy | Implemented | JSON parses a view over Beast's flat buffer; buffer consumed only after callback returns |
| 23 | G-16/G-17 reconciliation queue | Implemented | Atomic two-bit venue mask; CV only parks the worker |
| 24 | B-7 clock wait | Retained intentionally | Stop-token-aware CV is promptly interruptible; `sleep_for` would make shutdown wait up to 5 seconds |
| 25 | J-7 compaction | Designed and documented | `docs/JOURNAL_COMPACTION.md` specifies checkpoint/segment/manifest transaction and fault tests |
| 26 | BM-2/BM-3 benchmark quality | Implemented | Warmup helper and all four supported venue/symbol quote slots |

## Complete finding-family disposition

| Audit family | Result |
|---|---|
| Architecture/SOLID A-* | Lightweight snapshots added; adapter producer responsibilities separated; unsafe borrowing rejected |
| Domain D-* | Checked Decimal and stack formatting; transparent containers; no old exchange-ID copy; static `ApplyResult` reasons; canonical exact fingerprints |
| Gateway G-* | Disk/observer work outside state lock; O(1) positions; ID-based reconciliation; vector observer registry; atomic pending mask |
| Dispatcher E-* | Old shared dispatcher removed; venue SPSC rings, semaphore parking, 1 ms backpressure, flush and producer-contract tests |
| Market book M-* | Fixed slots, seqlock reads, copy-on-write observer registry, low-frequency status lock |
| Risk R-* | Happy path remains allocation-light and O(1); detailed owning messages retained only on rejection |
| Journal J-* | Single serialization, live latest map, ordered bounded event caches, concurrent sequence-safe updates, compaction design |
| mmap ring R-* | Span API, reusable buffers, generation/lag behavior, filesystem size query, no poll-time vector allocation |
| WebSocket W-* | Bounded outbound queue, zero-copy inbound view, explicit heartbeat state/timeout, no implicit reconnect replay |
| OKX O-* | Alias lock separated from immutable callbacks, transparent aliases, view-based REST header list, heartbeat fast path in transport |
| Binance B-* | Pending and alias locks split, duplicate subscription wait removed, dedicated order-status parser, server-time midpoint/safety margin retained |
| Rate limiter T-* | Deterministic atomic GCRA; burst, synchronization, and contention tests |
| Server S-* | Shared fan-out payload, selected header extraction, static asset cache, allocation-free status/field scans, lightweight snapshots |
| CLI C-* | One owned token buffer plus `string_view` tokens/options; ownership materialized only where commands outlive parsing |
| Test/benchmark Q/BM-* | 67 tests expanded to 84; sanitizer suite and expanded release harness completed |

## Recommendations deliberately not copied literally

| Proposal | Why the literal change is unsafe or inferior | Implemented alternative |
|---|---|---|
| One SPSC queue for the whole gateway | OKX and Binance callbacks are independent producers | One lane per adapter, with REST/query results on a separate return path |
| Store `string_view` in operational events/context | Events cross threads and are journaled after the caller returns | Typed value fields plus owning durable strings |
| Store `string_view` in `MarketQuote` | Quotes cross mmap/feed/observer/thread boundaries | Owning SSO symbol and reused destination capacity |
| Replace exact fingerprint with `uint64_t` FNV | A collision could replay a materially different financial instruction | Versioned, length-delimited exact canonical fingerprint; legacy journal compatibility |
| Replace weak session ownership | `weak_ptr::lock()` does not allocate a control block; strong ownership risks cycles/leaks | Weak lifecycle plus one shared immutable broadcast payload |
| Use an unordered map for a small error table | Hash nodes allocate and lose locality for a small fixed set | `constexpr array` scan |
| Replace stop-aware CV with `sleep_for` | Sleep is not promptly cancelable | Retain the interruptible CV |
| Borrow `const Order*` in queued audit work | State can mutate or rehash before serialization | Typed owned event snapshot |

## Verification chart

| Verification | Result |
|---|---|
| Debug build with warnings | Pass |
| Functional/concurrency tests | 84/84 pass |
| ASan + UBSan + leak detection | 84/84 pass |
| Release benchmark build/run | Pass |
| `git diff --check` | Pass |

Release measurements on this development host:

| Workload | Result |
|---|---:|
| Market quote lookup across four fixed slots | 6 ns/call |
| Decimal stack formatting | 12 ns/call |
| mmap market write/read round trip | 30 ns |
| Order state-machine report | 50 ns/report |
| Two independent venue SPSC lanes | 140 ns/event |
| One venue SPSC lane | 301 ns/event |
| Simulated end-to-end place | 59,891 ns/order |
| Order JSON serialization | 10,771 ns/order |
| Nondurable journal append | 3,715 ns/record |
| Durable journal append | 1,779,089 ns/record |

These are microbenchmarks, not venue latency promises. The durable result is filesystem-dependent and
shows why state-lock decoupling matters; it does not justify acknowledging an order before its WAL
record is stable.

## Residual engineering constraints

- JSON DOM creation remains the largest avoidable allocation cluster in durable serialization.
  Replacing the on-disk schema requires a versioned migration and is not a safe mechanical change.
- `fdatasync` latency is intrinsic when durable writes are enabled. The gateway now isolates that
  latency from state readers but intentionally preserves ordered, synchronous durability per command.
- Automatic compaction is intentionally gated on the crash-injection matrix in the compaction design.
  Enabling deletion before those tests would weaken recovery and audit retention.
- Venue/network latency, TLS, exchange rate limits, and disk characteristics dominate real production
  latency. Run the benchmark and soak tests on the deployment host before setting SLOs.
