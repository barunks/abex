# Performance engineering

ABEX treats performance as a measured correctness constraint. Order ownership, durable strings,
venue correlation, and crash recovery are not weakened to save allocations.

## Implemented hot-path changes

- Domain enums now expose static `std::string_view` names and parse ASCII case-insensitively
  without allocating an uppercase copy.
- Owning string maps use transparent hashing where request/report views perform lookups. The map
  still owns every key; only the lookup is borrowed.
- New-order and amend risk checks calculate a lightweight conservative position while the gateway
  lock is held. They no longer deep-copy every `Order`, including its event and idempotency sets.
- Market quotes are partitioned by venue and use transparent symbol lookup. A read no longer builds
  and hashes a temporary `VENUE:SYMBOL` key.
- Execution ingestion uses one fixed-capacity, preallocated SPSC ring per venue adapter. The ring
  operations are atomic; counting semaphores park an idle consumer or full producer, and atomic
  wait parks a flush caller. Synchronous REST/query reports use `AdapterResult` and never become a second lane
  producer.
- The journal reuses its exclusively locked descriptor, serializes each payload once, and builds
  the record around those bytes. It no longer opens/closes the file or walks the payload twice for
  every append. Latest orders and bounded event indexes are cached in memory. `fdatasync` remains
  enabled when durable writes are configured.
- The gateway state mutex is released before journal write/sync. A separate persistence-order gate
  preserves WAL order, and a dedicated writer serializes operational audit events.
- Market data uses four fixed seqlock slots, the ring reader writes into a reusable span, and
  conservative positions are maintained incrementally per symbol.
- The token limiter is a lock-free integer GCRA; request admission performs one 64-bit CAS.
- The synchronous HTTP transport keeps a four-handle connection pool. OKX REST and public market
  requests can reuse DNS/TCP/TLS state while retaining bounded concurrency.
- Binance query signing iterates the JSON object's existing canonical key order rather than copying
  every key/value into a second tree. OKX ISO-8601 timestamps use a fixed stack buffer rather than
  an allocating stream formatter.

`std::string_view` is deliberately not stored in `Order`, `ExecutionReport`, adapter configuration,
HTTP work, or asynchronous queues. Those values cross call/thread boundaries and require ownership;
borrowing there would trade allocations for dangling references.

## Release benchmark

Build and run the standalone harness with:

```bash
cmake --preset release -DABEX_BUILD_BENCHMARKS=ON
cmake --build --preset release --target abex_benchmark
./build-release/abex_benchmark
```

On the development host, the original release baseline and the median optimized run were:

| Workload | Baseline | Optimized | Change |
|---|---:|---:|---:|
| Risk snapshot copy with 10,000 orders | — | 3,044,450 ns | Reference slow path |
| Position calculation without deep copy | — | 69,236 ns | Reference scan; gateway uses O(1) index |
| Latest market quote lookup, four slots | — | 6 ns | Lock-free read |
| Order state-machine report | — | 50 ns | Allocation-free when no new event ID is owned |
| Decimal caller-buffer formatting | — | 12 ns | No returned string allocation |
| Two independent venue SPSC lanes | — | 140 ns/event | Correct producer topology |
| Simulated end-to-end place | — | 59,891 ns/order | Non-durable memory store |
| Nondurable journal append | — | 3,715 ns | Cached descriptor/indexes |
| Durable journal append | — | 1,779,089 ns | Filesystem-dependent `fdatasync` |

These are microbenchmarks, not exchange round-trip promises. Run them on the deployment host and
pin/load-isolate the process before using the numbers for capacity planning.

## Synchronization policy

Mutexes remain where they protect multi-field order invariants, libcurl handle ownership, low-rate
cache updates, or connection readiness. They have been removed from quote reads, token admission,
execution ring operations, and reconciliation deduplication. Condition variables remain for parking
and prompt stop-token cancellation; replacing them with spin loops or `sleep_for` would increase CPU
use or shutdown latency.

## Next measurement-led stages

1. Add production histograms for ingress queue delay, gateway-lock wait/hold time, journal append and
   sync time, adapter acknowledgement latency, and end-to-end order state latency.
2. Evaluate group commit or an `io_uring` journal only if production histograms show disk throughput
   is limiting; preserve intent durability and mutation-to-WAL ordering.
3. Implement the crash-safe checkpoint/segment design in `docs/JOURNAL_COMPACTION.md` after its
   fault-injection matrix is automated.
4. Replace JSON on the internal execution path with a typed/binary representation while retaining
   JSON at REST and venue boundaries.
5. Move public top-of-book ingestion from one-second REST polling to venue WebSockets and benchmark
   end-to-end freshness separately from order-routing latency.

Use `std::chrono::steady_clock` for elapsed-time measurement. Do not use raw `RDTSC` for business or
venue timestamps; it is a cycle counter, not UTC, and requires architecture-specific serialization,
core migration handling, and calibration even for trustworthy microbenchmarks.
