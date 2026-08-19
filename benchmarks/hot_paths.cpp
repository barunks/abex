#include "abex/application/spsc_execution_lane.hpp"
#include "abex/application/market_data_book.hpp"
#include "abex/application/order_gateway.hpp"
#include "abex/application/risk_manager.hpp"
#include "abex/domain/order_state_machine.hpp"
#include "abex/infrastructure/file_order_store.hpp"
#include "abex/infrastructure/market_data_ring.hpp"
#include "abex/infrastructure/simulated_exchange_adapter.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t warmup_divisor = 10;

template <typename Operation>
void warmup(std::size_t measured_operations, Operation&& operation) {
    for (std::size_t index = 0;
         index < std::max<std::size_t>(1, measured_operations / warmup_divisor); ++index) {
        operation(index);
    }
}

template <typename Operation>
std::int64_t nanoseconds_per_operation(std::size_t operations, Operation&& operation) {
    const auto started = Clock::now();
    operation();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count();
    return elapsed / static_cast<std::int64_t>(operations);
}

abex::RiskManager risk_manager() {
    return abex::RiskManager({
        {"BTC-USDT",
         {.max_order_size = abex::Decimal::parse("10"),
          .max_notional = abex::Decimal::parse("1000000"),
          .position_limit = abex::Decimal::parse("1000000")}},
    });
}

abex::Order order(std::size_t index) {
    const auto request = abex::OrderRequest{
        .client_order_id = "benchmark-order-" + std::to_string(index),
        .venue = abex::Venue::Okx,
        .symbol = "BTC-USDT",
        .side = index % 2 == 0 ? abex::Side::Buy : abex::Side::Sell,
        .type = abex::OrderType::Limit,
        .price = abex::Decimal::parse("60000"),
        .quantity = abex::Decimal::parse("0.001"),
        .time_in_force = abex::TimeInForce::Gtc,
    };
    auto result = abex::make_order(request);
    result.status = abex::OrderStatus::Live;
    result.pending_action = abex::PendingAction::None;
    result.exchange_order_id = "exchange-" + std::to_string(index);
    result.processed_event_ids.insert("event-" + std::to_string(index));
    return result;
}

void benchmark_risk_snapshot() {
    constexpr std::size_t order_count = 10'000;
    constexpr std::size_t iterations = 100;
    std::vector<abex::Order> orders;
    orders.reserve(order_count);
    for (std::size_t index = 0; index < order_count; ++index) orders.push_back(order(index));
    const auto request = abex::OrderRequest{
        .client_order_id = "benchmark-new",
        .venue = abex::Venue::Okx,
        .symbol = "BTC-USDT",
        .side = abex::Side::Buy,
        .type = abex::OrderType::Limit,
        .price = abex::Decimal::parse("60000"),
        .quantity = abex::Decimal::parse("0.001"),
        .time_in_force = abex::TimeInForce::Gtc,
    };
    const auto risk = risk_manager();
    std::atomic<std::size_t> accepted{0};
    const auto elapsed = nanoseconds_per_operation(iterations, [&] {
        for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
            const auto snapshot = orders;
            if (risk.check_new(request, snapshot).accepted) ++accepted;
        }
    });
    std::cout << "risk_snapshot_10000_orders_ns_per_check=" << elapsed << '\n';
    if (accepted.load() != iterations) std::abort();

    accepted.store(0);
    const auto no_copy_elapsed = nanoseconds_per_operation(iterations, [&] {
        for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
            abex::Decimal position;
            for (const auto& current : orders) {
                const auto exposure = abex::is_terminal(current.status)
                                          ? current.filled_quantity
                                          : current.quantity;
                position += current.side == abex::Side::Buy ? exposure : -exposure;
            }
            if (risk.check_new_with_position(request, position).accepted) ++accepted;
        }
    });
    std::cout << "risk_no_copy_10000_orders_ns_per_check=" << no_copy_elapsed << '\n';
    if (accepted.load() != iterations) std::abort();
}

void benchmark_market_lookup() {
    constexpr std::size_t iterations = 2'000'000;
    abex::MarketDataBook book;
    for (const auto venue : {abex::Venue::Okx, abex::Venue::Binance}) {
        for (const auto symbol : {"BTC-USDT", "ETH-USDT"}) {
            book.publish({.venue = venue,
                          .symbol = symbol,
                          .bid_price = abex::Decimal::parse("59999"),
                          .ask_price = abex::Decimal::parse("60000"),
                          .source_time_ms = abex::unix_time_ms(),
                          .published_at_ms = abex::unix_time_ms(),
                          .sequence = 1});
        }
    }
    warmup(iterations, [&](std::size_t iteration) {
        (void)book.latest(iteration % 2 == 0 ? abex::Venue::Okx : abex::Venue::Binance,
                          iteration % 4 < 2 ? "BTC-USDT" : "ETH-USDT");
    });
    std::int64_t sum = 0;
    const auto elapsed = nanoseconds_per_operation(iterations, [&] {
        for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
            const auto quote = book.latest(
                iteration % 2 == 0 ? abex::Venue::Okx : abex::Venue::Binance,
                iteration % 4 < 2 ? "BTC-USDT" : "ETH-USDT");
            if (quote) sum += quote->bid_price.raw();
        }
    });
    std::cout << "market_lookup_ns_per_call=" << elapsed << '\n';
    if (sum == 0) std::abort();
}

void benchmark_dispatcher() {
    constexpr std::size_t iterations = 200'000;
    std::atomic<std::size_t> handled{0};
    abex::SpscExecutionLane lane(
        4096, [&](const abex::ExecutionReport&) { ++handled; });
    const auto elapsed = nanoseconds_per_operation(iterations, [&] {
        for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
            abex::ExecutionReport report;
            report.event_id = "event-" + std::to_string(iteration);
            while (!lane.submit(std::move(report))) {
            }
        }
        lane.flush();
    });
    std::cout << "dispatcher_ns_per_event=" << elapsed << '\n';
    if (handled.load() != iterations) std::abort();
}

void benchmark_two_venue_dispatch() {
    constexpr std::size_t per_venue = 100'000;
    std::atomic<std::size_t> handled{0};
    abex::SpscExecutionLane okx(4096, [&](const abex::ExecutionReport&) { ++handled; });
    abex::SpscExecutionLane binance(4096, [&](const abex::ExecutionReport&) { ++handled; });
    const auto elapsed = nanoseconds_per_operation(per_venue * 2, [&] {
        std::jthread okx_producer([&] {
            for (std::size_t index = 0; index < per_venue; ++index) {
                while (!okx.submit(abex::ExecutionReport{}, std::chrono::seconds{1})) {
                }
            }
        });
        std::jthread binance_producer([&] {
            for (std::size_t index = 0; index < per_venue; ++index) {
                while (!binance.submit(abex::ExecutionReport{}, std::chrono::seconds{1})) {
                }
            }
        });
        okx_producer.join();
        binance_producer.join();
        okx.flush();
        binance.flush();
    });
    std::cout << "two_venue_spsc_ns_per_event=" << elapsed << '\n';
    if (handled.load() != per_venue * 2) std::abort();
}

void benchmark_state_machine() {
    constexpr std::size_t iterations = 1'000'000;
    auto current = order(0);
    abex::ExecutionReport report{
        .client_order_id = current.client_order_id,
        .exchange_order_id = current.exchange_order_id,
        .status = abex::OrderStatus::Live,
        .cumulative_filled = {},
    };
    warmup(iterations, [&](std::size_t index) {
        report.sequence = static_cast<std::uint64_t>(index + 1);
        (void)abex::OrderStateMachine::apply(current, report);
    });
    const auto elapsed = nanoseconds_per_operation(iterations, [&] {
        for (std::size_t index = 0; index < iterations; ++index) {
            report.sequence = static_cast<std::uint64_t>(iterations + index + 1);
            (void)abex::OrderStateMachine::apply(current, report);
        }
    });
    std::cout << "state_machine_ns_per_report=" << elapsed << '\n';
}

void benchmark_decimal_format() {
    constexpr std::size_t iterations = 1'000'000;
    constexpr auto value = abex::Decimal::literal("60000.12345678");
    std::array<char, abex::Decimal::maximum_formatted_size> buffer{};
    std::size_t bytes = 0;
    warmup(iterations, [&](std::size_t) { (void)value.format_to(buffer); });
    const auto elapsed = nanoseconds_per_operation(iterations, [&] {
        for (std::size_t index = 0; index < iterations; ++index) {
            bytes += value.format_to(buffer).size();
        }
    });
    std::cout << "decimal_format_to_ns_per_call=" << elapsed << '\n';
    if (bytes == 0) std::abort();
}

void benchmark_json_serialization() {
    constexpr std::size_t iterations = 20'000;
    const auto current = order(42);
    warmup(iterations, [&](std::size_t) { (void)nlohmann::json(current).dump(); });
    std::size_t bytes = 0;
    const auto elapsed = nanoseconds_per_operation(iterations, [&] {
        for (std::size_t index = 0; index < iterations; ++index) {
            bytes += nlohmann::json(current).dump().size();
        }
    });
    std::cout << "order_json_ns_per_serialize=" << elapsed << '\n';
    if (bytes == 0) std::abort();
}

void benchmark_ring_round_trip() {
    constexpr std::size_t iterations = 100'000;
    const auto path = std::filesystem::temp_directory_path() /
                      ("abex-ring-benchmark-" + std::to_string(::getpid()) + ".bin");
    std::filesystem::remove(path);
    {
        abex::MarketDataRingWriter writer(path, 1024);
        abex::MarketDataRingReader reader(path);
        abex::MarketDataCursor cursor;
        std::array<abex::MarketQuote, 1> output;
        abex::MarketQuote quote{
            .venue = abex::Venue::Okx,
            .symbol = "BTC-USDT",
            .bid_price = abex::Decimal::parse("59999"),
            .ask_price = abex::Decimal::parse("60000"),
            .source_time_ms = abex::unix_time_ms(),
            .published_at_ms = abex::unix_time_ms(),
        };
        const auto elapsed = nanoseconds_per_operation(iterations, [&] {
            for (std::size_t index = 0; index < iterations; ++index) {
                writer.publish(std::span<const abex::MarketQuote>(&quote, 1));
                if (reader.read_available(cursor, output) != 1) std::abort();
            }
        });
        std::cout << "market_ring_round_trip_ns=" << elapsed << '\n';
    }
    std::filesystem::remove(path);
}

void benchmark_gateway_place() {
    constexpr std::size_t iterations = 1'000;
    const abex::SimulatedExchangeAdapter::Config config{
        .request_burst = 1'000'000,
        .requests_per_second = 1'000'000,
    };
    auto okx = std::make_shared<abex::SimulatedExchangeAdapter>(abex::Venue::Okx, config);
    auto binance =
        std::make_shared<abex::SimulatedExchangeAdapter>(abex::Venue::Binance, config);
    abex::OrderGateway gateway(
        {okx, binance}, risk_manager(), std::make_shared<abex::MemoryOrderStore>(),
        {.event_queue_capacity = 4096, .reconcile_on_start = false});
    gateway.start();
    std::size_t accepted = 0;
    const auto elapsed = nanoseconds_per_operation(iterations, [&] {
        for (std::size_t index = 0; index < iterations; ++index) {
            auto request = abex::OrderRequest{
                .client_order_id = "gateway-benchmark-" + std::to_string(index),
                .venue = index % 2 == 0 ? abex::Venue::Okx : abex::Venue::Binance,
                .symbol = "BTC-USDT",
                .side = abex::Side::Buy,
                .type = abex::OrderType::Limit,
                .price = abex::Decimal::parse("60000"),
                .quantity = abex::Decimal::parse("0.001"),
                .time_in_force = abex::TimeInForce::Gtc,
            };
            if (gateway.place(request).ok) ++accepted;
        }
        gateway.flush_events();
    });
    gateway.stop();
    std::cout << "gateway_place_simulated_ns_per_order=" << elapsed << '\n';
    if (accepted != iterations) std::abort();
}

void benchmark_journal() {
    constexpr std::size_t iterations = 5'000;
    const auto path = std::filesystem::temp_directory_path() /
                      ("abex-benchmark-" + std::to_string(::getpid()) + ".jsonl");
    std::filesystem::remove(path);
    {
        abex::FileOrderStore store(path, false);
        const auto elapsed = nanoseconds_per_operation(iterations, [&] {
            for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
                (void)store.append_event({.occurred_at_ms = abex::unix_time_ms(),
                                          .severity = abex::OperationalSeverity::Info,
                                          .category = "BENCHMARK",
                                          .code = "APPEND",
                                          .message = "journal append benchmark"});
            }
        });
        std::cout << "journal_nondurable_ns_per_append=" << elapsed << '\n';
    }
    std::filesystem::remove(path);
}

void benchmark_durable_journal() {
    constexpr std::size_t iterations = 100;
    const auto path = std::filesystem::temp_directory_path() /
                      ("abex-durable-benchmark-" + std::to_string(::getpid()) + ".jsonl");
    std::filesystem::remove(path);
    {
        abex::FileOrderStore store(path, true);
        const auto elapsed = nanoseconds_per_operation(iterations, [&] {
            for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
                store.append(order(iteration));
            }
        });
        std::cout << "journal_durable_ns_per_append=" << elapsed << '\n';
    }
    std::filesystem::remove(path);
}

} // namespace

int main() {
    benchmark_risk_snapshot();
    benchmark_market_lookup();
    benchmark_state_machine();
    benchmark_decimal_format();
    benchmark_dispatcher();
    benchmark_two_venue_dispatch();
    benchmark_gateway_place();
    benchmark_ring_round_trip();
    benchmark_json_serialization();
    benchmark_journal();
    benchmark_durable_journal();
}
