#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace abex {

// Fixed-point decimal used for every monetary value. It avoids binary floating-point
// drift and provides deterministic serialization across processes and restarts.
class Decimal final {
public:
    static constexpr std::int64_t scale = 100'000'000;
    static constexpr int precision = 8;
    static constexpr std::size_t maximum_formatted_size = 32;

    constexpr Decimal() noexcept = default;

    [[nodiscard]] static Decimal parse(std::string_view text);
    [[nodiscard]] static consteval Decimal literal(std::string_view text) {
        if (text.empty()) throw "decimal literal is empty";
        bool negative = false;
        if (text.front() == '+' || text.front() == '-') {
            negative = text.front() == '-';
            text.remove_prefix(1);
        }
        if (text.empty()) throw "decimal literal has no digits";

        constexpr auto positive_limit =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        constexpr auto negative_limit = positive_limit + 1U;
        const auto raw_limit = negative ? negative_limit : positive_limit;
        std::uint64_t whole = 0;
        std::uint64_t fraction = 0;
        int fraction_digits = 0;
        bool decimal_point = false;
        bool any_digit = false;
        for (const char character : text) {
            if (character == '.') {
                if (decimal_point) throw "decimal literal has multiple decimal points";
                decimal_point = true;
                continue;
            }
            if (character < '0' || character > '9') {
                throw "decimal literal contains invalid characters";
            }
            any_digit = true;
            if (decimal_point) {
                if (++fraction_digits > precision) throw "decimal literal has excess precision";
                fraction = fraction * 10 + static_cast<std::uint64_t>(character - '0');
            } else {
                const auto digit = static_cast<std::uint64_t>(character - '0');
                if (whole > (raw_limit - digit) / 10U) {
                    throw "decimal literal is out of range";
                }
                whole = whole * 10U + digit;
            }
        }
        if (!any_digit) throw "decimal literal has no digits";
        while (fraction_digits++ < precision) fraction *= 10;
        if (whole > raw_limit / static_cast<std::uint64_t>(scale)) {
            throw "decimal literal is out of range";
        }
        auto raw = whole * static_cast<std::uint64_t>(scale);
        if (fraction > raw_limit - raw) throw "decimal literal is out of range";
        raw += fraction;
        if (!negative) return from_raw(static_cast<std::int64_t>(raw));
        if (raw == negative_limit) return from_raw(std::numeric_limits<std::int64_t>::min());
        return from_raw(-static_cast<std::int64_t>(raw));
    }
    [[nodiscard]] static constexpr Decimal from_raw(std::int64_t raw) noexcept {
        return Decimal(raw, RawTag{});
    }
    [[nodiscard]] static constexpr Decimal from_integer(std::int64_t value) {
        if (value > std::numeric_limits<std::int64_t>::max() / scale ||
            value < std::numeric_limits<std::int64_t>::min() / scale) {
            throw std::overflow_error("decimal integer conversion overflow");
        }
        return Decimal(value * scale, RawTag{});
    }

    [[nodiscard]] constexpr std::int64_t raw() const noexcept { return raw_; }
    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] std::string_view format_to(std::span<char> buffer) const;
    void append_to(std::string& destination) const;
    [[nodiscard]] constexpr bool is_zero() const noexcept { return raw_ == 0; }
    [[nodiscard]] constexpr bool is_positive() const noexcept { return raw_ > 0; }
    [[nodiscard]] Decimal abs() const;

    [[nodiscard]] Decimal operator-() const;
    Decimal& operator+=(Decimal rhs);
    Decimal& operator-=(Decimal rhs);

    friend Decimal operator+(Decimal lhs, Decimal rhs) { return lhs += rhs; }
    friend Decimal operator-(Decimal lhs, Decimal rhs) { return lhs -= rhs; }
    friend Decimal operator*(Decimal lhs, Decimal rhs);
    friend Decimal operator/(Decimal lhs, Decimal rhs);

    auto operator<=>(const Decimal&) const = default;

private:
    struct RawTag {};
    constexpr Decimal(std::int64_t raw, RawTag) noexcept : raw_(raw) {}

    std::int64_t raw_{0};
};

} // namespace abex
