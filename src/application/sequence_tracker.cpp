#include "abex/application/sequence_tracker.hpp"

namespace abex {

SequenceResult SequenceTracker::observe(std::uint64_t sequence) {
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

void SequenceTracker::reset(std::optional<std::uint64_t> last) noexcept {
    last_ = last;
    gap_detected_ = false;
}

} // namespace abex
