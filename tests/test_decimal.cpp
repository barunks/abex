#include "abex/domain/decimal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

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

TEST_CASE("decimal detects every fixed-point overflow boundary", "[decimal][overflow]") {
    const auto maximum = Decimal::from_raw(std::numeric_limits<std::int64_t>::max());
    const auto minimum = Decimal::from_raw(std::numeric_limits<std::int64_t>::min());

    CHECK(Decimal::parse("92233720368.54775807") == maximum);
    CHECK(Decimal::parse("-92233720368.54775808") == minimum);
    CHECK_THROWS_AS(Decimal::parse("92233720368.54775808"), std::overflow_error);
    CHECK_THROWS_AS(Decimal::parse("-92233720368.54775809"), std::overflow_error);
    CHECK_THROWS_AS(maximum + Decimal::from_raw(1), std::overflow_error);
    CHECK_THROWS_AS(minimum - Decimal::from_raw(1), std::overflow_error);
    CHECK_THROWS_AS(maximum * Decimal::from_integer(2), std::overflow_error);
    CHECK_THROWS_AS(maximum / Decimal::parse("0.5"), std::overflow_error);
    CHECK_THROWS_AS(-minimum, std::overflow_error);
    CHECK_THROWS_AS(minimum.abs(), std::overflow_error);
    CHECK_THROWS_AS(Decimal::from_integer(100'000'000'000LL), std::overflow_error);
}

TEST_CASE("decimal stack formatting covers signed limits without allocation contracts",
          "[decimal][format]") {
    constexpr auto compile_time = Decimal::literal("-123.45678901");
    static_assert(compile_time.raw() == -12'345'678'901LL);

    std::array<char, Decimal::maximum_formatted_size> buffer{};
    CHECK(Decimal::from_raw(std::numeric_limits<std::int64_t>::min()).format_to(buffer) ==
          "-92233720368.54775808");
    CHECK_THROWS_AS(Decimal::parse("1").format_to(std::span<char>(buffer).first(4)),
                    std::length_error);
}
