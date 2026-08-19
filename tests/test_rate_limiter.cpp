#include "abex/infrastructure/rate_limiter.hpp"
#include "abex/infrastructure/application_heartbeat.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

using abex::TokenBucket;

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
