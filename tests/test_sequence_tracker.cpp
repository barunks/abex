#include "abex/application/sequence_tracker.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace abex;

TEST_CASE("sequence tracker classifies continuity and gaps", "[sequence]") {
    SequenceTracker tracker;
    CHECK(tracker.observe(10).observation == SequenceObservation::First);
    CHECK(tracker.observe(11).observation == SequenceObservation::Contiguous);
    CHECK(tracker.observe(11).observation == SequenceObservation::Duplicate);
    CHECK(tracker.observe(9).observation == SequenceObservation::Stale);
    const auto gap = tracker.observe(14);
    CHECK(gap.observation == SequenceObservation::Gap);
    CHECK(gap.expected == 12);
    CHECK(tracker.gap_detected());
    tracker.clear_gap();
    CHECK_FALSE(tracker.gap_detected());
}
