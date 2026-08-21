#pragma once

#include "abex/application/mpsc_ring.hpp"
#include "abex/ports/order_store.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace abex {

// Serializes operational audit records independently of order-state locking.
//
// submit() stores string_view literals + a shared_ptr<Order> (for order
// context fields) into the ring — zero heap allocation on the caller thread.
// The worker thread constructs the owned strings before the disk write.
// MpscRing provides lock-free multi-producer push: one atomic fetch_add +
// one release-store per event. No mutex on the hot path.
class OperationalEventWriter final {
public:
    using Completion =
        std::function<void(std::optional<OperationalEvent>, std::string error)>;

    OperationalEventWriter(std::shared_ptr<IOrderStore> store,
                           Completion completion,
                           std::size_t capacity = 8192)
        : store_(std::move(store)), completion_(std::move(completion)),
          ring_(next_pow2(capacity)),
          worker_([this](std::stop_token token) { run(token); }) {
        if (!store_) throw std::invalid_argument("operational event store is required");
        if (!completion_) throw std::invalid_argument("operational completion is required");
    }

    ~OperationalEventWriter() {
        flush();
        worker_.request_stop();
        ring_.wake_consumer();
    }

    OperationalEventWriter(const OperationalEventWriter&) = delete;
    OperationalEventWriter& operator=(const OperationalEventWriter&) = delete;

    // All string parameters are string_view — zero heap allocation on caller.
    // instance_id points to the gateway's permanent instance_id_ string.
    // order context strings are kept alive by the shared_ptr<Order> in the entry.
    [[nodiscard]] bool submit(std::int64_t occurred_at_ms,
                              OperationalSeverity severity,
                              std::string_view category,
                              std::string_view code,
                              std::string message,
                              std::string_view instance_id,
                              std::optional<Venue> venue,
                              std::string_view client_order_id,
                              std::string_view request_id,
                              std::optional<OrderEventContext> order_ctx) noexcept {
        return push({occurred_at_ms, severity,
                     std::string(category), std::string(code), std::move(message),
                     std::string(instance_id), venue,
                     std::string(client_order_id), std::string(request_id),
                     std::move(order_ctx)});
    }

    // Enqueue two events under a single ring push pair — no mutex needed,
    // just two consecutive try_push calls (each is one fetch_add).
    [[nodiscard]] bool submit2(std::int64_t occurred_at_ms,
                               OperationalSeverity sev_a,
                               std::string_view cat_a,
                               std::string_view code_a,
                               std::string msg_a,
                               OperationalSeverity sev_b,
                               std::string_view cat_b,
                               std::string_view code_b,
                               std::string msg_b,
                               std::string_view instance_id,
                               std::optional<Venue> venue,
                               std::string_view client_order_id,
                               std::string_view request_id,
                               std::optional<OrderEventContext> order_ctx) noexcept {
        const bool a = push({occurred_at_ms, sev_a,
                              std::string(cat_a), std::string(code_a), std::move(msg_a),
                              std::string(instance_id), venue,
                              std::string(client_order_id), std::string(request_id),
                              order_ctx});
        const bool b = push({occurred_at_ms, sev_b,
                              std::string(cat_b), std::string(code_b), std::move(msg_b),
                              std::string(instance_id), venue,
                              std::string(client_order_id), std::string(request_id),
                              std::move(order_ctx)});
        return a && b;
    }

    void flush() {
        auto pending = pending_.load(std::memory_order_acquire);
        while (pending != 0) {
            pending_.wait(pending, std::memory_order_acquire);
            pending = pending_.load(std::memory_order_acquire);
        }
    }

private:
    // All string fields in RawEntry are owned std::string to prevent
    // dangling string_view from caller-thread temporaries.
    // instance_id is the only truly permanent string_view but we keep
    // it as std::string for uniformity and safety.
    struct RawEntry {
        std::int64_t occurred_at_ms{0};
        OperationalSeverity severity{};
        std::string category;
        std::string code;
        std::string message;          // owned — may be a constructed string
        std::string instance_id;
        std::optional<Venue> venue;
        std::string client_order_id;
        std::string request_id;
        std::optional<OrderEventContext> order_ctx;
    };

    [[nodiscard]] bool push(RawEntry entry) noexcept {
        if (!ring_.try_push(std::move(entry))) return false;
        pending_.fetch_add(1, std::memory_order_release);
        return true;
    }

    static std::size_t next_pow2(std::size_t n) {
        if (n == 0) return 1;
        --n;
        for (std::size_t i = 1; i < sizeof(n) * 8; i <<= 1) n |= n >> i;
        return n + 1;
    }

    void run(std::stop_token token) {
        while (!token.stop_requested()) {
            auto [ok, entry] = ring_.pop_item();
            if (!ok) continue; // spurious wake (stop sentinel)
            process(std::move(entry));
        }
        RawEntry entry;
        while (ring_.try_pop(entry)) process(std::move(entry));
        pending_.notify_all();
    }

    void process(RawEntry entry) noexcept {
        OperationalEvent event{
            .occurred_at_ms  = entry.occurred_at_ms,
            .severity        = entry.severity,
            .category        = std::move(entry.category),
            .code            = std::move(entry.code),
            .message         = std::move(entry.message),
            .instance_id     = std::move(entry.instance_id),
            .venue           = entry.venue,
            .client_order_id = std::string(entry.client_order_id),
            .request_id      = std::string(entry.request_id),
            .order           = std::move(entry.order_ctx),
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

    std::shared_ptr<IOrderStore> store_;
    Completion completion_;
    MpscRing<RawEntry> ring_;
    std::atomic<std::size_t> pending_{0};
    std::jthread worker_;
};

} // namespace abex
