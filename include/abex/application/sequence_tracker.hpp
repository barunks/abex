#pragma once

#include <cstdint>
#include <optional>

namespace abex {

enum class SequenceObservation { First, Contiguous, Duplicate, Stale, Gap };

struct SequenceResult {
    SequenceObservation observation{SequenceObservation::First};
    std::optional<std::uint64_t> expected;
    std::uint64_t received{0};
};

class SequenceTracker final {
public:
    [[nodiscard]] SequenceResult observe(std::uint64_t sequence) {
        if (!last_) {
            last_ = sequence;
            return {.observation = SequenceObservation::First,
                    .expected = std::nullopt,
                    .received = sequence};
        }
        if (sequence == *last_) {
            return {.observation = SequenceObservation::Duplicate,
                    .expected = *last_ + 1,
                    .received = sequence};
        }
        if (sequence < *last_) {
            return {.observation = SequenceObservation::Stale,
                    .expected = *last_ + 1,
                    .received = sequence};
        }
        const auto expected = *last_ + 1;
        const bool gap = sequence > expected;
        last_ = sequence;
        gap_detected_ = gap_detected_ || gap;
        return {.observation = gap ? SequenceObservation::Gap : SequenceObservation::Contiguous,
                .expected = expected,
                .received = sequence};
    }

    void reset(std::optional<std::uint64_t> last = std::nullopt) noexcept {
        last_ = last;
        gap_detected_ = false;
    }

    [[nodiscard]] std::optional<std::uint64_t> last() const noexcept { return last_; }
    [[nodiscard]] bool gap_detected() const noexcept { return gap_detected_; }
    void clear_gap() noexcept { gap_detected_ = false; }

private:
    std::optional<std::uint64_t> last_;
    bool gap_detected_{false};
};

} // namespace abex
