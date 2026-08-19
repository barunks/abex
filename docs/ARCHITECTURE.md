# Architecture and engineering decisions

## Dependency direction

ABEX follows a ports-and-adapters design. Dependencies always point inward:

```text
Browser UI -- REST / client WebSocket --\
                                         OrderGateway
CLI ------------------------------------/    /   |   \
                                          Risk Journal Exchange port
                                                          /        \
                                                     OKX adapter  Binance adapter
                                                          \        /
                                                     HTTP / WebSocket transport

                           abex_server (primary OMS)
                         /       |          |          \
                        v        v          v           v
                 Exchange GW  Journal   REST/WS/UI   supervised child
                                                       |
OKX/Binance public REST -- abex_market_data -- mmap ring file -- Gateway reader
```

The domain layer knows nothing about JSON, files, sockets, threads, or exchanges. `OrderGateway`
coordinates use cases through `IExchangeAdapter` and `IOrderStore`. This keeps each class focused:
exchange adapters translate protocols, the risk manager makes risk decisions, the state machine
owns lifecycle rules, and each presentation adapter only parses and presents use cases. A shared
`GatewayRuntime` is the composition root used by both executables.
Live and simulated exchange adapters are compiled into the same executable. `--mode
live|simulation` selects them at startup, with `live` as the default; trading mode never depends on
preprocessor state.

## Client-facing APIs

`GatewayApi` is independent of sockets, which makes routing and status mapping deterministic and
unit-testable. `HttpServer` adapts Boost.Beast requests to it and serves only a fixed whitelist of
UI assets. Unknown order fields are rejected, preventing exchange-specific concepts from leaking
through the client boundary. The OpenAPI 3.1 document is available at `/api/v1/openapi.json`.

The WebSocket at `/ws/v1/orders` starts with `orders.snapshot`, `market.snapshot`, and
`system.snapshot`, then sends `order.updated`, `market.updated`, and `system.event`. Order commands
always use REST; this client WebSocket is a server-to-browser update channel, not an exchange
connection. Each connection has a 256-message outbound bound. A slow consumer loses intermediate
updates and receives `resync.required`; it then reloads the authoritative REST snapshots. A slow
socket never blocks exchange-state processing.

`GET /api/v1/system` makes the boundaries inspectable: REST commands and recovery snapshots,
client WebSocket updates, mmap market ingress, journal durability/location/sequence, instance and
recovery counters, and recent durable operational events. The browser presents the same model and
switches its transport badge to REST fallback when its WebSocket is unavailable.

## Market-data process boundary

`abex_server` is the primary OMS/gateway/UI process. By default it forks/execs the sibling
`abex_market_data` as a separate child and waits on a one-byte readiness channel for its first
successful quote. The server terminates and reaps that child on shutdown and fails visibly if it
dies unexpectedly. `--no-market-data` disables supervision for systemd, containers, or manual
multi-process operation.

The child remains an unauthenticated publisher with no OMS, exchange-order, HTTP, or UI ownership.
Every second it fetches top-of-book bid/ask quotes for BTC-USDT and ETH-USDT from both venues. OKX
and Binance are fetched concurrently so one slow venue does not delay the other.

The publisher is the only writer to a fixed-layout POSIX memory-mapped ring file. An advisory file
lock prevents a second writer. Each slot is copied first and committed with a release-store sequence;
the reader uses acquire loads and a second sequence check to reject torn or overwritten slots. A
generation value detects publisher restarts, and a lagging reader resumes at the oldest record still
retained by the configured capacity.

`GatewayRuntime` tails this file without making market-data network requests. It builds the current
in-memory book, exposes it at `/api/v1/market-data`, and emits it over the client WebSocket. The UI
shows venue bids/asks plus the lowest ask as best buy and highest bid as best sell. A five-second
default maximum age makes stale data non-executable. The header also displays the runtime mode so
the live-default process is visually distinct from a simulation instance.

## Common order schema

The client model contains `clientOrderId`, venue, canonical `BASE-QUOTE` symbol, side, type,
optional limit price, base-asset quantity, and time-in-force. All monetary fields are `Decimal`,
a signed fixed-point integer scaled to eight places.

Internally an order also records current and historical physical exchange identifiers,
per-generation fill/quote offsets, filled and quote quantities, average fill price, authoritative
and pending amend terms, a monotonic local version, the last meaningful venue sequence, processed
event IDs, and request fingerprints. Internal bookkeeping is persisted but excluded from client
responses.

## Explicit venue mapping

| Common concept | OKX | Binance Spot |
|---|---|---|
| Symbol | `BTC-USDT` | `BTCUSDT` |
| Client ID | `clOrdId` | `newClientOrderId` |
| Limit/GTC | `ordType=limit` | `type=LIMIT,timeInForce=GTC` |
| Limit/IOC | `ordType=ioc` | `type=LIMIT,timeInForce=IOC` |
| Market buy quantity | `sz` with `tgtCcy=base_ccy` | `quantity` |
| Cancel | REST `/trade/cancel-order` | WS `order.cancel` |
| Amend | REST `/trade/amend-order` | WS `order.cancelReplace` |
| Updates | private `orders` channel | signed user-data `executionReport` |

Binance quantity-reduction can use `order.amend.keepPriority`, but the common amend contract also
allows price changes and increases. ABEX therefore uses cancel-replace consistently. A generated
exchange-client alias is correlated back to the stable client ID and persisted. This sacrifices
queue priority but gives deterministic cross-venue semantics.

## State and race rules

Public states are `LIVE`, `PARTIALLY_FILLED`, `FILLED`, `CANCELED`, `REJECTED`, and operational
`UNKNOWN`. `pendingAction` distinguishes an accepted local intent from venue confirmation.

- A duplicate `eventId` is a no-op.
- Cumulative filled quantity never decreases.
- A lower sequence may contribute previously unseen fill quantity, but cannot overwrite a newer
  lifecycle decision.
- `FILLED` is final and cannot regress to canceled or live.
- A full cumulative fill wins a cancel/fill race.
- A superseded Binance generation may add a late fill, but its `CANCELED` state cannot cancel the
  active replacement. Physical cumulative quantities are translated through persisted offsets.
- An amend acknowledgement never changes visible price/quantity by itself. OKX terms change only
  when reported by the order stream/query; Binance terms change from the compound replacement
  response and subsequent execution reports.
- A terminal canceled/rejected order may retain later-discovered partial execution, but its
  terminal state remains unless the full quantity is proven filled.
- The create intent is journaled before exchange I/O. This makes an execution report that beats
  the acknowledgement correlatable by `clientOrderId`.
- An acknowledgement timeout is not treated as rejection; the order becomes `UNKNOWN` and requires
  reconciliation.

## Idempotency

Create idempotency is keyed by `clientOrderId` and a canonical request fingerprint. An exact retry
returns the existing order; reusing the ID with different fields returns
`IDEMPOTENCY_CONFLICT`.

Cancel and amend accept a `requestId`. The order snapshot persists each request ID and fingerprint.
The same ID and payload is replayed without a second venue call, while reuse with a different
payload is rejected. The CLI generates deterministic defaults, but callers should provide explicit
IDs across process and network retries.

## Risk

Checks run before the create intent reaches an adapter:

1. schema and positive-value checks;
2. a fresh venue ask/bid for market-order notional and funding calculations;
3. configured per-instrument maximum quantity and notional, using the limit/executable price;
4. current venue instrument status, minimum/maximum quantity, quantity step, price range/tick, and
   minimum/maximum notional; rules are read-only and cached for at most 30 seconds;
5. absolute position limit using a conservative full-fill view of every open order plus realized
   fills on terminal orders;
6. authoritative venue available balance: BUY requires quote currency at the limit/current ask,
   while SELL requires base currency.

When the runtime has no fresh mapped venue quote, a market order fails closed with
`MARKET_DATA_UNAVAILABLE`. In simulation, market orders fill at the current ask/bid, while limit
orders fill only when the mapped ask/bid crosses their limit. Live venues still determine the real
execution price; the mapped quote is the pre-trade risk and operator reference.

Balance snapshots normalize total, available, frozen, and order-frozen values. OKX also exposes
`uid`/`mainUid` so operators can verify the exact credential-owned account. The UI requests only
the funding currency for its selected venue/instrument/side. `GET /api/v1/balances` requires one
of `BTC`, `ETH`, or `USDT`; the CLI derives that currency from `--symbol` and `--side` and can
provide conservative quantity guidance when given `--price`. These queries are read-only. A query failure rejects an order closed with
`BALANCE_UNAVAILABLE`; a shortfall is durably rejected before venue I/O with
`INSUFFICIENT_AVAILABLE_BALANCE` and a specific available/required/frozen explanation.

OKX rules come from the credential-aware account-instruments endpoint; Binance rules come from
WebSocket `exchangeInfo`. The order ticket combines those rules, the selected executable price,
and available balance into a routeable quantity range. Invalid inputs disable **Route order** and
the gateway independently persists a local rejection without calling the venue order endpoint.

The position model is intentionally approximate and nets buys against sells. Balance preflight is
authoritative at query time but cannot atomically reserve venue funds; the venue remains final
authority under concurrent account activity and price movement. A production service would add
account-scoped reservations and fee-aware buffers.

## Persistence and recovery

The OMS journal stores typed complete order snapshots and operational audit events in one
JSON-lines stream. Every record has a schema version, monotonic record number, timestamp, and
FNV-1a corruption checksum. Permissions are owner read/write; durable mode calls `fdatasync` before
acknowledging the local transition. Order intent and its request identity are durable before venue
I/O. A retained advisory lock permits only one process to own the journal, preventing split-brain
writers. Recovery keeps the highest snapshot for each client ID and reloads the recent audit
timeline. A malformed last record is treated as a torn append; corruption before the final record
is fatal.

On startup the gateway reloads snapshots, gives recovered aliases to adapters, reconnects private
streams, and enumerates open orders. OKX uses `/api/v5/trade/orders-pending`; Binance uses
`openOrders.status`. The durable journal is the ownership boundary: account-wide venue orders that
cannot be correlated with it are ignored rather than adopted, canceled, or reported as failures.
The same rule applies to unrelated updates on account-wide private order streams. Journaled
non-terminal orders absent from the snapshot are queried individually. Missing/failed evidence
leaves an owned order `UNKNOWN`, never falsely canceled. The same process runs after reconnect and
on a 30-second configurable interval.

Operational records cover process starts/restarts, durable intents, acknowledgements, unknown
outcomes, idempotent retries/conflicts, disconnects/reconnects, sequence gaps, backpressure, and
reconciliation. Each carries a process instance ID and optional venue/order/request identity. They
survive restart, are returned by `/api/v1/system`, and are streamed to the UI. Operational logging
failures are counted and alerted without replacing the primary OMS outcome.

## Resilience and backpressure

The TLS WebSocket transport verifies hostnames, answers WebSocket control frames through Beast,
serializes writes, reconnects with bounded exponential backoff, and replays subscriptions after
each connection. The application sees a venue as connected only after private authentication or
user-data subscription succeeds.

Execution ingestion uses a bounded queue. Producers wait briefly when it is full; persistent
pressure increments `droppedEvents` and forces reconciliation. Local token buckets fail fast before
known order budgets are exceeded. Binance's returned `REQUEST_WEIGHT` counters continuously
synchronize its bucket. OKX uses the documented conservative command budget and treats HTTP 429 as
an explicit failure.

Client WebSocket output is independently bounded. It preserves the buffer owned by an active
asynchronous write, drops excess intermediate updates, and emits one resynchronization instruction
after the write completes. REST remains the authoritative recovery path.

Sequence numbers are tracked when an adapter can supply a contiguous sequence. OKX order update
timestamps and Binance execution IDs are identifiers, not guaranteed contiguous sequences, so
live adapters deliberately do not invent sequence semantics. Gaps from sequencing-capable feeds or
the simulator degrade venue health and demand reconciliation.

## Latency trade-offs

- Durable `fdatasync` is correctness-first and expensive. Low-latency deployments can set
  `durableWrites=false`, batch WAL syncs, or replace the store port with a dedicated journal.
- Durable domain and asynchronous values retain owned strings. Static enum names and synchronous
  lookups borrow `string_view`, risk checks avoid order-book snapshots, the execution queue is
  preallocated, and HTTP handles reuse connections. JSON venue/REST boundaries remain a future
  SBE/FIX/binary-adapter opportunity. See `docs/PERFORMANCE.md` for measurements.
- Network clients and the HTTP/WebSocket server are asynchronous/persistent, while application use
  cases wait synchronously for bounded exchange acknowledgements. A future API version could expose
  asynchronous acceptance plus operation resources.
- Locks favor simple deterministic ordering. Sharding by account/instrument is the natural scaling
  step.

## Known limitations

- The local journal grows indefinitely; add snapshot compaction after proving safe crash recovery.
- There is no client authentication/RBAC, TLS terminates nowhere in-process, and CORS is permissive.
  The safe default is a loopback bind; a production edge must provide TLS, authorization, request
  limits, and audit identity.
- Public top-of-book data is polled once per second rather than consumed from venue market-data
  WebSockets; there is no metrics exporter.
- Live behavior depends on venue account configuration, balances, symbol filters, and API changes.
- Binance cancel-replace is not transactional. ABEX parses cancel and replacement legs separately;
  if cancel succeeds and replacement fails the canonical order becomes canceled and the client sees
  the nested replacement code/message. A fill racing the compound request can make the actual total
  exceed the requested target; ABEX reports authoritative quantity and emits the critical
  `REPLACEMENT_QUANTITY_DRIFT` audit event rather than hiding it.
- Secrets reside in process memory. Production should use short-lived credentials, locked memory,
  a secret manager/HSM, IP allow-listing, and separate trade/read keys.

The browser console is an operational demonstration, not a production trading terminal. It has no
account separation, approvals, entitlement model, or venue-specific capability discovery.
