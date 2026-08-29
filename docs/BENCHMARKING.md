# ABEX Benchmarking Guide

This document defines how ABEX performance is measured, reproduced, and interpreted. It is the
authoritative performance reference for the current implementation. Correctness guarantees are in
[ARCHITECTURE.md](ARCHITECTURE.md).

## Scope

The benchmark executable covers:

1. in-memory primitives: quote lookup, state validation, decimal parsing, and risk checks;
2. inter-thread transport through SPSC execution lanes;
3. journal append paths with background syncing disabled and enabled; and
4. composed gateway operations, including single-caller, burst, concurrent, observer, position,
   and idempotent-replay paths.

These are local component and gateway measurements. They exclude public-network latency, venue
matching-engine latency, TLS negotiation, and browser rendering.

## Reproduce the benchmark

From the repository root:

```bash
cmake --preset release -DABEX_BUILD_BENCHMARKS=ON
cmake --build --preset release --target abex_benchmark -j2
./build-release/abex_benchmark
```

For lower variance on Linux, select an otherwise idle physical core:

```bash
taskset -c 2 ./build-release/abex_benchmark
```

Record the commit, compiler, build options, CPU governor and affinity, operating system,
filesystem, and dirty-tree status. Never compare Debug and Release results.

## Measurement method

Each case warms up and then records individual operation durations with
`std::chrono::steady_clock`. The report includes minimum, p50, p95, p99, p99.9, maximum, and mean.
Percentiles are nearest-rank observations from the sorted sample set.

- p50 describes typical local execution.
- p95 and p99 expose scheduler, contention, allocator, and filesystem tails.
- maximum is diagnostic, not a service-level objective.
- minimum is a lower bound, not a capacity-planning value.
- means are outlier-sensitive and must be read with the percentiles.

Sub-microsecond cases approach timer and loop-overhead limits. Journal results are additionally
affected by the page cache, storage device, filesystem, and host scheduler.

## Latest baseline

Captured on 2026-08-29 from the current working tree based on commit `0fba683`.

| Environment | Value |
|---|---|
| Build | Release, GCC 13.4, optimization enabled |
| Host | WSL2 on Intel Core Ultra 7 155H |
| CPU affinity | Not pinned |
| Journal storage | Local WSL2 filesystem |
| Gateway mode | Simulated venues |
| Tests before measurement | 127/127 passing |

Selected p99 results:

| Benchmark | Samples | p99 | What is measured |
|---|---:|---:|---|
| Latest market quote lookup | high-volume | 31 ns | Four-slot seqlock read |
| Order state transition check | high-volume | 90 ns | State-machine validation |
| Decimal parsing | high-volume | 29 ns | Fixed-point input conversion |
| Risk check | high-volume | 48 ns | In-memory preflight path |
| mmap quote read | high-volume | 47 ns | Shared-memory snapshot read |
| JSON serialization | high-volume | 16,364 ns | Representative order payload |
| Journal append, background sync disabled | 5,000 | 14,835 ns | Serialized complete-record append |
| Journal burst, background sync disabled | 20,000 | 15,085 ns | Sustained append workload |
| Journal append, background sync enabled | 100 | 76,949 ns | Append and sync-worker notification |
| SPSC execution lane | high-volume | 514 ns | Producer-to-consumer handoff |
| Two-venue execution ingestion | high-volume | 598 ns | Independent lane ingestion |
| Burst execution lanes | high-volume | 666 ns | Contended burst ingestion |
| Gateway place, single caller | 1,000 | 92,117 ns | Simulated, background sync disabled |
| Gateway place, burst | workload-defined | 110,429 ns | Simulated burst submission |
| Gateway place, concurrent callers | workload-defined | 268,103 ns | Multi-producer contention |
| Position read under writes | workload-defined | 2,440 ns | Concurrent position snapshot |
| Observer notification | workload-defined | 83,069 ns | Order update fan-out |
| Idempotent request replay | workload-defined | 2,243 ns | Cached result lookup |

Retain the complete raw distribution printed by `abex_benchmark` with submission or CI artifacts.
This table is a concise baseline, not a substitute for raw output.

## Durability measurement semantics

ABEX appends a complete checksummed journal record before venue I/O. With
`journal.durableWrites=true`, the append signals a dedicated worker that coalesces `fdatasync`
requests. The caller does **not** wait for `fdatasync`.

Therefore, `journal_append_durable (background fdatasync)` measures the command-path append plus
worker notification. Store destruction drains the worker after sampling, so storage-sync time is
outside each recorded operation. The label means background syncing is enabled; it does not mean
each observation is a synchronous stable-storage commit.

This provides append-before-route ordering and bounded asynchronous persistence. It does not claim
stable-storage-before-route semantics. A process or machine failure inside the outstanding sync
window can lose recently appended records even if venue routing has occurred.

The background-sync case has only 100 observations. Its p99 is the worst sample and is
statistically weak; use at least 10,000 samples for a dependable tail comparison.

## Interpretation

### Lock-free reads

Quote lookup, mmap reads, and fixed-point parsing remain below one microsecond. These paths avoid
heap allocation and filesystem or network I/O. They validate local data-layout and synchronization
choices; they are not end-to-end order latency.

### Journal

The non-sync append p99 of 14.8 microseconds reflects serialization, the serialized write boundary,
syscall cost, and scheduler/filesystem jitter. The background-sync row has a higher tail because
the worker and caller share synchronization and filesystem resources, even though the caller does
not synchronously wait for `fdatasync`.

Use a representative filesystem. Results from tmpfs, container overlays, native Linux, WSL2, and
network-mounted storage are not interchangeable.

### Gateway placement

The single-caller p99 is 92.1 microseconds with simulated venues and background syncing disabled.
Concurrent p99 rises to 268.1 microseconds as producers contend for shared order state, journal
serialization, allocation, and execution-lane work. Neither result predicts live venue latency.

### Replay and reads

Idempotent replay and position reads are intentionally cheaper than new placement. Replays return
the recorded result without a second venue mutation; position reads use in-memory snapshots.

## Responsible comparisons

For before/after claims:

1. build both revisions with the same compiler, preset, and CMake options;
2. use the same machine, affinity, power profile, filesystem, and journal location;
3. run each revision at least five times in alternating order;
4. retain raw output, not only one percentile;
5. compare medians of run-level p50 and p99 values; and
6. disclose correctness or workload changes that invalidate direct comparison.

Do not mix historical implementation numbers into the current table. Synchronous-`fdatasync` and
background-sync measurements describe different semantics.

## Suggested regression gates

Establish hard gates from repeated runs on a dedicated CI runner. Until then, treat these as review
triggers:

- any lock-free primitive p99 regression greater than 25%;
- journal or gateway p50 regression greater than 20% across five paired runs;
- journal or gateway p99 regression greater than 35% across five paired runs;
- throughput loss, queue saturation, or dropped reports under the standard burst; or
- any correctness, ordering, idempotency, or state-machine regression.

Correctness takes precedence over a faster number. Changes must preserve the guarantees in
[ARCHITECTURE.md](ARCHITECTURE.md).

## Verification before publication

```bash
cmake --preset debug
cmake --build --preset debug -j2
ctest --preset debug --output-on-failure
git status --short
```

Attach the commit and dirty-tree status, exact commands, raw output, hardware, operating system,
compiler, filesystem, and any affinity, governor, virtualization, or concurrent-load limitations.

## Known limitations

- The current baseline was not CPU-pinned and was collected under WSL2.
- Small sample sets do not support strong p99 or p99.9 conclusions.
- Simulated adapters remove public-network and exchange variability.
- Microbenchmarks do not establish production capacity by themselves.
- The harness does not yet model prolonged venue outage, saturated client WebSockets, or multi-hour
  journal growth; those belong in load and soak tests.
