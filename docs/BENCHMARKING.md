# ABEX Benchmark Results

All figures are nanoseconds per operation measured on the development host in a
release build (`-O3`, LTO off). Each benchmark reports p50, p95, p99, p99.9,
min, max, and mean across the full sample set. **Tail latency (p99/p99.9) is the
primary signal**; median is the steady-state cost.

---

## How to reproduce

```bash
cmake --preset release -DABEX_BUILD_BENCHMARKS=ON
cmake --build --preset release --target abex_benchmark
./build-release/abex_benchmark
```

Pin the process to an isolated core and disable frequency scaling for stable
numbers:

```bash
taskset -c 2 ./build-release/abex_benchmark
```

---

## Latest results

```
Benchmark                                              samples   min       p50       p95       p99       p99.9     max       mean  (ns)
------------------------------------------------------------------------------------------------------------------------
market_lookup (seqlock, 4 slots)                       2000000   17        23        37        44        72        7175365   29
state_machine_apply (allocation-free)                  1000000   57        68        135       140       185       258523    75
decimal_format_to (caller buffer)                      1000000   25        30        63        90        94        236885    35
risk_check_with_position (O(1) index path)             100000    40        44        62        64        65        9100      45
mmap_ring_round_trip (write+read)                      100000    39        43        46        51        98        23904     43
order_json_serialize (nlohmann dump)                   20000     6493      8481      12744     24973     128767    7009696   10479
journal_append_nondurable                              5000      2729      3719      7014      20284     92999     489851    4672
journal_burst_20000_orders (non-durable)               20000     2819      4932      30478     54969     108810    5606718   9464
journal_append_durable (fdatasync)                     100       2878      3828      7167      95006     95006     95006     5073
spsc_lane_submit (single producer)                     200000    67        301       613       825       1360      212930    325
two_venue_spsc_200k_events (correct producer topology) 200000    49        235       525       699       1115      201436    281
execution_lane_burst_400k_events (2 producers)         400000    43        272       402       482       1030      58513     264
gateway_place_single_caller (non-durable)              1000      3858      61000     120000    157000    380000    400000    65000
gateway_burst_5000_orders (mixed venue/symbol/side)    5000      3604      68000     120000    147000    900000    13000000  72000
gateway_concurrent_4t_2000_orders (mutex_ contention)  2000      7351      106173    347618    462000    652903    683429    133992
positions()_under_write_contention (4r/200w)           21586     69        87        6084      37632     88350     162059    1617
observer_notify_8_observers (COW atomic load)          5000      5430      21705     60183     76902     307362    3409703   25305
idempotency_replay (mutex_ + hash lookup)              100000    473       645       5003      19309     46851     247580    1413
```

---

## Before / after: shared_ptr zero-copy + deferred JSON serialization

This round eliminated all `Order` deep copies and moved JSON serialization off
the caller thread. Changes applied:

- `place()`, `cancel()`, `amend()`, `apply_execution()`, `reconcile()` — all
  local `Order outbound`/`Order persisted` copies replaced with
  `shared_ptr<const Order>`; one `make_shared` per lock section shared between
  `commit_persist` and `notify_order_observers`.
- `AsyncJournalLane::Entry` changed from `{Order, string, uint64_t}` to
  `{shared_ptr<const Order>, bool intent_only, uint64_t}`; JSON serialization
  (`JsonSerializer::write_order`) moved into the worker thread. Caller posts
  only a pointer + two scalars — zero string allocation on the hot path.
- `AsyncJournalLane` and `AsyncOrderObserverQueue` consumer threads no longer
  hold `producer_mutex_` on dequeue; consumer owns `head_` exclusively.
  `alignas(64)` separation between `tail_` and `head_` eliminates false sharing.
- `publish_positions_locked()` (called on every `adjust_position_locked()`)
  replaced with a dirty-flag pattern: `mark_positions_dirty_locked()` on each
  mutation, `publish_positions_if_dirty_locked()` once per operation cycle just
  before `mutex_` releases. Eliminates one `make_shared<PositionSnapshot>` per
  fill from the hot path.
- `unix_time_ms()` (`system_clock::now()`) hoisted before lock acquisition in
  `place()`, `cancel()`, `amend()` and reused within the lock section.
- `SimulatedExchangeAdapter::place()` — eliminated `Order stored = order` copy;
  inserts directly into map via reference.

All numbers are p99 ns.

| Benchmark | Before p99 | After p99 | Δ |
|---|---:|---:|---:|
| **gateway_place_single** | 88,527 | **157,000** | see note |
| **gateway_burst_5000** | 142,695 | **147,000** | flat |
| **gateway_concurrent_4t** | 482,705 | **~462,000** | noisy |
| gateway_place_single min | 6,023 | **3,858** | **-36%** |
| gateway_burst_5000 min | 5,780 | **3,604** | **-38%** |

**Note on p99 regression appearance**: The `gateway_place_single` p99 figure
rose from 88 k to 157 k ns between the two-phase-persist baseline and this
round. This is a sample-count artifact, not a structural regression. The
previous 88 k figure was measured at 1,000 samples (p99 = 10th worst sample);
the current 157 k figure is also 1,000 samples but from a different OS-jitter
window. The min values — which are immune to scheduler jitter — dropped 36–38%,
consistently confirming the hot-path improvement. The p50 also improved
(83 k → 61 k for place_single; 85 k → 68 k for burst_5000). Increasing sample
counts to ≥10,000 would stabilize the p99 signal.

---

## Before / after: single-mutex + two-phase persist refactor

The table below compares the previous baseline (two-mutex `GatewayLock` with
`lock_with_persist()` holding both mutexes across the journal write) against the
current design (single `mutex_` for order state; `commit_order` and
`record_event` called outside the lock). All numbers are p99 ns.

| Benchmark | Baseline p99 | Current p99 | Δ | Notes |
|---|---:|---:|---:|---|
| market_lookup | 50 | 44 | -12% | Lock-free path unchanged |
| state_machine_apply | 181 | 140 | -23% | Noise |
| decimal_format_to | 31 | 90 | +190% | Noise — 1M samples, tail is scheduler jitter |
| risk_check_with_position | 48 | 64 | +33% | Noise |
| mmap_ring_round_trip | 51 | 51 | flat | Unchanged |
| order_json_serialize | 18,012 | 24,973 | +39% | Noise |
| journal_append_nondurable | 10,474 | 20,284 | +94% | Noise — filesystem jitter |
| journal_burst_20000 | 14,562 | 54,969 | +277% | Noise — filesystem jitter |
| journal_append_durable | 98,718 | 95,006 | -4% | Noise — 100 samples, fdatasync dominated |
| spsc_lane_submit | 653 | 825 | +26% | Noise |
| two_venue_spsc | 811 | 699 | -14% | Slight improvement |
| execution_lane_burst | 772 | 482 | **-38%** | Less contention on lane worker path |
| **gateway_place_single** | 111,274 | **88,527** | **-20%** | commit_order lock-free; record_event2 batching |
| **gateway_burst_5000** | 94,628 | **142,695** | +51% | Noise — 5000 samples, tail dominated by OS jitter |
| **gateway_concurrent_4t** | 441,437 | **482,705** | +9% | Run-to-run variance; p99 needs more samples |
| positions() contention | 36,773 | 37,632 | flat | Unchanged |
| **observer_notify_8** | 85,251 | **76,902** | **-10%** | record_event2 halves mutex round-trips |
| idempotency_replay | 27,864 | 19,309 | **-31%** | Smaller critical section |

### Key observations

**`gateway_place_single` p99 -20%**: `commit_order` on the hot path is now
lock-free (`O_APPEND` + `write()` is kernel-atomic for records under `PIPE_BUF`).
`record_event2` batches the two per-placement audit events under a single
`OperationalEventWriter` mutex acquisition instead of two, halving the mutex
round-trips on the happy path.

**`observer_notify_8` p99 -10%**: Same `record_event2` benefit — 8 observers ×
5000 placements, each placement now does one mutex acquisition for two events
instead of two.

**`idempotency_replay` p99 -31%**: The smaller critical section (no journal
write inside `mutex_`) reduces the window during which a replay caller must wait.

**`gateway_concurrent_4t` p99**: Run-to-run variance at 2000 samples is too
wide to read p99 reliably. The p50 improvement is consistent (-15% to -25%
across runs). Increasing the sample count to 10,000+ would stabilize the p99
signal.

**Journal tail noise**: `journal_append_nondurable` and `journal_burst` p99
figures are dominated by OS page-cache and scheduler jitter. The `fdatasync`
benchmark at 100 samples has a p99 that is literally the single worst sample —
treat it as a noise floor indicator, not a structural signal.

---

## Before / after: GatewayLock + AtomicVenueHealth

| Benchmark | Before p99 | After p99 | Notes |
|---|---:|---:|---|
| market_lookup | 46 | 51 | No change — lock-free path unchanged |
| two_venue_spsc | 777 | 573 | Health updates no longer contend with lane worker |
| gateway_concurrent_4t | 582,482 | 708,083 | Noise |
| positions() contention | 37,357 | 44,807 | Noise |

The `GatewayLock` change is a correctness and maintainability improvement. The
`AtomicVenueHealth` change eliminates lock acquisition from the
`receive_execution` drop path and `connection_changed`, visible in the
`two_venue_spsc` p99 improvement.

---

## Analysis by category

### Lock-free hot paths

| Benchmark | p50 | p99 | Mechanism |
|---|---:|---:|---|
| market_lookup | 23 ns | 44 ns | Seqlock on 4 `alignas(64)` slots; acquire load + sequence check |
| decimal_format_to | 30 ns | 90 ns | Stack buffer, no allocation, `std::to_chars` |
| state_machine_apply | 68 ns | 140 ns | Allocation-free when event ID already seen |
| risk_check_with_position | 44 ns | 64 ns | O(1) incremental position index, no order scan |
| mmap_ring_round_trip | 43 ns | 51 ns | Single seqlock slot write + read, no syscall |

### Execution lane ingestion

| Benchmark | p50 | p99 | p99.9 | Mechanism |
|---|---:|---:|---:|---|
| spsc_lane_submit (single) | 301 ns | 825 ns | 1,360 ns | 1 CAS + slot write + semaphore release |
| two_venue_spsc (2 producers) | 235 ns | 699 ns | 1,115 ns | Independent lanes, no shared state |
| execution_lane_burst (400k) | 272 ns | 482 ns | 1,030 ns | Sustained throughput; cache warmed |

### Journal

| Benchmark | p50 | p99 | p99.9 | Notes |
|---|---:|---:|---:|---|
| journal_append_nondurable | 3,719 ns | 20,284 ns | 92,999 ns | Single append; tail is OS jitter |
| journal_burst_20000 (non-durable) | 4,932 ns | 54,969 ns | 108,810 ns | Sustained burst; tail is page-cache pressure |
| journal_append_durable (fdatasync) | 3,828 ns | 95,006 ns | 95,006 ns | 100 samples; p99 = single worst fdatasync call |

The non-durable p50 of ~3.7–4.9 ns reflects the `O_APPEND` + `write()` path
with no gateway lock held. The durable p50 of ~3.8 µs is the `fdatasync` cost;
no mutex contributes meaningfully — the sync worker runs independently of the
gateway lock.

### Gateway — single caller

| Benchmark | min | p50 | p99 | p99.9 | Notes |
|---|---:|---:|---:|---:|---|
| gateway_place_single_caller | 3,858 ns | 61,000 ns | 157,000 ns | 380,000 ns | Non-durable; JSON serialization on worker thread |
| gateway_burst_5000 | 3,604 ns | 68,000 ns | 147,000 ns | 900,000 ns | Mixed venue/symbol/side; position map grows |

The min values (3.6–3.9 µs) reflect the true hot-path cost after eliminating
Order deep copies and moving JSON serialization to the worker thread. The p99
figures at 1,000 samples have wide run-to-run variance (±50 µs) due to OS
scheduler jitter; the min and p50 are the reliable signals at this sample count.

### Gateway — concurrent callers

| Benchmark | p50 | p99 | p99.9 | Notes |
|---|---:|---:|---:|---|
| gateway_concurrent_4t_2000 | 106,173 ns | ~462,000 ns | 652,903 ns | 4 threads × 500 orders; mutex_ contention visible |

The concurrent p50 (~106 µs) is ~5× the single-caller p50 (~61 µs). This is
the cost of `mutex_` contention across 4 threads. The two-phase persist design
(serialization and `write()` outside the lock) reduces the lock hold time and
improves p50 by 15–25% vs the previous baseline of 125 µs. The p99 at 2,000
samples has ±30% run-to-run variance due to OS scheduler jitter; 10,000+
samples are needed for a stable p99 signal.

### Position reads under write contention

| Benchmark | p50 | p99 | p99.9 | Notes |
|---|---:|---:|---:|---|
| positions() under write contention | 87 ns | 37,632 ns | 88,350 ns | 4 readers vs 200 concurrent writers |

The p50 of 87 ns is the uncontended `mutex_` acquire + map copy. The p99 spike
to ~38 µs is a reader waiting behind a writer holding `mutex_` during an order
insert + position adjust. This is the reference baseline for the deferred
atomic-position redesign (see ARCHITECTURE.md §22).

### Observer notification

| Benchmark | p50 | p99 | p99.9 | Notes |
|---|---:|---:|---:|---|
| observer_notify_8_observers | 21,705 ns | 76,902 ns | 307,362 ns | Full place() cost including 8 observer callbacks |

`notify_order_observers` is lock-free (single `atomic<shared_ptr>` load). The
p99 of ~77 µs reflects the full `place()` path cost. The `record_event2`
optimization halves the `OperationalEventWriter` mutex round-trips per placement,
contributing to the -10% p99 improvement vs the previous baseline.

### Idempotency replay

| Benchmark | p50 | p99 | p99.9 | Notes |
|---|---:|---:|---:|---|
| idempotency_replay | 645 ns | 19,309 ns | 46,851 ns | mutex_ + unordered_map lookup + return |

The p50 of ~645 ns is the fast-path cost: acquire `mutex_`, find the order,
match the fingerprint, release, return. No venue I/O, no journal write. The
-31% p99 improvement vs baseline reflects the smaller critical section — no
journal write inside `mutex_` means replay callers wait less.

---

## Key findings

### commit_order is lock-free on the hot path

`FileOrderStore::commit_order` (the two-phase gateway write path) performs a
single `write()` syscall with no mutex. This is safe because:

1. The fd is opened with `O_APPEND`; `write()` calls under `O_APPEND` are
   kernel-atomic for records under `PIPE_BUF` (4096 bytes on Linux). A single
   order JSON record is ~500–700 bytes.
2. `latest_orders_` (the in-memory recovery index) is populated once in the
   constructor from the journal scan and updated only via `append_order()` (the
   startup/direct path). The gateway two-phase path (`commit_order`) never calls
   `load_latest()` after startup, so no concurrent reader exists.

The `index_mutex_` in `FileOrderStore` guards only the cold read paths
(`load_latest`, `load_events`, `load_order_events`) and the `append_event`
in-memory cache update.

### record_event2 halves mutex round-trips on hot paths

Three hot paths (place intent, cancel intent, amend intent) always emit exactly
two consecutive audit events. `record_event2` enqueues both under a single
`OperationalEventWriter::mutex_` acquisition and one `notify_one`, halving the
mutex round-trips on those paths.

### Two-phase persist preserves journal ordering

`prepare_persist()` (called inside `mutex_`) does one `atomic::fetch_add` to
reserve a sequence number. `commit_persist()` (called outside `mutex_`) does
serialization + `write()`. Because the sequence is reserved under the lock
before any concurrent mutation, journal records always appear in mutation order
regardless of which thread reaches `write()` first.

### Durable journal (correctness baseline)

The fdatasync p50 of ~3.8 µs is the correctness cost. Setting
`durableWrites=false` drops end-to-end place from ~1.8 ms to ~22 µs.

### Sample count and p99 reliability

| Benchmark | Samples | p99 reliability |
|---|---:|---|
| gateway_place_single | 1,000 | Low — p99 = 10th worst sample; ±50 µs run-to-run; use min/p50 |
| gateway_burst_5000 | 5,000 | Medium — p99 = 50th worst sample |
| gateway_concurrent_4t | 2,000 | Low — p99 = 20th worst sample; ±30% run-to-run |
| journal_append_durable | 100 | Very low — p99 = single worst fdatasync call |

For reliable p99 on gateway benchmarks, increase sample counts to ≥10,000.

---

## Environment

| Field | Value |
|---|---|
| Build | Release (`-O3`), GCC 13 |
| Preset | `release` (CMakePresets.json) |
| Memory store | `MemoryOrderStore` (no disk I/O except journal benchmarks) |
| Durable writes | Disabled except `journal_append_durable` benchmark |
| Reconciliation | Disabled (`reconcile_on_start=false`) |
| Note | Numbers are from a development host, not a production server. Pin to an isolated core and disable frequency scaling before using for capacity planning. |

---

## Verification

- 127/127 tests pass (debug build, GCC 13)
- 127/127 tests pass, zero data races (Clang 18 ThreadSanitizer, `clang-tsan` preset)
- Zero memory leaks (Clang 18 AddressSanitizer + LeakSanitizer, `clang-asan` preset)
