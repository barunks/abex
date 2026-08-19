#pragma once

#include "abex/domain/execution_report.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace abex {

class BoundedEventDispatcher final {
public:
    using Handler = std::function<void(Venue, const ExecutionReport&)>;

    BoundedEventDispatcher(std::size_t capacity, Handler handler)
        : capacity_(checked_capacity(capacity)), handler_(std::move(handler)), queue_(capacity_),
          worker_([this](std::stop_token token) {
              run(token);
          }) {}

    ~BoundedEventDispatcher() {
        worker_.request_stop();
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    BoundedEventDispatcher(const BoundedEventDispatcher&) = delete;
    BoundedEventDispatcher& operator=(const BoundedEventDispatcher&) = delete;

    [[nodiscard]] bool submit(Venue venue,
                              ExecutionReport report,
                              std::chrono::milliseconds timeout = std::chrono::milliseconds{250}) {
        std::unique_lock lock(mutex_);
        if (!not_full_.wait_for(lock, timeout, [this] { return size_ < capacity_; })) {
            return false;
        }
        queue_[tail_].emplace(venue, std::move(report));
        if (++tail_ == capacity_) tail_ = 0;
        ++size_;
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    void flush() {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [this] { return size_ == 0 && in_flight_ == 0; });
    }

    [[nodiscard]] std::size_t size() const {
        std::scoped_lock lock(mutex_);
        return size_;
    }

private:
    [[nodiscard]] static std::size_t checked_capacity(std::size_t capacity) {
        if (capacity == 0) throw std::invalid_argument("event queue capacity must be positive");
        return capacity;
    }

    void run(std::stop_token token) {
        while (true) {
            std::pair<Venue, ExecutionReport> item;
            {
                std::unique_lock lock(mutex_);
                not_empty_.wait(lock, token, [this] { return size_ != 0; });
                if (token.stop_requested() && size_ == 0) break;
                item = std::move(*queue_[head_]);
                queue_[head_].reset();
                if (++head_ == capacity_) head_ = 0;
                --size_;
                ++in_flight_;
            }
            not_full_.notify_one();
            try {
                handler_(item.first, item.second);
            } catch (...) {
                // Ingestion must stay alive. The gateway records the operational error.
            }
            {
                std::scoped_lock lock(mutex_);
                --in_flight_;
                if (size_ == 0 && in_flight_ == 0) idle_.notify_all();
            }
        }
    }

    std::size_t capacity_;
    Handler handler_;
    mutable std::mutex mutex_;
    std::condition_variable_any not_empty_;
    std::condition_variable not_full_;
    std::condition_variable idle_;
    std::vector<std::optional<std::pair<Venue, ExecutionReport>>> queue_;
    std::size_t head_{0};
    std::size_t tail_{0};
    std::size_t size_{0};
    std::size_t in_flight_{0};
    std::jthread worker_;
};

} // namespace abex
