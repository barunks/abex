#include "abex/domain/order_state_machine.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace abex;

namespace {

Order live_order() {
    auto order = make_order(test::limit_order("state-1"));
    order.status = OrderStatus::Live;
    order.pending_action = PendingAction::None;
    return order;
}

ExecutionReport report(std::string id,
                       OrderStatus status,
                       std::string filled,
                       std::uint64_t sequence) {
    return {
        .event_id = std::move(id),
        .client_order_id = "state-1",
        .exchange_order_id = "venue-1",
        .status = status,
        .cumulative_filled = Decimal::parse(filled),
        .last_fill_price = Decimal::parse("50000"),
        .sequence = sequence,
        .event_time_ms = unix_time_ms(),
    };
}

} // namespace

TEST_CASE("state machine applies fills and terminal states monotonically", "[state]") {
    auto order = live_order();
    CHECK(OrderStateMachine::apply(
              order, report("event-1", OrderStatus::PartiallyFilled, "0.04", 1))
              .state_changed);
    CHECK(order.status == OrderStatus::PartiallyFilled);
    CHECK(order.filled_quantity == Decimal::parse("0.04"));
    CHECK(order.average_fill_price == Decimal::parse("50000"));

    CHECK(OrderStateMachine::apply(order, report("event-2", OrderStatus::Filled, "0.1", 2))
              .state_changed);
    CHECK(order.status == OrderStatus::Filled);
    CHECK(order.filled_quantity == order.quantity);

    (void)OrderStateMachine::apply(order, report("event-3", OrderStatus::Canceled, "0.1", 3));
    CHECK(order.status == OrderStatus::Filled);
}

TEST_CASE("full fill dominates pending cancel and every late cancel report",
          "[state][race]") {
    auto order = live_order();
    order.pending_action = PendingAction::Cancel;
    REQUIRE(OrderStateMachine::apply(
                order, report("fill-wins", OrderStatus::Filled, "0.1", 20))
                .state_changed);
    CHECK(order.status == OrderStatus::Filled);
    CHECK(order.pending_action == PendingAction::None);

    const auto late_cancel = OrderStateMachine::apply(
        order, report("late-cancel", OrderStatus::Canceled, "0.1", 21));
    CHECK(late_cancel.disposition == ApplyDisposition::Applied);
    CHECK(order.status == OrderStatus::Filled);
    CHECK(order.filled_quantity == order.quantity);
}

TEST_CASE("duplicate execution reports are no-ops", "[state]") {
    auto order = live_order();
    const auto update = report("same-event", OrderStatus::PartiallyFilled, "0.02", 1);
    (void)OrderStateMachine::apply(order, update);
    const auto version = order.version;
    const auto result = OrderStateMachine::apply(order, update);
    CHECK(result.disposition == ApplyDisposition::Duplicate);
    CHECK_FALSE(result.state_changed);
    CHECK(order.version == version);
}

TEST_CASE("out-of-order reports merge fills without accepting stale status", "[state]") {
    auto order = live_order();
    (void)OrderStateMachine::apply(order, report("newer", OrderStatus::Live, "0", 10));
    const auto result = OrderStateMachine::apply(
        order, report("older", OrderStatus::Canceled, "0.03", 8));
    CHECK(result.disposition == ApplyDisposition::Stale);
    CHECK(order.status == OrderStatus::PartiallyFilled);
    CHECK(order.filled_quantity == Decimal::parse("0.03"));
    CHECK(order.last_sequence == 10);
}

TEST_CASE("invalid cumulative quantities are rejected", "[state]") {
    auto order = live_order();
    auto invalid = report("invalid", OrderStatus::Filled, "0.2", 1);
    const auto result = OrderStateMachine::apply(order, invalid);
    CHECK(result.disposition == ApplyDisposition::Invalid);
    CHECK(order.status == OrderStatus::Live);
}

TEST_CASE("historical replacement reports merge fills without canceling the active order",
          "[state][replacement]") {
    auto order = live_order();
    order.exchange_order_id = "new-2";
    order.exchange_order_id_aliases.insert("old-1");
    order.exchange_fill_offsets["old-1"] = Decimal{};
    order.exchange_fill_offsets["new-2"] = Decimal::parse("0.03");
    order.exchange_quote_offsets["new-2"] = Decimal::parse("1500");
    order.filled_quantity = Decimal::parse("0.03");
    order.cumulative_quote = Decimal::parse("1500");
    order.status = OrderStatus::PartiallyFilled;
    order.pending_action = PendingAction::Amend;
    order.pending_amend_price = Decimal::parse("49000");
    order.pending_amend_quantity = Decimal::parse("0.1");

    auto old_cancel = report("old-cancel", OrderStatus::Canceled, "0.04", 11);
    old_cancel.exchange_order_id = "old-1";
    old_cancel.order_price = Decimal::parse("50000");
    old_cancel.order_quantity = Decimal::parse("0.1");
    REQUIRE(OrderStateMachine::apply(order, old_cancel).disposition ==
            ApplyDisposition::Applied);
    CHECK(order.exchange_order_id == "new-2");
    CHECK(order.status == OrderStatus::PartiallyFilled);
    CHECK(order.filled_quantity == Decimal::parse("0.04"));
    CHECK(order.pending_action == PendingAction::Amend);

    auto replacement = report("replacement-live", OrderStatus::Live, "0.02", 12);
    replacement.exchange_order_id = "new-2";
    replacement.order_price = Decimal::parse("49000");
    replacement.order_quantity = Decimal::parse("0.07");
    REQUIRE(OrderStateMachine::apply(order, replacement).disposition ==
            ApplyDisposition::Applied);
    CHECK(order.exchange_order_id == "new-2");
    CHECK(order.filled_quantity == Decimal::parse("0.05"));
    CHECK(order.quantity == Decimal::parse("0.1"));
    CHECK(order.price == Decimal::parse("49000"));
    CHECK(order.pending_action == PendingAction::None);
}
