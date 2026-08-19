#include "abex/domain/decimal.hpp"

#include <catch2/catch_test_macros.hpp>

using abex::Decimal;

TEST_CASE("decimal parsing and formatting is canonical", "[decimal]") {
    CHECK(Decimal::parse("0").to_string() == "0");
    CHECK(Decimal::parse("001.23000000").to_string() == "1.23");
    CHECK(Decimal::parse(".5").to_string() == "0.5");
    CHECK(Decimal::parse("-0.00000001").raw() == -1);
    CHECK(Decimal::parse("+42").to_string() == "42");
}

TEST_CASE("decimal rejects ambiguous or excessive precision", "[decimal]") {
    CHECK_THROWS_AS(Decimal::parse(""), std::invalid_argument);
    CHECK_THROWS_AS(Decimal::parse("1.2.3"), std::invalid_argument);
    CHECK_THROWS_AS(Decimal::parse("1e3"), std::invalid_argument);
    CHECK_THROWS_AS(Decimal::parse("0.123456789"), std::invalid_argument);
}

TEST_CASE("decimal arithmetic stays fixed point", "[decimal]") {
    CHECK((Decimal::parse("1.2") + Decimal::parse("3.45")).to_string() == "4.65");
    CHECK((Decimal::parse("5") - Decimal::parse("1.25")).to_string() == "3.75");
    CHECK((Decimal::parse("12.5") * Decimal::parse("0.2")).to_string() == "2.5");
    CHECK((Decimal::parse("10") / Decimal::parse("4")).to_string() == "2.5");
    CHECK_THROWS_AS(Decimal::parse("1") / Decimal{}, std::domain_error);
}
