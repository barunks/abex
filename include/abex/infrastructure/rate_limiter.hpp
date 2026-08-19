#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>

namespace abex {

// Lock-free Generic Cell Rate Algorithm (GCRA) limiter. Configuration values
// are converted once to integer micro-tokens and nanoseconds; the operation path
// uses one 64-bit CAS and deterministic integer arithmetic.
class TokenBucket final {
public:
    TokenBucket(double capacity, double tokens_per_second) {
        configure(capacity, capacity, tokens_per_second);
    }

    [[nodiscard]] bool try_acquire(double cost = 1.0) noexcept {
        const auto cost_units = token_units(cost);
        const auto capacity = capacity_units_.load(std::memory_order_acquire);
        const auto period = period_ns_.load(std::memory_order_acquire);
        if (cost_units <= 0 || cost_units > capacity || period <= 0) return false;

        const auto increment = scaled_duration(cost_units, period);
        const auto tolerance = scaled_duration(capacity - cost_units, period);
        const auto now = now_ns();
        auto observed = theoretical_arrival_ns_.load(std::memory_order_relaxed);
        for (;;) {
            if (observed > saturated_add(now, tolerance)) return false;
            const auto candidate = saturated_add(std::max(observed, now), increment);
            if (theoretical_arrival_ns_.compare_exchange_weak(
                    observed, candidate, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    [[nodiscard]] double available() const noexcept {
        const auto capacity = capacity_units_.load(std::memory_order_acquire);
        const auto period = period_ns_.load(std::memory_order_acquire);
        if (capacity <= 0 || period <= 0) return 0.0;
        const auto backlog = std::max<std::int64_t>(
            0, theoretical_arrival_ns_.load(std::memory_order_acquire) - now_ns());
        const auto consumed_value = std::ceil(
            static_cast<long double>(backlog) * units_per_token /
            static_cast<long double>(period));
        const auto consumed = consumed_value >= std::numeric_limits<std::int64_t>::max()
                                  ? std::numeric_limits<std::int64_t>::max()
                                  : static_cast<std::int64_t>(consumed_value);
        return static_cast<double>(std::max<std::int64_t>(0, capacity - consumed)) /
               static_cast<double>(units_per_token);
    }

    void synchronize(double capacity, double available, double tokens_per_second) noexcept {
        if (capacity <= 0.0 || tokens_per_second <= 0.0) return;
        configure(capacity, std::clamp(available, 0.0, capacity), tokens_per_second);
    }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr std::int64_t units_per_token = 1'000'000;
    static_assert(std::atomic<std::int64_t>::is_always_lock_free,
                  "the rate limiter requires lock-free 64-bit atomics");

    [[nodiscard]] static std::int64_t now_ns() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   Clock::now().time_since_epoch())
            .count();
    }

    [[nodiscard]] static std::int64_t token_units(double value) noexcept {
        if (!std::isfinite(value) || value <= 0.0) return 0;
        const auto scaled = static_cast<long double>(value) * units_per_token;
        if (scaled >= std::numeric_limits<std::int64_t>::max()) {
            return std::numeric_limits<std::int64_t>::max();
        }
        return static_cast<std::int64_t>(std::llround(scaled));
    }

    [[nodiscard]] static std::int64_t scaled_duration(std::int64_t units,
                                                       std::int64_t period) noexcept {
        if (units <= 0 || period <= 0) return 0;
        const auto whole = units / units_per_token;
        const auto remainder = units % units_per_token;
        const auto maximum = std::numeric_limits<std::int64_t>::max();
        if (whole > maximum / period) return maximum;
        const auto base = whole * period;

        const auto period_whole = period / units_per_token;
        const auto period_remainder = period % units_per_token;
        if (period_whole != 0 && remainder > (maximum - base) / period_whole) {
            return maximum;
        }
        const auto partial = remainder * period_whole;
        const auto tail = remainder * period_remainder / units_per_token;
        if (tail > maximum - base - partial) return maximum;
        return base + partial + tail;
    }

    [[nodiscard]] static std::int64_t saturated_add(std::int64_t lhs,
                                                     std::int64_t rhs) noexcept {
        if (rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs) {
            return std::numeric_limits<std::int64_t>::max();
        }
        return lhs + rhs;
    }

    void configure(double capacity, double available, double tokens_per_second) noexcept {
        const auto capacity_units = token_units(capacity);
        const auto period_value = std::clamp<long double>(
            1'000'000'000.0L / tokens_per_second, 1.0L,
            static_cast<long double>(std::numeric_limits<std::int64_t>::max()));
        const auto period = static_cast<std::int64_t>(std::llround(period_value));
        const auto available_units = token_units(available);
        capacity_units_.store(capacity_units, std::memory_order_release);
        period_ns_.store(period, std::memory_order_release);
        const auto consumed = std::max<std::int64_t>(0, capacity_units - available_units);
        theoretical_arrival_ns_.store(
            saturated_add(now_ns(), scaled_duration(consumed, period)),
            std::memory_order_release);
    }

    std::atomic<std::int64_t> capacity_units_{0};
    std::atomic<std::int64_t> period_ns_{1};
    std::atomic<std::int64_t> theoretical_arrival_ns_{0};
};

} // namespace abex
