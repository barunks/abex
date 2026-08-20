#pragma once

#include "abex/domain/order.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <vector>

namespace abex {

// Moves order-observer fanout off the critical path.
//
// Two threads produce notifications: the place/cancel/amend caller thread and
// the SpscExecutionLane worker thread. A mutex serializes the two producers
// only for the slot-claim (tail increment + move-into-slot) — O(1) hold time,
// no heap allocation. The consumer owns head_ exclusively and never touches
// producer_mutex_. Cache-line padding separates head and tail.
//
// Observer registration/removal is COW (atomic<shared_ptr>) so the dispatch
// thread reads the list with a single atomic load — no lock held across calls.
class AsyncOrderObserverQueue final {
public:
    using ObserverToken = std::uint64_t;
    using OrderObserver = std::function<void(const Order&)>;

    explicit AsyncOrderObserverQueue(std::size_t capacity = 8192)
        : capacity_(capacity),
          ring_(capacity),
          available_slots_(static_cast<std::ptrdiff_t>(capacity)),
          observers_(std::make_shared<ObserverList>()),
          worker_([this](std::stop_token token) { run(token); }) {
        if (capacity_ == 0) throw std::invalid_argument("observer queue capacity is required");
    }

    ~AsyncOrderObserverQueue() {
        flush();
        worker_.request_stop();
        available_items_.release(); // wake worker for stop check
    }

    AsyncOrderObserverQueue(const AsyncOrderObserverQueue&) = delete;
    AsyncOrderObserverQueue& operator=(const AsyncOrderObserverQueue&) = delete;

    // Called from any thread. Takes shared ownership — zero deep copy of Order.
    // Returns false if the queue is full (backpressure).
    [[nodiscard]] bool post(std::shared_ptr<const Order> order) noexcept {
        if (!order) return false;
        if (!available_slots_.try_acquire()) return false;
        try {
            {
                std::scoped_lock lock(producer_mutex_);
                ring_[tail_ % capacity_].emplace(std::move(order));
                ++tail_;
            }
            pending_.fetch_add(1, std::memory_order_release);
            available_items_.release();
            return true;
        } catch (...) {
            available_slots_.release();
            return false;
        }
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

    void run(std::stop_token token) {
        while (true) {
            available_items_.acquire();
            if (token.stop_requested() &&
                pending_.load(std::memory_order_acquire) == 0) break;

            // Consumer owns head_ — no lock needed.
            auto& slot = ring_[head_ % capacity_];
            std::shared_ptr<const Order> order = std::move(*slot);
            slot.reset();
            ++head_;
            available_slots_.release();

            const auto snapshot = observers_.load(std::memory_order_acquire);
            for (const auto& [tok, observer] : *snapshot) {
                (void)tok;
                try { observer(*order); } catch (...) {}
            }
            if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1)
                pending_.notify_all();
        }
        pending_.notify_all();
    }

    const std::size_t capacity_;
    std::vector<std::optional<std::shared_ptr<const Order>>> ring_;
    std::counting_semaphore<> available_slots_;
    std::counting_semaphore<> available_items_{0};

    // Serializes the two producers. Hold time: one index increment + one move-into-slot.
    std::mutex producer_mutex_;
    alignas(64) std::size_t tail_{0};

    // Consumer-private — never touched by producers or producer_mutex_.
    alignas(64) std::size_t head_{0};

    std::atomic<std::size_t> pending_{0};

    mutable std::mutex observer_mutex_;
    std::atomic<std::shared_ptr<const ObserverList>> observers_;
    std::atomic<ObserverToken> next_token_{1};

    std::jthread worker_;
};

} // namespace abex
