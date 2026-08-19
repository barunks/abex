#pragma once

#include "abex/ports/order_store.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace abex {

// Serializes operational audit records independently of order-state locking.
// Order snapshots remain synchronous WAL records; observability records are
// durably written in submission order and published only after append succeeds.
class OperationalEventWriter final {
public:
    using Completion =
        std::function<void(std::optional<OperationalEvent>, std::string error)>;

    OperationalEventWriter(std::shared_ptr<IOrderStore> store,
                           Completion completion,
                           std::size_t capacity = 8192)
        : store_(std::move(store)), completion_(std::move(completion)), capacity_(capacity),
          worker_([this](std::stop_token token) { run(token); }) {
        if (!store_) throw std::invalid_argument("operational event store is required");
        if (!completion_) throw std::invalid_argument("operational completion is required");
        if (capacity_ == 0) throw std::invalid_argument("operational queue capacity is required");
    }

    ~OperationalEventWriter() {
        flush();
        worker_.request_stop();
        available_.notify_all();
    }

    OperationalEventWriter(const OperationalEventWriter&) = delete;
    OperationalEventWriter& operator=(const OperationalEventWriter&) = delete;

    [[nodiscard]] bool submit(OperationalEvent event) noexcept {
        try {
            {
                std::scoped_lock lock(mutex_);
                if (queue_.size() >= capacity_) return false;
                queue_.push_back(std::move(event));
            }
            available_.notify_one();
            return true;
        } catch (...) {
            return false;
        }
    }

    void flush() {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [this] { return queue_.empty() && in_flight_ == 0; });
    }

private:
    void run(std::stop_token token) {
        while (true) {
            OperationalEvent event;
            {
                std::unique_lock lock(mutex_);
                available_.wait(lock, token, [this] { return !queue_.empty(); });
                if (token.stop_requested() && queue_.empty()) break;
                event = std::move(queue_.front());
                queue_.pop_front();
                ++in_flight_;
            }
            try {
                completion_(store_->append_event(std::move(event)), {});
            } catch (const std::exception& error) {
                completion_(std::nullopt, error.what());
            } catch (...) {
                completion_(std::nullopt, "unknown operational logging failure");
            }
            {
                std::scoped_lock lock(mutex_);
                --in_flight_;
                if (queue_.empty() && in_flight_ == 0) idle_.notify_all();
            }
        }
    }

    std::shared_ptr<IOrderStore> store_;
    Completion completion_;
    const std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable_any available_;
    std::condition_variable idle_;
    std::deque<OperationalEvent> queue_;
    std::size_t in_flight_{0};
    std::jthread worker_;
};

} // namespace abex
