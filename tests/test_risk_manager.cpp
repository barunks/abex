#include "abex/application/risk_manager.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace abex;

TEST_CASE("risk manager enforces size notional and schema checks", "[risk]") {
    const auto risk = test::risk_manager();
    auto request = test::limit_order("risk-1");
    CHECK(risk.check_new(request, {}).accepted);

    request.quantity = Decimal::parse("2.1");
    CHECK(risk.check_new(request, {}).code == "MAX_ORDER_SIZE");

    request.quantity = Decimal::parse("2");
    request.price = Decimal::parse("110000");
    CHECK(risk.check_new(request, {}).code == "MAX_NOTIONAL");

    request.type = OrderType::Market;
    CHECK(risk.check_new(request, {}).code == "INVALID_ORDER");
    request.price.reset();
    CHECK(risk.check_new(request, {}).accepted);
}

TEST_CASE("position checks include worst-case open quantity", "[risk]") {
    const auto risk = test::risk_manager();
    auto existing = make_order(test::limit_order("existing", Venue::Okx, Side::Buy, "2"));
    existing.status = OrderStatus::PartiallyFilled;
    existing.filled_quantity = Decimal::parse("0.5");

    auto next = test::limit_order("next", Venue::Binance, Side::Buy, "2");
    CHECK(risk.check_new(next, {existing}).accepted);

    auto realized = make_order(test::limit_order("realized", Venue::Okx, Side::Buy, "0.1"));
    realized.status = OrderStatus::Filled;
    realized.filled_quantity = Decimal::parse("0.1");
    CHECK(risk.check_new(next, {existing, realized}).code == "POSITION_LIMIT");
}

TEST_CASE("amend cannot reduce below executed quantity", "[risk]") {
    const auto risk = test::risk_manager();
    auto order = make_order(test::limit_order("amend-risk"));
    order.status = OrderStatus::PartiallyFilled;
    order.filled_quantity = Decimal::parse("0.06");
    const auto decision = risk.check_amend(
        order, std::nullopt, Decimal::parse("0.05"), {order});
    CHECK_FALSE(decision.accepted);
    CHECK(decision.code == "INVALID_AMEND");
}
