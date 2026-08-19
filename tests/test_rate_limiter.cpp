#include "abex/infrastructure/rate_limiter.hpp"
#include "abex/infrastructure/application_heartbeat.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

using abex::TokenBucket;
using namespace abex;

TEST_CASE("application heartbeat times out only after a sent ping lacks its pong",
          "[websocket][heartbeat]") {
    abex::ApplicationHeartbeat heartbeat;
    CHECK(heartbeat.timer_elapsed() == abex::HeartbeatTimerAction::Send);
    heartbeat.request_sent();
    CHECK(heartbeat.awaiting_response());
    CHECK(heartbeat.timer_elapsed() == abex::HeartbeatTimerAction::Timeout);
    CHECK_FALSE(heartbeat.observe("unrelated", "pong"));
    CHECK(heartbeat.timer_elapsed() == abex::HeartbeatTimerAction::Timeout);
    CHECK(heartbeat.observe("pong", "pong"));
    CHECK_FALSE(heartbeat.awaiting_response());
    CHECK(heartbeat.timer_elapsed() == abex::HeartbeatTimerAction::Send);
}

TEST_CASE("token bucket enforces burst and synchronized availability",
          "[rate-limiter]") {
    TokenBucket bucket(2.0, 0.01);
    CHECK(bucket.try_acquire());
    CHECK(bucket.try_acquire());
    CHECK_FALSE(bucket.try_acquire());
    CHECK_FALSE(bucket.try_acquire(3.0));

    bucket.synchronize(5.0, 2.0, 0.01);
    CHECK(bucket.available() <= 2.0);
    CHECK(bucket.available() > 1.99);
    CHECK(bucket.try_acquire());
    CHECK(bucket.try_acquire());
    CHECK_FALSE(bucket.try_acquire());
}

TEST_CASE("token bucket CAS admits no more than capacity under contention",
          "[rate-limiter][concurrency]") {
    constexpr std::size_t capacity = 100;
    TokenBucket bucket(static_cast<double>(capacity), 0.01);
    std::atomic<std::size_t> accepted{0};
    std::vector<std::jthread> workers;
    for (std::size_t thread = 0; thread < 8; ++thread) {
        workers.emplace_back([&] {
            for (std::size_t attempt = 0; attempt < capacity; ++attempt) {
                if (bucket.try_acquire()) accepted.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    workers.clear();
    CHECK(accepted.load(std::memory_order_relaxed) == capacity);
    CHECK_FALSE(bucket.try_acquire());
}

TEST_CASE("rate-limited place is persisted as REJECTED and never reaches the adapter",
          "[rate-limiter][gateway]") {
    // Burst=2: query_balances + place = 2 tokens per successful order.
    // After one successful placement the bucket is empty; the next place() is
    // rejected by the adapter before any venue order is created.
    test::GatewayFixture fixture(
        std::make_shared<MemoryOrderStore>(),
        SimulatedExchangeAdapter::Config{.request_burst = 2.0, .requests_per_second = 0.001});

    REQUIRE(fixture.gateway->place(test::limit_order("rl-first")).ok);

    const auto result = fixture.gateway->place(test::limit_order("rl-rejected"));
    CHECK_FALSE(result.ok);
    REQUIRE(result.order);
    CHECK(result.order->status == OrderStatus::Rejected);
    CHECK(result.order->pending_action == PendingAction::None);
    // The adapter must not have a record of the rejected order.
    CHECK_FALSE(fixture.okx->query(*result.order));
}

TEST_CASE("rate-limited rejection is idempotently replayed on retry",
          "[rate-limiter][gateway][idempotency]") {
    // Same burst=2 budget: first order succeeds, second is rejected locally.
    test::GatewayFixture fixture(
        std::make_shared<MemoryOrderStore>(),
        SimulatedExchangeAdapter::Config{.request_burst = 2.0, .requests_per_second = 0.001});

    REQUIRE(fixture.gateway->place(test::limit_order("rl-base")).ok);
    const auto request = test::limit_order("rl-retry");
    const auto first = fixture.gateway->place(request);
    REQUIRE_FALSE(first.ok);
    REQUIRE(first.order);
    REQUIRE(first.order->status == OrderStatus::Rejected);

    // Identical retry must replay the persisted rejection without touching the adapter.
    const auto replay = fixture.gateway->place(request);
    CHECK_FALSE(replay.ok);
    CHECK(replay.idempotent_replay);
    CHECK(replay.code == "ORDER_REJECTED");
    CHECK(replay.order->status == OrderStatus::Rejected);
}

TEST_CASE("synchronize restores token bucket capacity and allows subsequent placement",
          "[rate-limiter]") {
    // Verify synchronize() semantics directly: exhaust a small bucket then
    // restore it to full capacity via synchronize().
    TokenBucket bucket(2.0, 0.001);
    CHECK(bucket.try_acquire());
    CHECK(bucket.try_acquire());
    CHECK_FALSE(bucket.try_acquire());

    // Simulate a Binance rateLimits response that reports full capacity available.
    bucket.synchronize(2.0, 2.0, 0.001);
    CHECK(bucket.available() > 1.99);
    CHECK(bucket.try_acquire());
    CHECK(bucket.try_acquire());
    CHECK_FALSE(bucket.try_acquire());
}

TEST_CASE("Binance rateLimits synchronize restores gateway placement after exhaustion",
          "[rate-limiter][gateway]") {
    // Exhaust the OKX adapter bucket, then call synchronize_rate_limiter() to
    // simulate a Binance rateLimits resync restoring full capacity, and verify
    // that a subsequent placement succeeds end-to-end through the gateway.
    test::GatewayFixture fixture(
        std::make_shared<MemoryOrderStore>(),
        SimulatedExchangeAdapter::Config{.request_burst = 2.0, .requests_per_second = 0.001});

    REQUIRE(fixture.gateway->place(test::limit_order("rl-sync-first")).ok);
    const auto blocked = fixture.gateway->place(test::limit_order("rl-sync-blocked"));
    REQUIRE_FALSE(blocked.ok);
    REQUIRE(blocked.order);
    CHECK(blocked.order->status == OrderStatus::Rejected);

    // Simulate rateLimits response: restore full capacity.
    fixture.okx->synchronize_rate_limiter(100.0, 100.0, 100.0);

    const auto recovered = fixture.gateway->place(test::limit_order("rl-sync-recovered"));
    CHECK(recovered.ok);
    CHECK(recovered.order->status == OrderStatus::Live);
}

TEST_CASE("adapter exception on place produces UNKNOWN outcome and reconciliation flag",
          "[rate-limiter][gateway]") {
    // throw_on_place causes adapter->place() to throw, which the gateway catches
    // and maps to outcome_uncertain=true / ADAPTER_EXCEPTION.
    test::GatewayFixture fixture(
        std::make_shared<MemoryOrderStore>(),
        SimulatedExchangeAdapter::Config{.throw_on_place = true});

    const auto result = fixture.gateway->place(test::limit_order("rl-exception"));
    CHECK_FALSE(result.ok);
    CHECK(result.code == "ADAPTER_EXCEPTION");
    REQUIRE(result.order);
    CHECK(result.order->status == OrderStatus::Unknown);
    CHECK(result.order->pending_action == PendingAction::Reconcile);
    CHECK(fixture.gateway->health().at(Venue::Okx).reconciliation_required);
}

TEST_CASE("concurrent add and remove order observers never corrupt notification",
          "[gateway][observer][concurrency]") {
    test::GatewayFixture fixture;
    std::atomic<std::size_t> notifications{0};

    // Add 8 observers from 4 threads while simultaneously removing them.
    std::vector<OrderGateway::ObserverToken> tokens;
    std::mutex tokens_mutex;
    std::vector<std::jthread> workers;

    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&] {
            const auto t1 = fixture.gateway->add_order_observer(
                [&](const Order&) { notifications.fetch_add(1, std::memory_order_relaxed); });
            const auto t2 = fixture.gateway->add_order_observer(
                [&](const Order&) { notifications.fetch_add(1, std::memory_order_relaxed); });
            {
                std::scoped_lock lock(tokens_mutex);
                tokens.push_back(t1);
                tokens.push_back(t2);
            }
        });
    }
    workers.clear();

    // Place an order — all 8 observers must fire without crash or deadlock.
    // place() notifies twice: once for intent, once for ack = 8 * 2 = 16.
    REQUIRE(fixture.gateway->place(test::limit_order("obs-concurrent")).ok);
    fixture.gateway->flush_events();
    CHECK(notifications.load(std::memory_order_relaxed) == 16);

    // Remove all observers then verify no further notifications arrive.
    for (const auto token : tokens) fixture.gateway->remove_order_observer(token);
    notifications.store(0, std::memory_order_relaxed);
    REQUIRE(fixture.gateway->place(test::limit_order("obs-after-remove")).ok);
    fixture.gateway->flush_events();
    CHECK(notifications.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("observer removed during notification is not called on the next order",
          "[gateway][observer]") {
    test::GatewayFixture fixture;
    std::atomic<std::size_t> calls{0};
    OrderGateway::ObserverToken self_token{0};

    // Observer removes itself on first call.
    self_token = fixture.gateway->add_order_observer([&](const Order&) {
        calls.fetch_add(1, std::memory_order_relaxed);
        fixture.gateway->remove_order_observer(self_token);
    });

    REQUIRE(fixture.gateway->place(test::limit_order("obs-self-remove-1")).ok);
    fixture.gateway->flush_events();
    CHECK(calls.load() == 1);

    // Second placement must not invoke the removed observer.
    REQUIRE(fixture.gateway->place(test::limit_order("obs-self-remove-2")).ok);
    fixture.gateway->flush_events();
    CHECK(calls.load() == 1);
}
