#pragma once

#include "abex/domain/execution_report.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace abex {

// One execution lane belongs to exactly one adapter. The adapter callback is the
// single logical producer and this lane's worker is the single consumer. The
// ring itself is lock-free; counting semaphores park a full producer or idle
// consumer without the lost-wakeup window of a condition variable whose queue
// predicate is changed outside its mutex.
class SpscExecutionLane final {
public:
    using Handler = std::function<void(const ExecutionReport&)>;

    SpscExecutionLane(std::size_t capacity, Handler handler)
        : capacity_(checked_capacity(capacity)), handler_(checked_handler(std::move(handler))),
          queue_(capacity_), available_slots_(static_cast<std::ptrdiff_t>(capacity_)),
          worker_([this](std::stop_token token) { run(token); }) {}

    ~SpscExecutionLane() {
        worker_.request_stop();
        available_items_.release(); // stop sentinel; the worker drains first
    }

    SpscExecutionLane(const SpscExecutionLane&) = delete;
    SpscExecutionLane& operator=(const SpscExecutionLane&) = delete;

    [[nodiscard]] bool submit(
        ExecutionReport report,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{1}) {
        // Multiple OS threads may represent a single serialized adapter producer
        // (the simulator does this). Concurrent entry violates the lane contract;
        // reject it safely instead of racing the producer-owned tail.
        if (producer_active_.test_and_set(std::memory_order_acquire)) {
            producer_violations_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        struct ProducerGuard {
            std::atomic_flag& flag;
            ~ProducerGuard() { flag.clear(std::memory_order_release); }
        } guard{producer_active_};

        const bool acquired = timeout <= std::chrono::milliseconds::zero()
                                  ? available_slots_.try_acquire()
                                  : available_slots_.try_acquire_for(timeout);
        if (!acquired) return false;
        push(std::move(report));
        return true;
    }

    void flush() {
        auto pending = pending_.load(std::memory_order_acquire);
        while (pending != 0) {
            pending_.wait(pending, std::memory_order_acquire);
            pending = pending_.load(std::memory_order_acquire);
        }
    }

    [[nodiscard]] std::size_t size() const noexcept {
        const auto tail = tail_.load(std::memory_order_acquire);
        const auto head = head_.load(std::memory_order_acquire);
        return static_cast<std::size_t>(tail - head);
    }

    [[nodiscard]] std::uint64_t producer_violations() const noexcept {
        return producer_violations_.load(std::memory_order_relaxed);
    }

private:
    [[nodiscard]] static std::size_t checked_capacity(std::size_t capacity) {
        if (capacity == 0 ||
            capacity > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max())) {
            throw std::invalid_argument("execution lane capacity is outside the supported range");
        }
        return capacity;
    }

    [[nodiscard]] static Handler checked_handler(Handler handler) {
        if (!handler) throw std::invalid_argument("execution lane handler is required");
        return handler;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    void push(ExecutionReport&& report) {
        const auto tail = tail_.load(std::memory_order_relaxed);
        queue_[static_cast<std::size_t>(tail % capacity_)].emplace(std::move(report));
        pending_.fetch_add(1, std::memory_order_release);
        tail_.store(tail + 1, std::memory_order_release);
        available_items_.release();
    }

    [[nodiscard]] bool try_pop(ExecutionReport& report) {
        const auto head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) return false;
        auto& slot = queue_[static_cast<std::size_t>(head % capacity_)];
        report = std::move(*slot);
        slot.reset();
        head_.store(head + 1, std::memory_order_release);
        available_slots_.release();
        return true;
    }

    void run(std::stop_token token) {
        while (true) {
            available_items_.acquire();
            if (token.stop_requested() && empty()) break;
            ExecutionReport report;
            if (!try_pop(report)) {
                continue;
            }

            try {
                handler_(report);
            } catch (...) {
                // An isolated handler failure cannot terminate venue ingestion.
            }
            if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) pending_.notify_all();
        }
        pending_.notify_all();
    }

    const std::uint64_t capacity_;
    Handler handler_;
    std::vector<std::optional<ExecutionReport>> queue_;
    alignas(64) std::atomic<std::uint64_t> head_{0};
    alignas(64) std::atomic<std::uint64_t> tail_{0};
    std::atomic<std::size_t> pending_{0};
    std::atomic_flag producer_active_ = ATOMIC_FLAG_INIT;
    std::atomic<std::uint64_t> producer_violations_{0};
    std::counting_semaphore<> available_slots_;
    std::counting_semaphore<> available_items_{0};
    std::jthread worker_;
};

} // namespace abex
