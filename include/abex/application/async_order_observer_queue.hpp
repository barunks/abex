#pragma once

#include "abex/application/mpsc_ring.hpp"
#include "abex/domain/order.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace abex {

// Moves order-observer fanout off the critical path.
//
// Two producers: the place/cancel/amend caller thread and the SpscExecutionLane
// worker. Both post via MpscRing::try_push — one atomic fetch_add + one
// release-store, no mutex. Observer registration/removal is COW so the
// dispatch thread reads the list with a single atomic load.
class AsyncOrderObserverQueue final {
public:
    using ObserverToken = std::uint64_t;
    using OrderObserver = std::function<void(const Order&)>;

    explicit AsyncOrderObserverQueue(std::size_t capacity = 8192)
        : ring_(next_pow2(capacity)),
          observers_(std::make_shared<ObserverList>()),
          worker_([this](std::stop_token token) { run(token); }) {}

    ~AsyncOrderObserverQueue() {
        flush();
        worker_.request_stop();
        ring_.wake_consumer();
    }

    AsyncOrderObserverQueue(const AsyncOrderObserverQueue&) = delete;
    AsyncOrderObserverQueue& operator=(const AsyncOrderObserverQueue&) = delete;

    // One atomic fetch_add + one release-store. No mutex.
    [[nodiscard]] bool post(std::shared_ptr<Order> order) noexcept {
        if (!order) return false;
        if (!ring_.try_push(std::move(order))) return false;
        pending_.fetch_add(1, std::memory_order_release);
        return true;
    }

    void flush() {
        auto pending = pending_.load(std::memory_order_acquire);
        while (pending != 0) {
            pending_.wait(pending, std::memory_order_acquire);
            pending = pending_.load(std::memory_order_acquire);
        }
    }

    [[nodiscard]] ObserverToken add(OrderObserver observer) {
        if (!observer) throw std::invalid_argument("order observer is empty");
        const auto token = next_token_.fetch_add(1, std::memory_order_relaxed);
        std::scoped_lock lock(observer_mutex_);
        auto updated = std::make_shared<ObserverList>(*observers_.load(std::memory_order_acquire));
        updated->emplace_back(token, std::move(observer));
        observers_.store(std::move(updated), std::memory_order_release);
        return token;
    }

    void remove(ObserverToken token) noexcept {
        std::scoped_lock lock(observer_mutex_);
        auto updated = std::make_shared<ObserverList>(*observers_.load(std::memory_order_acquire));
        std::erase_if(*updated, [token](const auto& e) { return e.first == token; });
        observers_.store(std::move(updated), std::memory_order_release);
    }

private:
    using ObserverList = std::vector<std::pair<ObserverToken, OrderObserver>>;

    static std::size_t next_pow2(std::size_t n) {
        if (n == 0) return 1;
        --n;
        for (std::size_t i = 1; i < sizeof(n) * 8; i <<= 1) n |= n >> i;
        return n + 1;
    }

    void dispatch(std::shared_ptr<Order> order) noexcept {
        const auto snapshot = observers_.load(std::memory_order_acquire);
        for (const auto& [tok, observer] : *snapshot) {
            (void)tok;
            try { observer(*order); } catch (...) {}
        }
        if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1)
            pending_.notify_all();
    }

    void run(std::stop_token token) {
        while (!token.stop_requested()) {
            auto [ok, order] = ring_.pop_item();
            if (!ok) continue; // spurious wake (stop sentinel)
            dispatch(std::move(order));
        }
        // Drain any items posted before stop was observed.
        std::shared_ptr<Order> order;
        while (ring_.try_pop(order)) dispatch(std::move(order));
        pending_.notify_all();
    }

    MpscRing<std::shared_ptr<Order>> ring_;
    std::atomic<std::size_t> pending_{0};

    mutable std::mutex observer_mutex_;
    std::atomic<std::shared_ptr<const ObserverList>> observers_;
    std::atomic<ObserverToken> next_token_{1};

    std::jthread worker_;
};

} // namespace abex
