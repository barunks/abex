#include "abex/application/bounded_event_dispatcher.hpp"
#include "abex/application/market_data_book.hpp"
#include "abex/application/risk_manager.hpp"
#include "abex/infrastructure/file_order_store.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

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
    book.publish({.venue = abex::Venue::Okx,
                  .symbol = "BTC-USDT",
                  .bid_price = abex::Decimal::parse("59999"),
                  .ask_price = abex::Decimal::parse("60000"),
                  .source_time_ms = abex::unix_time_ms(),
                  .published_at_ms = abex::unix_time_ms(),
                  .sequence = 1});
    std::int64_t sum = 0;
    const auto elapsed = nanoseconds_per_operation(iterations, [&] {
        for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
            const auto quote = book.latest(abex::Venue::Okx, "BTC-USDT");
            if (quote) sum += quote->bid_price.raw();
        }
    });
    std::cout << "market_lookup_ns_per_call=" << elapsed << '\n';
    if (sum == 0) std::abort();
}

void benchmark_dispatcher() {
    constexpr std::size_t iterations = 200'000;
    std::atomic<std::size_t> handled{0};
    abex::BoundedEventDispatcher dispatcher(
        4096, [&](abex::Venue, const abex::ExecutionReport&) { ++handled; });
    const auto elapsed = nanoseconds_per_operation(iterations, [&] {
        for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
            abex::ExecutionReport report;
            report.event_id = "event-" + std::to_string(iteration);
            while (!dispatcher.submit(abex::Venue::Okx, std::move(report))) {
            }
        }
        dispatcher.flush();
    });
    std::cout << "dispatcher_ns_per_event=" << elapsed << '\n';
    if (handled.load() != iterations) std::abort();
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

} // namespace

int main() {
    benchmark_risk_snapshot();
    benchmark_market_lookup();
    benchmark_dispatcher();
    benchmark_journal();
}
