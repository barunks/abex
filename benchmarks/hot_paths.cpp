#include "abex/application/spsc_execution_lane.hpp"
#include "abex/application/market_data_book.hpp"
#include "abex/application/order_gateway.hpp"
#include "abex/application/risk_manager.hpp"
#include "abex/domain/order_state_machine.hpp"
#include "abex/infrastructure/file_order_store.hpp"
#include "abex/infrastructure/market_data_ring.hpp"
#include "abex/infrastructure/simulated_exchange_adapter.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using Clock = std::chrono::steady_clock;

// ── percentile infrastructure ─────────────────────────────────────────────

struct LatencyStats {
    std::string name;
    std::int64_t p50{0};
    std::int64_t p95{0};
    std::int64_t p99{0};
    std::int64_t p999{0};
    std::int64_t min_ns{0};
    std::int64_t max_ns{0};
    std::int64_t mean_ns{0};
    std::size_t samples{0};
};

[[nodiscard]] LatencyStats compute_stats(std::string name,
                                         std::vector<std::int64_t>& samples) {
    if (samples.empty()) return {std::move(name)};
    std::sort(samples.begin(), samples.end());
    const auto n = samples.size();
    auto pct = [&](double p) -> std::int64_t {
        const auto idx = static_cast<std::size_t>(p / 100.0 * static_cast<double>(n));
        return samples[std::min(idx, n - 1)];
    };
    const auto sum = std::accumulate(samples.begin(), samples.end(), std::int64_t{0});
    return {
        .name    = std::move(name),
        .p50     = pct(50.0),
        .p95     = pct(95.0),
        .p99     = pct(99.0),
        .p999    = pct(99.9),
        .min_ns  = samples.front(),
        .max_ns  = samples.back(),
        .mean_ns = sum / static_cast<std::int64_t>(n),
        .samples = n,
    };
}

void print_stats(const LatencyStats& s) {
    std::cout << std::left << std::setw(52) << s.name
              << "  samples=" << std::setw(7) << s.samples
              << "  min="    << std::setw(9) << s.min_ns
              << "  p50="    << std::setw(9) << s.p50
              << "  p95="    << std::setw(9) << s.p95
              << "  p99="    << std::setw(9) << s.p99
              << "  p99.9="  << std::setw(9) << s.p999
              << "  max="    << std::setw(9) << s.max_ns
              << "  mean="   << s.mean_ns
              << "  (ns)\n";
}

// Run `fn()` once per sample, collect per-call nanoseconds.
template <typename Fn>
LatencyStats measure(std::string name, std::size_t warmup_count,
                     std::size_t sample_count, Fn&& fn) {
    for (std::size_t i = 0; i < warmup_count; ++i) fn();
    std::vector<std::int64_t> samples;
    samples.reserve(sample_count);
    for (std::size_t i = 0; i < sample_count; ++i) {
        const auto t0 = Clock::now();
        fn();
        samples.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
    }
    return compute_stats(std::move(name), samples);
}

// ── helpers ───────────────────────────────────────────────────────────────

abex::RiskManager risk_manager() {
    return abex::RiskManager({
        {"BTC-USDT",
         {.max_order_size = abex::Decimal::parse("100"),
          .max_notional   = abex::Decimal::parse("10000000"),
          .position_limit = abex::Decimal::parse("10000000")}},
        {"ETH-USDT",
         {.max_order_size = abex::Decimal::parse("1000"),
          .max_notional   = abex::Decimal::parse("10000000"),
          .position_limit = abex::Decimal::parse("10000000")}},
    });
}

abex::Order make_bench_order(std::size_t index) {
    const auto request = abex::OrderRequest{
        .client_order_id = "benchmark-order-" + std::to_string(index),
        .venue           = abex::Venue::Okx,
        .symbol          = "BTC-USDT",
        .side            = index % 2 == 0 ? abex::Side::Buy : abex::Side::Sell,
        .type            = abex::OrderType::Limit,
        .price           = abex::Decimal::parse("60000"),
        .quantity        = abex::Decimal::parse("0.001"),
        .time_in_force   = abex::TimeInForce::Gtc,
    };
    auto result = abex::make_order(request);
    result.status           = abex::OrderStatus::Live;
    result.pending_action   = abex::PendingAction::None;
    result.exchange_order_id = "exchange-" + std::to_string(index);
    result.processed_event_ids.insert("event-" + std::to_string(index));
    return result;
}

abex::SimulatedExchangeAdapter::Config unlimited_config(std::size_t n = 10'000'000) {
    return {.request_burst        = static_cast<double>(n),
            .requests_per_second  = 1'000'000.0};
}

// ── micro-benchmarks ──────────────────────────────────────────────────────

LatencyStats bench_market_lookup() {
    abex::MarketDataBook book;
    for (const auto venue : {abex::Venue::Okx, abex::Venue::Binance}) {
        for (const auto sym : {"BTC-USDT", "ETH-USDT"}) {
            book.publish({.venue           = venue,
                          .symbol          = sym,
                          .bid_price       = abex::Decimal::parse("59999"),
                          .ask_price       = abex::Decimal::parse("60000"),
                          .source_time_ms  = abex::unix_time_ms(),
                          .published_at_ms = abex::unix_time_ms(),
                          .sequence        = 1});
        }
    }
    std::size_t i = 0;
    std::int64_t sink = 0;
    return measure("market_lookup (seqlock, 4 slots)", 200'000, 2'000'000, [&] {
        const auto q = book.latest(
            i % 2 == 0 ? abex::Venue::Okx : abex::Venue::Binance,
            i % 4 < 2  ? "BTC-USDT" : "ETH-USDT");
        if (q) sink += q->bid_price.raw();
        ++i;
    });
    (void)sink;
}

LatencyStats bench_state_machine() {
    auto cur = make_bench_order(0);
    abex::ExecutionReport report{
        .client_order_id  = cur.client_order_id,
        .exchange_order_id = cur.exchange_order_id,
        .status           = abex::OrderStatus::Live,
        .cumulative_filled = {},
    };
    std::size_t seq = 1;
    return measure("state_machine_apply (allocation-free)", 100'000, 1'000'000, [&] {
        report.sequence = seq++;
        (void)abex::OrderStateMachine::apply(cur, report);
    });
}

LatencyStats bench_decimal_format() {
    constexpr auto value = abex::Decimal::literal("60000.12345678");
    std::array<char, abex::Decimal::maximum_formatted_size> buf{};
    std::size_t sink = 0;
    return measure("decimal_format_to (caller buffer)", 100'000, 1'000'000, [&] {
        sink += value.format_to(buf).size();
    });
    (void)sink;
}

LatencyStats bench_spsc_single_lane() {
    std::atomic<std::size_t> handled{0};
    abex::SpscExecutionLane lane(4096, [&](const abex::ExecutionReport&) { ++handled; });
    std::size_t i = 0;
    // measure per-submit cost (producer side only, consumer runs in its own thread)
    return measure("spsc_lane_submit (single producer)", 20'000, 200'000, [&] {
        abex::ExecutionReport r;
        r.event_id = "e" + std::to_string(i++);
        while (!lane.submit(std::move(r))) {}
    });
}

LatencyStats bench_ring_round_trip() {
    const auto path = std::filesystem::temp_directory_path() /
                      ("abex-ring-bm-" + std::to_string(::getpid()) + ".bin");
    std::filesystem::remove(path);
    abex::MarketDataRingWriter writer(path, 1024);
    abex::MarketDataRingReader reader(path);
    abex::MarketDataCursor cursor;
    std::array<abex::MarketQuote, 1> out;
    abex::MarketQuote quote{
        .venue           = abex::Venue::Okx,
        .symbol          = "BTC-USDT",
        .bid_price       = abex::Decimal::parse("59999"),
        .ask_price       = abex::Decimal::parse("60000"),
        .source_time_ms  = abex::unix_time_ms(),
        .published_at_ms = abex::unix_time_ms(),
    };
    auto stats = measure("mmap_ring_round_trip (write+read)", 10'000, 100'000, [&] {
        writer.publish(std::span<const abex::MarketQuote>(&quote, 1));
        if (reader.read_available(cursor, out) != 1) std::abort();
    });
    std::filesystem::remove(path);
    return stats;
}

LatencyStats bench_json_serialize() {
    const auto cur = make_bench_order(42);
    std::size_t sink = 0;
    return measure("order_json_serialize (nlohmann dump)", 2'000, 20'000, [&] {
        sink += nlohmann::json(cur).dump().size();
    });
    (void)sink;
}

LatencyStats bench_journal_nondurable() {
    const auto path = std::filesystem::temp_directory_path() /
                      ("abex-jnl-bm-" + std::to_string(::getpid()) + ".jsonl");
    std::filesystem::remove(path);
    abex::FileOrderStore store(path, false);
    std::size_t i = 0;
    auto stats = measure("journal_append_nondurable", 500, 5'000, [&] {
        store.append(make_bench_order(i++));
    });
    std::filesystem::remove(path);
    return stats;
}

LatencyStats bench_journal_durable() {
    const auto path = std::filesystem::temp_directory_path() /
                      ("abex-jnl-dur-" + std::to_string(::getpid()) + ".jsonl");
    std::filesystem::remove(path);
    abex::FileOrderStore store(path, true);
    std::size_t i = 0;
    auto stats = measure("journal_append_durable (fdatasync)", 10, 100, [&] {
        store.append(make_bench_order(i++));
    });
    std::filesystem::remove(path);
    return stats;
}

LatencyStats bench_risk_with_position() {
    const auto risk = risk_manager();
    const auto request = abex::OrderRequest{
        .client_order_id = "risk-bm",
        .venue           = abex::Venue::Okx,
        .symbol          = "BTC-USDT",
        .side            = abex::Side::Buy,
        .type            = abex::OrderType::Limit,
        .price           = abex::Decimal::parse("60000"),
        .quantity        = abex::Decimal::parse("0.001"),
        .time_in_force   = abex::TimeInForce::Gtc,
    };
    const abex::Decimal position = abex::Decimal::parse("1.5");
    return measure("risk_check_with_position (O(1) index path)", 10'000, 100'000, [&] {
        (void)risk.check_new_with_position(request, position);
    });
}

// ── gateway benchmarks ────────────────────────────────────────────────────

LatencyStats bench_gateway_place_single() {
    auto okx     = std::make_shared<abex::SimulatedExchangeAdapter>(abex::Venue::Okx,     unlimited_config());
    auto binance = std::make_shared<abex::SimulatedExchangeAdapter>(abex::Venue::Binance, unlimited_config());
    abex::OrderGateway gw({okx, binance}, risk_manager(),
                          std::make_shared<abex::MemoryOrderStore>(),
                          {.event_queue_capacity = 8192, .reconcile_on_start = false});
    gw.start();
    std::size_t i = 0;
    auto stats = measure("gateway_place_single_caller (non-durable)", 100, 1'000, [&] {
        abex::OrderRequest req{
            .client_order_id = "gw-single-" + std::to_string(i++),
            .venue           = i % 2 == 0 ? abex::Venue::Okx : abex::Venue::Binance,
            .symbol          = "BTC-USDT",
            .side            = abex::Side::Buy,
            .type            = abex::OrderType::Limit,
            .price           = abex::Decimal::parse("60000"),
            .quantity        = abex::Decimal::parse("0.001"),
            .time_in_force   = abex::TimeInForce::Gtc,
        };
        if (!gw.place(req).ok) std::abort();
    });
    gw.stop();
    return stats;
}

// Burst: 5000 orders, alternating venue/symbol/side — sustained single-caller throughput.
LatencyStats bench_gateway_burst() {
    constexpr std::size_t burst = 5'000;
    auto okx     = std::make_shared<abex::SimulatedExchangeAdapter>(abex::Venue::Okx,     unlimited_config(burst * 4));
    auto binance = std::make_shared<abex::SimulatedExchangeAdapter>(abex::Venue::Binance, unlimited_config(burst * 4));
    abex::OrderGateway gw({okx, binance}, risk_manager(),
                          std::make_shared<abex::MemoryOrderStore>(),
                          {.event_queue_capacity = 8192, .reconcile_on_start = false});
    gw.start();
    // warmup
    for (std::size_t i = 0; i < burst / 10; ++i) {
        (void)gw.place({.client_order_id = "warm-" + std::to_string(i),
                        .venue = abex::Venue::Okx, .symbol = "BTC-USDT",
                        .side = abex::Side::Buy, .type = abex::OrderType::Limit,
                        .price = abex::Decimal::parse("60000"),
                        .quantity = abex::Decimal::parse("0.001"),
                        .time_in_force = abex::TimeInForce::Gtc});
    }
    gw.flush_events();
    std::size_t i = 0;
    auto stats = measure("gateway_burst_5000_orders (mixed venue/symbol/side)", 0, burst, [&] {
        abex::OrderRequest req{
            .client_order_id = "burst-" + std::to_string(i),
            .venue           = i % 2 == 0 ? abex::Venue::Okx : abex::Venue::Binance,
            .symbol          = i % 4 < 2  ? "BTC-USDT" : "ETH-USDT",
            .side            = i % 3 == 0 ? abex::Side::Sell : abex::Side::Buy,
            .type            = abex::OrderType::Limit,
            .price           = abex::Decimal::parse("60000"),
            .quantity        = abex::Decimal::parse("0.001"),
            .time_in_force   = abex::TimeInForce::Gtc,
        };
        if (!gw.place(req).ok) std::abort();
        ++i;
    });
    gw.flush_events();
    gw.stop();
    return stats;
}

// Concurrent: 4 threads × 500 orders each — measures mutex_ contention under load.
LatencyStats bench_gateway_concurrent() {
    constexpr std::size_t per_thread   = 500;
    constexpr std::size_t thread_count = 4;
    constexpr std::size_t total        = per_thread * thread_count;
    auto okx     = std::make_shared<abex::SimulatedExchangeAdapter>(abex::Venue::Okx,     unlimited_config(total * 4));
    auto binance = std::make_shared<abex::SimulatedExchangeAdapter>(abex::Venue::Binance, unlimited_config(total * 4));
    abex::OrderGateway gw({okx, binance}, risk_manager(),
                          std::make_shared<abex::MemoryOrderStore>(),
                          {.event_queue_capacity = 8192, .reconcile_on_start = false});
    gw.start();

    // Collect per-order samples from all threads into a shared vector.
    std::vector<std::int64_t> all_samples(total);
    {
        std::vector<std::jthread> threads;
        for (std::size_t t = 0; t < thread_count; ++t) {
            threads.emplace_back([&, t] {
                for (std::size_t i = 0; i < per_thread; ++i) {
                    const auto id = t * per_thread + i;
                    abex::OrderRequest req{
                        .client_order_id = "conc-" + std::to_string(id),
                        .venue           = id % 2 == 0 ? abex::Venue::Okx : abex::Venue::Binance,
                        .symbol          = "BTC-USDT",
                        .side            = abex::Side::Buy,
                        .type            = abex::OrderType::Limit,
                        .price           = abex::Decimal::parse("60000"),
                        .quantity        = abex::Decimal::parse("0.001"),
                        .time_in_force   = abex::TimeInForce::Gtc,
                    };
                    const auto t0 = Clock::now();
                    if (!gw.place(req).ok) std::abort();
                    all_samples[id] = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          Clock::now() - t0).count();
                }
            });
        }
    }
    gw.flush_events();
    gw.stop();
    return compute_stats("gateway_concurrent_4t_2000_orders (mutex_ contention)", all_samples);
}

// Position read under write contention: 4 reader threads vs 200 writer placements.
LatencyStats bench_position_read_contention() {
    constexpr std::size_t write_count    = 200;
    constexpr std::size_t read_iters     = 10'000;
    constexpr std::size_t reader_threads = 4;
    auto okx     = std::make_shared<abex::SimulatedExchangeAdapter>(abex::Venue::Okx,     unlimited_config(write_count * 4));
    auto binance = std::make_shared<abex::SimulatedExchangeAdapter>(abex::Venue::Binance, unlimited_config(write_count * 4));
    abex::OrderGateway gw({okx, binance}, risk_manager(),
                          std::make_shared<abex::MemoryOrderStore>(),
                          {.event_queue_capacity = 4096, .reconcile_on_start = false});
    gw.start();

    std::atomic<bool> writing{true};
    std::vector<std::int64_t> read_samples;
    std::mutex read_mutex;

    std::vector<std::jthread> readers;
    for (std::size_t t = 0; t < reader_threads; ++t) {
        readers.emplace_back([&] {
            std::vector<std::int64_t> local;
            local.reserve(read_iters);
            while (writing.load(std::memory_order_acquire)) {
                const auto t0 = Clock::now();
                (void)gw.positions();
                local.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     Clock::now() - t0).count());
            }
            std::scoped_lock lk(read_mutex);
            read_samples.insert(read_samples.end(), local.begin(), local.end());
        });
    }

    for (std::size_t i = 0; i < write_count; ++i) {
        (void)gw.place({.client_order_id = "pos-" + std::to_string(i),
                        .venue    = i % 2 == 0 ? abex::Venue::Okx : abex::Venue::Binance,
                        .symbol   = i % 2 == 0 ? "BTC-USDT" : "ETH-USDT",
                        .side     = i % 3 == 0 ? abex::Side::Sell : abex::Side::Buy,
                        .type     = abex::OrderType::Limit,
                        .price    = abex::Decimal::parse("60000"),
                        .quantity = abex::Decimal::parse("0.001"),
                        .time_in_force = abex::TimeInForce::Gtc});
    }
    writing.store(false, std::memory_order_release);
    readers.clear();
    gw.stop();
    return compute_stats("positions()_under_write_contention (4 readers vs 200 writers)", read_samples);
}

// Observer notification: 8 COW observers, per-place cost of lock-free notify path.
LatencyStats bench_observer_notification() {
    constexpr std::size_t iters          = 5'000;
    constexpr std::size_t observer_count = 8;
    auto okx     = std::make_shared<abex::SimulatedExchangeAdapter>(abex::Venue::Okx,     unlimited_config(iters * 4));
    auto binance = std::make_shared<abex::SimulatedExchangeAdapter>(abex::Venue::Binance, unlimited_config(iters * 4));
    abex::OrderGateway gw({okx, binance}, risk_manager(),
                          std::make_shared<abex::MemoryOrderStore>(),
                          {.event_queue_capacity = 4096, .reconcile_on_start = false});
    std::atomic<std::size_t> notifications{0};
    std::vector<abex::OrderGateway::ObserverToken> tokens;
    tokens.reserve(observer_count);
    for (std::size_t i = 0; i < observer_count; ++i) {
        tokens.push_back(gw.add_order_observer([&](const abex::Order&) {
            notifications.fetch_add(1, std::memory_order_relaxed);
        }));
    }
    gw.start();
    // warmup
    for (std::size_t i = 0; i < iters / 10; ++i) {
        (void)gw.place({.client_order_id = "obs-w-" + std::to_string(i),
                        .venue = abex::Venue::Okx, .symbol = "BTC-USDT",
                        .side = abex::Side::Buy, .type = abex::OrderType::Limit,
                        .price = abex::Decimal::parse("60000"),
                        .quantity = abex::Decimal::parse("0.001"),
                        .time_in_force = abex::TimeInForce::Gtc});
    }
    gw.flush_events();
    notifications.store(0, std::memory_order_relaxed);
    std::size_t i = 0;
    auto stats = measure("observer_notify_8_observers (COW atomic load)", 0, iters, [&] {
        (void)gw.place({.client_order_id = "obs-" + std::to_string(i++),
                        .venue = abex::Venue::Okx, .symbol = "BTC-USDT",
                        .side = abex::Side::Buy, .type = abex::OrderType::Limit,
                        .price = abex::Decimal::parse("60000"),
                        .quantity = abex::Decimal::parse("0.001"),
                        .time_in_force = abex::TimeInForce::Gtc});
    });
    gw.flush_events();
    gw.stop();
    return stats;
}

// Idempotency replay: repeated identical create — fast path: mutex_ + hash lookup + return.
LatencyStats bench_idempotency_replay() {
    auto okx     = std::make_shared<abex::SimulatedExchangeAdapter>(abex::Venue::Okx,     unlimited_config(10));
    auto binance = std::make_shared<abex::SimulatedExchangeAdapter>(abex::Venue::Binance, unlimited_config(10));
    abex::OrderGateway gw({okx, binance}, risk_manager(),
                          std::make_shared<abex::MemoryOrderStore>(),
                          {.event_queue_capacity = 4096, .reconcile_on_start = false});
    gw.start();
    const abex::OrderRequest req{
        .client_order_id = "idem-order",
        .venue           = abex::Venue::Okx,
        .symbol          = "BTC-USDT",
        .side            = abex::Side::Buy,
        .type            = abex::OrderType::Limit,
        .price           = abex::Decimal::parse("60000"),
        .quantity        = abex::Decimal::parse("0.001"),
        .time_in_force   = abex::TimeInForce::Gtc,
    };
    if (!gw.place(req).ok) std::abort();
    gw.flush_events();
    auto stats = measure("idempotency_replay (mutex_ + hash lookup)", 10'000, 100'000, [&] {
        const auto r = gw.place(req);
        if (!r.idempotent_replay) std::abort();
    });
    gw.stop();
    return stats;
}

// Journal burst: 20 000 order appends, non-durable — sustained append throughput.
LatencyStats bench_journal_burst() {
    constexpr std::size_t iters = 20'000;
    const auto path = std::filesystem::temp_directory_path() /
                      ("abex-jnl-burst-" + std::to_string(::getpid()) + ".jsonl");
    std::filesystem::remove(path);
    abex::FileOrderStore store(path, false);
    std::size_t i = 0;
    auto stats = measure("journal_burst_20000_orders (non-durable)", 2'000, iters, [&] {
        store.append(make_bench_order(i++));
    });
    std::filesystem::remove(path);
    return stats;
}

// Execution lane burst: two producer threads × 200 000 events each.
LatencyStats bench_execution_lane_burst() {
    constexpr std::size_t per_venue = 200'000;
    std::atomic<std::size_t> handled{0};
    abex::SpscExecutionLane okx_lane(8192, [&](const abex::ExecutionReport&) {
        handled.fetch_add(1, std::memory_order_relaxed);
    });
    abex::SpscExecutionLane bnb_lane(8192, [&](const abex::ExecutionReport&) {
        handled.fetch_add(1, std::memory_order_relaxed);
    });
    std::vector<std::int64_t> samples(per_venue * 2);
    {
        std::jthread okx_prod([&] {
            for (std::size_t i = 0; i < per_venue; ++i) {
                abex::ExecutionReport r; r.event_id = "o" + std::to_string(i);
                const auto t0 = Clock::now();
                while (!okx_lane.submit(std::move(r), std::chrono::seconds{5})) {}
                samples[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 Clock::now() - t0).count();
            }
        });
        std::jthread bnb_prod([&] {
            for (std::size_t i = 0; i < per_venue; ++i) {
                abex::ExecutionReport r; r.event_id = "b" + std::to_string(i);
                const auto t0 = Clock::now();
                while (!bnb_lane.submit(std::move(r), std::chrono::seconds{5})) {}
                samples[per_venue + i] = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             Clock::now() - t0).count();
            }
        });
        okx_prod.join();
        bnb_prod.join();
        okx_lane.flush();
        bnb_lane.flush();
    }
    if (handled.load() != per_venue * 2) std::abort();
    return compute_stats("execution_lane_burst_400k_events (2 producers)", samples);
}

// Two-venue SPSC: original benchmark kept for comparison.
LatencyStats bench_two_venue_spsc() {
    constexpr std::size_t per_venue = 100'000;
    std::atomic<std::size_t> handled{0};
    abex::SpscExecutionLane okx(4096, [&](const abex::ExecutionReport&) { ++handled; });
    abex::SpscExecutionLane binance(4096, [&](const abex::ExecutionReport&) { ++handled; });
    std::vector<std::int64_t> samples(per_venue * 2);
    {
        std::jthread op([&] {
            for (std::size_t i = 0; i < per_venue; ++i) {
                const auto t0 = Clock::now();
                while (!okx.submit(abex::ExecutionReport{}, std::chrono::seconds{1})) {}
                samples[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 Clock::now() - t0).count();
            }
        });
        std::jthread bp([&] {
            for (std::size_t i = 0; i < per_venue; ++i) {
                const auto t0 = Clock::now();
                while (!binance.submit(abex::ExecutionReport{}, std::chrono::seconds{1})) {}
                samples[per_venue + i] = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             Clock::now() - t0).count();
            }
        });
        op.join(); bp.join();
        okx.flush(); binance.flush();
    }
    if (handled.load() != per_venue * 2) std::abort();
    return compute_stats("two_venue_spsc_200k_events (correct producer topology)", samples);
}

} // namespace

int main() {
    std::cout << "\n=== ABEX Benchmark Results ===\n";
    std::cout << std::string(120, '-') << '\n';
    std::cout << std::left
              << std::setw(52) << "Benchmark"
              << "  " << std::setw(14) << "samples"
              << std::setw(11) << "min"
              << std::setw(11) << "p50"
              << std::setw(11) << "p95"
              << std::setw(11) << "p99"
              << std::setw(11) << "p99.9"
              << std::setw(11) << "max"
              << "mean  (ns)\n";
    std::cout << std::string(120, '-') << '\n';

    // micro
    print_stats(bench_market_lookup());
    print_stats(bench_state_machine());
    print_stats(bench_decimal_format());
    print_stats(bench_risk_with_position());
    print_stats(bench_ring_round_trip());
    print_stats(bench_json_serialize());

    // journal
    print_stats(bench_journal_nondurable());
    print_stats(bench_journal_burst());
    print_stats(bench_journal_durable());

    // execution lanes
    print_stats(bench_spsc_single_lane());
    print_stats(bench_two_venue_spsc());
    print_stats(bench_execution_lane_burst());

    // gateway
    print_stats(bench_gateway_place_single());
    print_stats(bench_gateway_burst());
    print_stats(bench_gateway_concurrent());
    print_stats(bench_position_read_contention());
    print_stats(bench_observer_notification());
    print_stats(bench_idempotency_replay());

    std::cout << std::string(120, '-') << '\n';
    std::cout << "All benchmarks completed.\n";
}
