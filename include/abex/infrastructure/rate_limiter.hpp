#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <mutex>

namespace abex {

class TokenBucket final {
public:
    TokenBucket(double capacity, double tokens_per_second)
        : capacity_(capacity), tokens_(capacity), refill_per_second_(tokens_per_second),
          last_refill_(Clock::now()) {}

    [[nodiscard]] bool try_acquire(double cost = 1.0) {
        std::scoped_lock lock(mutex_);
        refill();
        if (cost <= 0.0 || tokens_ < cost) return false;
        tokens_ -= cost;
        return true;
    }

    [[nodiscard]] double available() {
        std::scoped_lock lock(mutex_);
        refill();
        return tokens_;
    }

    void synchronize(double capacity, double available, double tokens_per_second) {
        if (capacity <= 0.0 || tokens_per_second <= 0.0) return;
        std::scoped_lock lock(mutex_);
        capacity_ = capacity;
        tokens_ = std::clamp(available, 0.0, capacity_);
        refill_per_second_ = tokens_per_second;
        last_refill_ = Clock::now();
    }

private:
    using Clock = std::chrono::steady_clock;

    void refill() {
        const auto now = Clock::now();
        const auto elapsed = std::chrono::duration<double>(now - last_refill_).count();
        tokens_ = std::min(capacity_, tokens_ + elapsed * refill_per_second_);
        last_refill_ = now;
    }

    double capacity_;
    double tokens_;
    double refill_per_second_;
    Clock::time_point last_refill_;
    std::mutex mutex_;
};

} // namespace abex
