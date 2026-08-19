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
- The execution dispatcher is a fixed-capacity, preallocated ring. After construction, queue nodes
  do not allocate. Its condition variables remain because blocking backpressure is preferable to
  burning a CPU core with polling.
- The journal reuses its exclusively locked descriptor, serializes each payload once, and builds
  the record around those bytes. It no longer opens/closes the file or walks the payload twice for
  every append. `fdatasync` remains enabled when durable writes are configured.
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
| Risk check with 10,000 existing orders | 2,664,747 ns | 36,659 ns | 72x faster |
| Latest market quote lookup | 40 ns | 14 ns | 65.0% lower |
| Bounded dispatcher event | 662 ns | 622 ns | 6.0% lower; scheduler-sensitive |
| Nondurable journal append | 9,962 ns | 3,371 ns | 66.2% lower |

These are microbenchmarks, not exchange round-trip promises. Run them on the deployment host and
pin/load-isolate the process before using the numbers for capacity planning.

## Synchronization policy

Mutexes are retained where they protect multi-field invariants, callback registration, libcurl
handle ownership, or token-bucket accounting. Condition variables are retained for queue capacity,
flush completion, connection readiness, and reconciliation scheduling. Replacing these with spin
loops would generally increase tail latency under load.

The next structural contention target is the gateway-wide order mutex: durable journal I/O is still
performed while it protects transition ordering. Removing that wait safely requires an ordered WAL
writer plus per-order operation sequencing; merely unlocking around `fdatasync` can reorder snapshots
and corrupt restart semantics.

## Next measurement-led stages

1. Add production histograms for ingress queue delay, gateway-lock wait/hold time, journal append and
   sync time, adapter acknowledgement latency, and end-to-end order state latency.
2. Introduce an ordered WAL writer with per-order sequencing, then shard mutable order state by
   client order ID. Preserve intent durability before venue I/O.
3. Bound or compact persisted event-id history without weakening duplicate-event behavior across
   restart, and compact the append-only journal from a crash-safe checkpoint.
4. Replace JSON on the internal execution path with a typed/binary representation while retaining
   JSON at REST and venue boundaries.
5. Move public top-of-book ingestion from one-second REST polling to venue WebSockets and benchmark
   end-to-end freshness separately from order-routing latency.

Use `std::chrono::steady_clock` for elapsed-time measurement. Do not use raw `RDTSC` for business or
venue timestamps; it is a cycle counter, not UTC, and requires architecture-specific serialization,
core migration handling, and calibration even for trustworthy microbenchmarks.
