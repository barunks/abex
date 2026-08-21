#pragma once

// Lock-free MPSC (multi-producer, single-consumer) bounded ring.
//
// Based on Dmitry Vyukov's bounded MPSC queue.
//
// Each slot has a sequence counter initialised to its index.
//
// Producer protocol (try_push):
//   1. try_acquire available_slots_  (backpressure, non-blocking)
//   2. fetch_add tail_ to claim a position
//   3. Spin-wait until slot.sequence == pos  (consumer recycled this slot)
//   4. Write value into slot, store sequence = pos + 1  (publish)
//   5. release available_items_  (wake consumer)
//
// Consumer protocol (single thread):
//   pop_item() acquires available_items_ then reads the slot.
//   Because available_items_ is released only AFTER sequence is stored,
//   the sequence check always succeeds immediately — no spurious wakes
//   from real pushes. The only spurious wake is from wake_consumer()
//   (stop sentinel), which does NOT write a slot. pop_item() detects
//   this by checking sequence and returns {false, T{}} in that case,
//   WITHOUT advancing head_ or releasing available_slots_.
//
// Stop protocol:
//   Caller: flush() -> request_stop() -> wake_consumer()
//   Worker: pop_item() returns {false,_} -> checks stop_token -> exits
//   Drain:  try_pop() drains any items that arrived before stop.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace abex {

template <typename T>
class MpscRing final {
public:
    explicit MpscRing(std::size_t capacity)
        : capacity_(capacity), mask_(capacity - 1), slots_(capacity),
          available_slots_(static_cast<std::ptrdiff_t>(capacity)) {
        if (capacity == 0 || (capacity & (capacity - 1)) != 0)
            throw std::invalid_argument("MpscRing capacity must be a power of two");
        for (std::size_t i = 0; i < capacity; ++i)
            slots_[i].sequence.store(static_cast<std::uint64_t>(i),
                                     std::memory_order_relaxed);
    }

    MpscRing(const MpscRing&) = delete;
    MpscRing& operator=(const MpscRing&) = delete;

    // Producer: one fetch_add + spin-wait (0 iters in practice) + release-store.
    // Returns false immediately if ring is full.
    [[nodiscard]] bool try_push(T value) noexcept {
        if (!available_slots_.try_acquire()) return false;
        const auto pos = tail_.fetch_add(1, std::memory_order_relaxed);
        auto& slot = slots_[pos & mask_];
        // Wait until the consumer has recycled this slot for our turn.
        // Spins 0 times in the common case because available_slots_ ensures
        // we never lap the consumer.
        while (slot.sequence.load(std::memory_order_acquire) != pos)
            std::this_thread::yield();
        slot.value.emplace(std::move(value));
        slot.sequence.store(pos + 1, std::memory_order_release); // publish
        available_items_.release();
        return true;
    }

    // Consumer: block until available_items_ is released, then try to consume.
    // Returns {true, value} for a real item.
    // Returns {false, T{}} for a stop-sentinel wake (wake_consumer() was called).
    // Caller must loop on false and check stop_token.
    [[nodiscard]] std::pair<bool, T> pop_item() {
        available_items_.acquire();
        const auto pos = head_;
        auto& slot = slots_[pos & mask_];
        if (slot.sequence.load(std::memory_order_acquire) != pos + 1) {
            // Stop sentinel: wake_consumer() released available_items_ without
            // writing a slot. Do NOT advance head_ or release available_slots_.
            return {false, T{}};
        }
        T value = std::move(*slot.value);
        slot.value.reset();
        slot.sequence.store(pos + capacity_, std::memory_order_release); // recycle
        ++head_;
        available_slots_.release();
        return {true, std::move(value)};
    }

    // Non-blocking consumer attempt. Returns false if nothing available.
    [[nodiscard]] bool try_pop(T& out) noexcept {
        if (!available_items_.try_acquire()) return false;
        const auto pos = head_;
        auto& slot = slots_[pos & mask_];
        if (slot.sequence.load(std::memory_order_acquire) != pos + 1) {
            // Spurious wake sentinel consumed — don't advance head_.
            return false;
        }
        out = std::move(*slot.value);
        slot.value.reset();
        slot.sequence.store(pos + capacity_, std::memory_order_release);
        ++head_;
        available_slots_.release();
        return true;
    }

    // Release available_items_ without writing a slot — unblocks a parked
    // consumer for a stop check. pop_item() will return {false, T{}}.
    void wake_consumer() noexcept { available_items_.release(); }

private:
    struct Slot {
        std::atomic<std::uint64_t> sequence{0};
        std::optional<T> value;
    };

    const std::size_t capacity_;
    const std::size_t mask_;
    std::vector<Slot> slots_;
    std::counting_semaphore<> available_slots_;
    std::counting_semaphore<> available_items_{0};

    alignas(64) std::atomic<std::uint64_t> tail_{0};
    alignas(64) std::uint64_t head_{0}; // consumer-private
};

} // namespace abex
