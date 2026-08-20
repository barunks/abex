#pragma once

#include "abex/ports/order_store.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace abex {

// Serializes operational audit records independently of order-state locking.
//
// submit() stores a small fixed-size struct into the ring — string construction
// happens on the caller side (SSO-eligible for string literals) but no heap
// allocation for the ring node itself. The consumer owns head_ exclusively and
// never touches producer_mutex_. Cache-line padding separates tail and head.
class OperationalEventWriter final {
public:
    using Completion =
        std::function<void(std::optional<OperationalEvent>, std::string error)>;

    OperationalEventWriter(std::shared_ptr<IOrderStore> store,
                           Completion completion,
                           std::size_t capacity = 8192)
        : store_(std::move(store)), completion_(std::move(completion)), capacity_(capacity),
          ring_(capacity),
          available_slots_(static_cast<std::ptrdiff_t>(capacity)),
          worker_([this](std::stop_token token) { run(token); }) {
        if (!store_) throw std::invalid_argument("operational event store is required");
        if (!completion_) throw std::invalid_argument("operational completion is required");
        if (capacity_ == 0) throw std::invalid_argument("operational queue capacity is required");
    }

    ~OperationalEventWriter() {
        flush();
        worker_.request_stop();
        available_items_.release();
    }

    OperationalEventWriter(const OperationalEventWriter&) = delete;
    OperationalEventWriter& operator=(const OperationalEventWriter&) = delete;

    [[nodiscard]] bool submit(std::int64_t occurred_at_ms,
                              OperationalSeverity severity,
                              std::string_view category,
                              std::string_view code,
                              std::string_view message,
                              std::string_view instance_id,
                              std::optional<Venue> venue,
                              std::string_view client_order_id,
                              std::string_view request_id,
                              std::optional<OrderEventContext> order) noexcept {
        return push(RawEntry{occurred_at_ms, severity,
                             std::string(category), std::string(code),
                             std::string(message), std::string(instance_id),
                             venue, std::string(client_order_id),
                             std::string(request_id), std::move(order)});
    }

    // Enqueue two events under a single producer_mutex_ acquisition.
    [[nodiscard]] bool submit2(std::int64_t occurred_at_ms,
                               OperationalSeverity sev_a,
                               std::string_view cat_a,
                               std::string_view code_a,
                               std::string_view msg_a,
                               OperationalSeverity sev_b,
                               std::string_view cat_b,
                               std::string_view code_b,
                               std::string_view msg_b,
                               std::string_view instance_id,
                               std::optional<Venue> venue,
                               std::string_view client_order_id,
                               std::string_view request_id,
                               std::optional<OrderEventContext> order) noexcept {
        if (!available_slots_.try_acquire()) return false;
        if (!available_slots_.try_acquire()) {
            available_slots_.release();
            return false;
        }
        try {
            {
                std::scoped_lock lock(producer_mutex_);
                ring_[tail_ % capacity_].emplace(RawEntry{
                    occurred_at_ms, sev_a,
                    std::string(cat_a), std::string(code_a), std::string(msg_a),
                    std::string(instance_id), venue,
                    std::string(client_order_id), std::string(request_id), order});
                ring_[(tail_ + 1) % capacity_].emplace(RawEntry{
                    occurred_at_ms, sev_b,
                    std::string(cat_b), std::string(code_b), std::string(msg_b),
                    std::string(instance_id), venue,
                    std::string(client_order_id), std::string(request_id), std::move(order)});
                tail_ += 2;
            }
            pending_.fetch_add(2, std::memory_order_release);
            available_items_.release(2);
            return true;
        } catch (...) {
            available_slots_.release(2);
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

private:
    struct RawEntry {
        std::int64_t occurred_at_ms{0};
        OperationalSeverity severity{};
        std::string category;
        std::string code;
        std::string message;
        std::string instance_id;
        std::optional<Venue> venue;
        std::string client_order_id;
        std::string request_id;
        std::optional<OrderEventContext> order;
    };

    [[nodiscard]] bool push(RawEntry entry) noexcept {
        if (!available_slots_.try_acquire()) return false;
        try {
            {
                std::scoped_lock lock(producer_mutex_);
                ring_[tail_ % capacity_].emplace(std::move(entry));
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

    void run(std::stop_token token) {
        while (true) {
            available_items_.acquire();
            if (token.stop_requested() &&
                pending_.load(std::memory_order_acquire) == 0) break;

            // Consumer owns head_ — no lock needed.
            auto& slot = ring_[head_ % capacity_];
            RawEntry entry = std::move(*slot);
            slot.reset();
            ++head_;
            available_slots_.release();

            OperationalEvent event{
                .occurred_at_ms  = entry.occurred_at_ms,
                .severity        = entry.severity,
                .category        = std::move(entry.category),
                .code            = std::move(entry.code),
                .message         = std::move(entry.message),
                .instance_id     = std::move(entry.instance_id),
                .venue           = entry.venue,
                .client_order_id = std::move(entry.client_order_id),
                .request_id      = std::move(entry.request_id),
                .order           = std::move(entry.order),
            };
            try {
                completion_(store_->append_event(std::move(event)), {});
            } catch (const std::exception& e) {
                completion_(std::nullopt, e.what());
            } catch (...) {
                completion_(std::nullopt, "unknown operational logging failure");
            }

            if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1)
                pending_.notify_all();
        }
        pending_.notify_all();
    }

    std::shared_ptr<IOrderStore> store_;
    Completion completion_;
    const std::size_t capacity_;
    std::vector<std::optional<RawEntry>> ring_;
    std::counting_semaphore<> available_slots_;
    std::counting_semaphore<> available_items_{0};

    std::mutex producer_mutex_;
    alignas(64) std::size_t tail_{0};

    alignas(64) std::size_t head_{0};

    std::atomic<std::size_t> pending_{0};

    std::jthread worker_;
};

} // namespace abex
