#include "abex/domain/order_state_machine.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <random>

using namespace abex;

TEST_CASE("randomized state transitions preserve order invariants", "[property][state]") {
    std::mt19937_64 random{0xabc123U};
    std::uniform_int_distribution<int> status_distribution(0, 4);
    std::uniform_int_distribution<int> fill_distribution(0, 100);
    std::uniform_int_distribution<int> sequence_distribution(1, 500);

    for (int trial = 0; trial < 100; ++trial) {
        auto order = make_order(test::limit_order("property-" + std::to_string(trial),
                                                  Venue::Okx, Side::Buy, "1", "100"));
        order.status = OrderStatus::Live;
        order.pending_action = PendingAction::None;
        bool ever_filled = false;
        for (int step = 0; step < 200; ++step) {
            const auto status_index = status_distribution(random);
            const OrderStatus statuses[] = {
                OrderStatus::Live, OrderStatus::PartiallyFilled, OrderStatus::Filled,
                OrderStatus::Canceled, OrderStatus::Rejected};
            const auto fill = Decimal::from_raw(
                static_cast<std::int64_t>(fill_distribution(random)) * Decimal::scale / 100);
            ExecutionReport report{
                .event_id = "event-" + std::to_string(trial) + '-' + std::to_string(step),
                .client_order_id = order.client_order_id,
                .status = statuses[status_index],
                .cumulative_filled = fill,
                .last_fill_price = Decimal::parse("100"),
                .sequence = static_cast<std::uint64_t>(sequence_distribution(random)),
                .event_time_ms = unix_time_ms(),
            };
            (void)OrderStateMachine::apply(order, report);
            CHECK(order.filled_quantity >= Decimal{});
            CHECK(order.filled_quantity <= order.quantity);
            if (ever_filled) CHECK(order.status == OrderStatus::Filled);
            ever_filled = ever_filled || order.status == OrderStatus::Filled;
            if (order.status == OrderStatus::Filled) CHECK(order.filled_quantity == order.quantity);
        }
    }
}
