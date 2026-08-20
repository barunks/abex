#pragma once

#include "abex/domain/order.hpp"
#include "abex/infrastructure/journal_serializer.hpp"
#include "abex/ports/order_store.hpp"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace abex {

// Moves journal writes off the critical path.
//
// Two producers: the gateway caller thread (place/cancel/amend) and the
// SpscExecutionLane worker (apply_execution). A mutex serializes only the
// slot-claim (tail increment + move-into-slot) — O(1) hold time, no heap.
// The consumer owns head_ exclusively and never touches producer_mutex_.
// Cache-line padding separates tail and head.
class AsyncJournalLane final {
public:
    struct Entry {
        std::shared_ptr<const Order> order;
        bool intent_only{false};
        std::uint64_t sequence{0};
    };

    explicit AsyncJournalLane(std::shared_ptr<IOrderStore> store, std::size_t capacity = 8192)
        : store_(std::move(store)), capacity_(capacity),
          ring_(capacity),
          available_slots_(static_cast<std::ptrdiff_t>(capacity)),
          worker_([this](std::stop_token token) { run(token); }) {
        if (!store_) throw std::invalid_argument("journal lane store is required");
        if (capacity_ == 0) throw std::invalid_argument("journal lane capacity is required");
    }

    ~AsyncJournalLane() {
        flush();
        worker_.request_stop();
        available_items_.release();
    }

    AsyncJournalLane(const AsyncJournalLane&) = delete;
    AsyncJournalLane& operator=(const AsyncJournalLane&) = delete;

    // Returns false if the queue is full — caller falls back to synchronous write.
    // Serialization is deferred to the worker thread — caller only posts the
    // shared_ptr and sequence number (no string construction on hot path).
    [[nodiscard]] bool post(std::shared_ptr<const Order> order, bool intent_only, std::uint64_t sequence) noexcept {
        if (!order) return false;
        if (!available_slots_.try_acquire()) return false;
        try {
            {
                std::scoped_lock lock(producer_mutex_);
                ring_[tail_ % capacity_].emplace(Entry{std::move(order), intent_only, sequence});
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

private:
    void run(std::stop_token token) {
        while (true) {
            available_items_.acquire();
            if (token.stop_requested() &&
                pending_.load(std::memory_order_acquire) == 0) break;

            // Consumer owns head_ — no lock needed.
            auto& slot = ring_[head_ % capacity_];
            Entry entry = std::move(*slot);
            slot.reset();
            ++head_;
            available_slots_.release();

            try {
                std::string payload;
                payload.reserve(512);
                JsonSerializer::write_order(payload, *entry.order, entry.intent_only);
                store_->commit_order(*entry.order, std::move(payload), entry.sequence);
            } catch (...) {}

            if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1)
                pending_.notify_all();
        }
        pending_.notify_all();
    }

    std::shared_ptr<IOrderStore> store_;
    const std::size_t capacity_;
    std::vector<std::optional<Entry>> ring_;
    std::counting_semaphore<> available_slots_;
    std::counting_semaphore<> available_items_{0};

    std::mutex producer_mutex_;
    alignas(64) std::size_t tail_{0};

    alignas(64) std::size_t head_{0};

    std::atomic<std::size_t> pending_{0};

    std::jthread worker_;
};

} // namespace abex
