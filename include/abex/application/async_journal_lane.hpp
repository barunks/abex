#pragma once

#include "abex/application/mpsc_ring.hpp"
#include "abex/domain/order.hpp"
#include "abex/infrastructure/journal_serializer.hpp"
#include "abex/ports/order_store.hpp"

#include <atomic>
#include <cstddef>
#include <memory>
#include <stop_token>
#include <thread>

namespace abex {

// Moves journal writes off the critical path.
//
// Two producers: the gateway caller thread (place/cancel/amend) and the
// SpscExecutionLane worker (apply_execution). Both post via MpscRing::try_push
// — one atomic fetch_add + one release-store, no mutex.
class AsyncJournalLane final {
public:
    struct Entry {
        std::shared_ptr<const Order> order;
        bool intent_only{false};
        std::uint64_t sequence{0};
    };

    explicit AsyncJournalLane(std::shared_ptr<IOrderStore> store, std::size_t capacity = 8192)
        : store_(std::move(store)),
          ring_(next_pow2(capacity)),
          worker_([this](std::stop_token token) { run(token); }) {
        if (!store_) throw std::invalid_argument("journal lane store is required");
    }

    ~AsyncJournalLane() {
        flush();
        worker_.request_stop();
        ring_.wake_consumer();
    }

    AsyncJournalLane(const AsyncJournalLane&) = delete;
    AsyncJournalLane& operator=(const AsyncJournalLane&) = delete;

    // One atomic fetch_add + one release-store. No mutex.
    [[nodiscard]] bool post(std::shared_ptr<const Order> order,
                            bool intent_only,
                            std::uint64_t sequence) noexcept {
        if (!order) return false;
        pending_.fetch_add(1, std::memory_order_release);
        if (!ring_.try_push(Entry{std::move(order), intent_only, sequence})) {
            if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1)
                pending_.notify_all();
            return false;
        }
        return true;
    }

    void flush() {
        auto pending = pending_.load(std::memory_order_acquire);
        while (pending != 0) {
            pending_.wait(pending, std::memory_order_acquire);
            pending = pending_.load(std::memory_order_acquire);
        }
    }

private:
    static std::size_t next_pow2(std::size_t n) {
        if (n == 0) return 1;
        --n;
        for (std::size_t i = 1; i < sizeof(n) * 8; i <<= 1) n |= n >> i;
        return n + 1;
    }

    void process(Entry entry) noexcept {
        try {
            std::string payload;
            payload.reserve(512);
            JsonSerializer::write_order(payload, *entry.order, entry.intent_only);
            store_->commit_order(*entry.order, std::move(payload), entry.sequence);
        } catch (...) {}
        if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1)
            pending_.notify_all();
    }

    void run(std::stop_token token) {
        while (!token.stop_requested()) {
            auto [ok, entry] = ring_.pop_item(token);
            if (!ok) continue; // spurious wake (stop sentinel)
            process(std::move(entry));
        }
        // Drain any items posted before stop was observed.
        Entry entry;
        while (ring_.try_pop(entry)) process(std::move(entry));
        pending_.notify_all();
    }

    std::shared_ptr<IOrderStore> store_;
    MpscRing<Entry> ring_;
    std::atomic<std::size_t> pending_{0};
    std::jthread worker_;
};

} // namespace abex
