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
    [[nodiscard]] SequenceResult observe(std::uint64_t sequence);
    void reset(std::optional<std::uint64_t> last = std::nullopt) noexcept;
    [[nodiscard]] std::optional<std::uint64_t> last() const noexcept { return last_; }
    [[nodiscard]] bool gap_detected() const noexcept { return gap_detected_; }
    void clear_gap() noexcept { gap_detected_ = false; }

private:
    std::optional<std::uint64_t> last_;
    bool gap_detected_{false};
};

} // namespace abex
