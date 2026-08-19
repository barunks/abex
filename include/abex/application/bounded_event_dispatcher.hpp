#pragma once

#include "abex/domain/execution_report.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

namespace abex {

class BoundedEventDispatcher final {
public:
    using Handler = std::function<void(Venue, const ExecutionReport&)>;

    BoundedEventDispatcher(std::size_t capacity, Handler handler)
        : capacity_(capacity), handler_(std::move(handler)), worker_([this](std::stop_token token) {
              run(token);
          }) {
        if (capacity == 0) throw std::invalid_argument("event queue capacity must be positive");
    }

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
        if (!not_full_.wait_for(lock, timeout, [this] { return queue_.size() < capacity_; })) {
            return false;
        }
        queue_.emplace_back(venue, std::move(report));
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    void flush() {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [this] { return queue_.empty() && in_flight_ == 0; });
    }

    [[nodiscard]] std::size_t size() const {
        std::scoped_lock lock(mutex_);
        return queue_.size();
    }

private:
    void run(std::stop_token token) {
        while (!token.stop_requested()) {
            std::pair<Venue, ExecutionReport> item;
            {
                std::unique_lock lock(mutex_);
                not_empty_.wait(lock, token, [this] { return !queue_.empty(); });
                if (token.stop_requested() && queue_.empty()) break;
                item = std::move(queue_.front());
                queue_.pop_front();
                ++in_flight_;
                not_full_.notify_one();
            }
            try {
                handler_(item.first, item.second);
            } catch (...) {
                // Ingestion must stay alive. The gateway records the operational error.
            }
            {
                std::scoped_lock lock(mutex_);
                --in_flight_;
                if (queue_.empty() && in_flight_ == 0) idle_.notify_all();
            }
        }
    }

    std::size_t capacity_;
    Handler handler_;
    mutable std::mutex mutex_;
    std::condition_variable_any not_empty_;
    std::condition_variable not_full_;
    std::condition_variable idle_;
    std::deque<std::pair<Venue, ExecutionReport>> queue_;
    std::size_t in_flight_{0};
    std::jthread worker_;
};

} // namespace abex
