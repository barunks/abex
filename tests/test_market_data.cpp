#include "abex/application/market_data_book.hpp"
#include "abex/application/order_gateway.hpp"
#include "abex/infrastructure/market_data_feed.hpp"
#include "abex/infrastructure/market_data_ring.hpp"
#include "abex/infrastructure/simulated_exchange_adapter.hpp"

#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

using namespace abex;

namespace {

class TemporaryRing final {
public:
    TemporaryRing()
        : path_(std::filesystem::temp_directory_path() /
                ("abex-market-ring-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 ".ring")) {}
    ~TemporaryRing() { std::filesystem::remove(path_); }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

[[nodiscard]] MarketQuote quote(Venue venue,
                                std::string symbol,
                                std::string bid,
                                std::string ask) {
    const auto now = unix_time_ms();
    return {
        .venue = venue,
        .symbol = std::move(symbol),
        .bid_price = Decimal::parse(bid),
        .ask_price = Decimal::parse(ask),
        .source_time_ms = now,
        .published_at_ms = now,
    };
}

} // namespace

TEST_CASE("memory-mapped market-data ring publishes committed records", "[market-data][mmap]") {
    TemporaryRing file;
    MarketDataRingWriter writer(file.path(), 4);
    MarketDataRingReader reader(file.path());
    MarketDataCursor cursor;

    const std::array first{
        quote(Venue::Okx, "BTC-USDT", "59999", "60001"),
        quote(Venue::Binance, "BTC-USDT", "60000", "60002"),
    };
    writer.publish(first);
    const auto initial = reader.read_available(cursor);
    REQUIRE(initial.size() == 2);
    CHECK(initial.front().sequence == 1);
    CHECK(initial.back().ask_price == Decimal::parse("60002"));
    CHECK(cursor.generation == writer.generation());
    CHECK(cursor.sequence == 2);

    const std::array overwrite{
        quote(Venue::Okx, "ETH-USDT", "3000", "3001"),
        quote(Venue::Binance, "ETH-USDT", "3001", "3002"),
        quote(Venue::Okx, "BTC-USDT", "60003", "60004"),
        quote(Venue::Binance, "BTC-USDT", "60004", "60005"),
        quote(Venue::Okx, "ETH-USDT", "3002", "3003"),
    };
    writer.publish(overwrite);
    MarketDataCursor late_reader;
    const auto retained = reader.read_available(late_reader);
    REQUIRE(retained.size() == 4);
    CHECK(retained.front().sequence == 4);
    CHECK(retained.back().sequence == 7);
}

TEST_CASE("mapped readers follow a restarted publisher generation", "[market-data][mmap]") {
    TemporaryRing file;
    MarketDataCursor cursor;
    std::unique_ptr<MarketDataRingReader> reader;
    std::uint64_t first_generation = 0;
    {
        MarketDataRingWriter writer(file.path(), 4);
        reader = std::make_unique<MarketDataRingReader>(file.path());
        const std::array quotes{quote(Venue::Okx, "BTC-USDT", "59999", "60001")};
        writer.publish(quotes);
        REQUIRE(reader->read_available(cursor).size() == 1);
        first_generation = cursor.generation;
    }
    {
        MarketDataRingWriter writer(file.path(), 4);
        const std::array quotes{quote(Venue::Binance, "ETH-USDT", "3000", "3001")};
        writer.publish(quotes);
        const auto restarted = reader->read_available(cursor);
        REQUIRE(restarted.size() == 1);
        CHECK(cursor.generation != first_generation);
        CHECK(cursor.sequence == 1);
        CHECK(restarted.front().symbol == "ETH-USDT");
    }
}

TEST_CASE("ring feed reads mapped records into the in-memory book", "[market-data][feed]") {
    TemporaryRing file;
    auto book = std::make_shared<MarketDataBook>();
    MarketDataRingWriter writer(file.path(), 16);
    MarketDataRingFeed feed(file.path(), book, std::chrono::milliseconds{2});
    feed.start();
    const std::array quotes{
        quote(Venue::Okx, "BTC-USDT", "59999", "60001"),
        quote(Venue::Binance, "ETH-USDT", "3000", "3001"),
    };
    writer.publish(quotes);

    for (int attempt = 0; attempt < 100 && book->snapshot().size() != 2; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    REQUIRE(book->snapshot().size() == 2);
    CHECK(book->status().ring_connected);
    CHECK(book->price(Venue::Okx, "BTC-USDT", Side::Buy) == Decimal::parse("60001"));
    feed.stop();
}

TEST_CASE("ring feed replaces stale quotes when the publisher generation restarts",
          "[market-data][feed][restart]") {
    TemporaryRing file;
    auto book = std::make_shared<MarketDataBook>();
    auto old = quote(Venue::Okx, "BTC-USDT", "50000", "50001");
    old.sequence = 50'000;
    book->publish(old);
    book->set_ring_status(true, 99, old.sequence);

    MarketDataRingWriter writer(file.path(), 16);
    MarketDataRingFeed feed(file.path(), book, std::chrono::milliseconds{2});
    feed.start();
    const std::array restarted{
        quote(Venue::Okx, "BTC-USDT", "60000", "60001"),
    };
    writer.publish(restarted);

    for (int attempt = 0; attempt < 100; ++attempt) {
        const auto current = book->latest(Venue::Okx, "BTC-USDT");
        if (current && current->sequence == 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    const auto current = book->latest(Venue::Okx, "BTC-USDT");
    REQUIRE(current);
    CHECK(current->sequence == 1);
    CHECK(current->bid_price == Decimal::parse("60000"));
    CHECK(book->status().generation == writer.generation());
    CHECK(book->status().ring_connected);
    feed.stop();
}

TEST_CASE("mapped ring health becomes stale when publisher updates stop",
          "[market-data][health]") {
    MarketDataBook book(std::chrono::milliseconds{5});
    auto stale = quote(Venue::Binance, "ETH-USDT", "3000", "3001");
    stale.published_at_ms -= 100;
    book.publish(stale);
    book.set_ring_status(true, 7, 1);

    const auto status = book.status();
    CHECK(status.ring_mapped);
    CHECK_FALSE(status.ring_connected);
    CHECK(status.last_error == "market-data publisher updates are stale");
}

TEST_CASE("market-data executable prices reject stale quotes explicitly",
          "[market-data][stale]") {
    MarketDataBook book(std::chrono::milliseconds{5});
    auto stale = quote(Venue::Okx, "BTC-USDT", "59999", "60001");
    stale.published_at_ms -= 100;
    book.publish(stale);

    REQUIRE(book.latest(Venue::Okx, "BTC-USDT"));
    CHECK_FALSE(book.price(Venue::Okx, "BTC-USDT", Side::Buy));
    CHECK_FALSE(book.price(Venue::Okx, "BTC-USDT", Side::Sell));
}

TEST_CASE("market-data slots remain coherent during concurrent publish and read",
          "[market-data][concurrency]") {
    MarketDataBook book;
    std::atomic<bool> finished{false};
    std::atomic<bool> coherent{true};
    std::jthread reader([&] {
        while (!finished.load(std::memory_order_acquire)) {
            const auto current = book.latest(Venue::Binance, "ETH-USDT");
            if (current && (current->symbol != "ETH-USDT" ||
                            current->ask_price.raw() - current->bid_price.raw() != 1)) {
                coherent.store(false, std::memory_order_release);
            }
        }
    });
    for (std::uint64_t sequence = 1; sequence <= 20'000; ++sequence) {
        const auto bid = Decimal::from_raw(static_cast<std::int64_t>(sequence));
        book.publish({.venue = Venue::Binance,
                      .symbol = "ETH-USDT",
                      .bid_price = bid,
                      .ask_price = Decimal::from_raw(bid.raw() + 1),
                      .source_time_ms = unix_time_ms(),
                      .published_at_ms = unix_time_ms(),
                      .sequence = sequence});
    }
    finished.store(true, std::memory_order_release);
    reader.join();
    CHECK(coherent.load(std::memory_order_acquire));
    CHECK(book.latest(Venue::Binance, "ETH-USDT")->sequence == 20'000);
}

TEST_CASE("mapped quotes price market orders and trigger simulated limit fills",
          "[market-data][simulation]") {
    auto book = std::make_shared<MarketDataBook>();
    auto okx = std::make_shared<SimulatedExchangeAdapter>(
        Venue::Okx, SimulatedExchangeAdapter::Config{}, book);
    auto binance = std::make_shared<SimulatedExchangeAdapter>(
        Venue::Binance, SimulatedExchangeAdapter::Config{}, book);
    OrderGateway gateway({okx, binance}, test::risk_manager(),
                         std::make_shared<MemoryOrderStore>(),
                         {.event_queue_capacity = 64, .reconcile_on_start = false}, book);
    gateway.start();
    book->publish(quote(Venue::Okx, "BTC-USDT", "59999", "60001"));

    auto market = test::limit_order("market-fill", Venue::Okx);
    market.type = OrderType::Market;
    market.price.reset();
    REQUIRE(gateway.place(market).ok);
    gateway.flush_events();
    const auto market_order = gateway.get("market-fill");
    REQUIRE(market_order);
    CHECK(market_order->status == OrderStatus::Filled);
    CHECK(market_order->average_fill_price == Decimal::parse("60001"));

    auto limit = test::limit_order("limit-fill", Venue::Okx, Side::Buy, "0.1", "59000");
    REQUIRE(gateway.place(limit).ok);
    gateway.flush_events();
    CHECK(gateway.get("limit-fill")->status == OrderStatus::Live);
    book->publish(quote(Venue::Okx, "BTC-USDT", "58998", "58999"));
    gateway.flush_events();
    CHECK(gateway.get("limit-fill")->status == OrderStatus::Filled);
    CHECK(gateway.get("limit-fill")->average_fill_price == Decimal::parse("58999"));
    gateway.stop();
}

TEST_CASE("market orders fail closed when the mapped quote is unavailable",
          "[market-data][risk]") {
    auto book = std::make_shared<MarketDataBook>();
    auto okx = std::make_shared<SimulatedExchangeAdapter>(
        Venue::Okx, SimulatedExchangeAdapter::Config{}, book);
    auto binance = std::make_shared<SimulatedExchangeAdapter>(
        Venue::Binance, SimulatedExchangeAdapter::Config{}, book);
    OrderGateway gateway({okx, binance}, test::risk_manager(),
                         std::make_shared<MemoryOrderStore>(),
                         {.event_queue_capacity = 64, .reconcile_on_start = false}, book);
    gateway.start();
    auto request = test::limit_order("no-market-data", Venue::Binance);
    request.type = OrderType::Market;
    request.price.reset();
    const auto result = gateway.place(request);
    CHECK_FALSE(result.ok);
    CHECK(result.code == "MARKET_DATA_UNAVAILABLE");
    gateway.stop();
}

TEST_CASE("market orders fail closed when the only mapped quote is stale",
          "[market-data][risk][stale]") {
    auto book = std::make_shared<MarketDataBook>(std::chrono::milliseconds{5});
    auto okx = std::make_shared<SimulatedExchangeAdapter>(
        Venue::Okx, SimulatedExchangeAdapter::Config{}, book);
    auto binance = std::make_shared<SimulatedExchangeAdapter>(
        Venue::Binance, SimulatedExchangeAdapter::Config{}, book);
    OrderGateway gateway({okx, binance}, test::risk_manager(),
                         std::make_shared<MemoryOrderStore>(),
                         {.event_queue_capacity = 64, .reconcile_on_start = false}, book);
    gateway.start();
    auto stale = quote(Venue::Okx, "BTC-USDT", "59999", "60001");
    stale.published_at_ms -= 100;
    book->publish(stale);
    auto request = test::limit_order("stale-market-data", Venue::Okx);
    request.type = OrderType::Market;
    request.price.reset();
    const auto result = gateway.place(request);
    CHECK_FALSE(result.ok);
    CHECK(result.code == "MARKET_DATA_UNAVAILABLE");
    CHECK(result.order->status == OrderStatus::Rejected);
    gateway.stop();
}
