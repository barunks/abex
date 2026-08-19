#include "abex/domain/decimal.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <stdexcept>

namespace abex {
namespace {

[[nodiscard]] std::int64_t checked_from_wide(__int128 value) {
    if (value > std::numeric_limits<std::int64_t>::max() ||
        value < std::numeric_limits<std::int64_t>::min()) {
        throw std::overflow_error("decimal arithmetic overflow");
    }
    return static_cast<std::int64_t>(value);
}

} // namespace

Decimal Decimal::parse(std::string_view text) {
    if (text.empty()) {
        throw std::invalid_argument("decimal value is empty");
    }

    bool negative = false;
    if (text.front() == '+' || text.front() == '-') {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text.empty()) {
        throw std::invalid_argument("decimal value has no digits");
    }

    const auto dot = text.find('.');
    if (dot != std::string_view::npos && text.find('.', dot + 1) != std::string_view::npos) {
        throw std::invalid_argument("decimal value has multiple decimal points");
    }

    const auto whole_text = text.substr(0, dot);
    const auto fraction_text = dot == std::string_view::npos
                                   ? std::string_view{}
                                   : text.substr(dot + 1);
    if (whole_text.empty() && fraction_text.empty()) {
        throw std::invalid_argument("decimal value has no digits");
    }
    if (fraction_text.size() > static_cast<std::size_t>(precision)) {
        throw std::invalid_argument("decimal supports at most 8 fractional digits");
    }

    auto parse_digits = [](std::string_view digits) -> std::int64_t {
        if (digits.empty()) {
            return 0;
        }
        std::int64_t value = 0;
        const auto [ptr, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (error != std::errc{} || ptr != digits.data() + digits.size()) {
            throw std::invalid_argument("decimal contains invalid characters");
        }
        return value;
    };

    const auto whole = parse_digits(whole_text);
    const auto fraction = parse_digits(fraction_text);
    std::int64_t fraction_scale = 1;
    for (std::size_t i = fraction_text.size(); i < static_cast<std::size_t>(precision); ++i) {
        fraction_scale *= 10;
    }

    __int128 raw = static_cast<__int128>(whole) * scale +
                   static_cast<__int128>(fraction) * fraction_scale;
    if (negative) {
        raw = -raw;
    }
    return from_raw(checked_from_wide(raw));
}

std::string Decimal::to_string() const {
    std::array<char, maximum_formatted_size> buffer{};
    const auto formatted = format_to(buffer);
    return std::string(formatted);
}

std::string_view Decimal::format_to(std::span<char> buffer) const {
    if (buffer.size() < maximum_formatted_size) {
        throw std::length_error("decimal formatting buffer is too small");
    }
    const bool negative = raw_ < 0;
    const auto magnitude = negative
        ? static_cast<std::uint64_t>(-(static_cast<__int128>(raw_)))
        : static_cast<std::uint64_t>(raw_);
    const auto whole = magnitude / static_cast<std::uint64_t>(scale);
    const auto fraction = magnitude % static_cast<std::uint64_t>(scale);

    auto* output = buffer.data();
    if (negative) *output++ = '-';
    const auto [whole_end, error] = std::to_chars(output, buffer.data() + buffer.size(), whole);
    if (error != std::errc{}) throw std::length_error("decimal formatting buffer is too small");
    output = whole_end;
    if (fraction == 0) return {buffer.data(), static_cast<std::size_t>(output - buffer.data())};

    *output++ = '.';
    auto divisor = static_cast<std::uint64_t>(scale / 10);
    for (int index = 0; index < precision; ++index) {
        *output++ = static_cast<char>('0' + (fraction / divisor) % 10);
        divisor /= 10;
    }
    while (output[-1] == '0') --output;
    return {buffer.data(), static_cast<std::size_t>(output - buffer.data())};
}

void Decimal::append_to(std::string& destination) const {
    std::array<char, maximum_formatted_size> buffer{};
    destination.append(format_to(buffer));
}

Decimal Decimal::abs() const {
    if (raw_ == std::numeric_limits<std::int64_t>::min()) {
        throw std::overflow_error("decimal absolute value overflow");
    }
    return from_raw(raw_ < 0 ? -raw_ : raw_);
}

Decimal Decimal::operator-() const {
    if (raw_ == std::numeric_limits<std::int64_t>::min()) {
        throw std::overflow_error("decimal negation overflow");
    }
    return from_raw(-raw_);
}

Decimal& Decimal::operator+=(Decimal rhs) {
    raw_ = checked_from_wide(static_cast<__int128>(raw_) + rhs.raw_);
    return *this;
}

Decimal& Decimal::operator-=(Decimal rhs) {
    raw_ = checked_from_wide(static_cast<__int128>(raw_) - rhs.raw_);
    return *this;
}

Decimal operator*(Decimal lhs, Decimal rhs) {
    const auto product = static_cast<__int128>(lhs.raw_) * rhs.raw_;
    return Decimal::from_raw(checked_from_wide(product / Decimal::scale));
}

Decimal operator/(Decimal lhs, Decimal rhs) {
    if (rhs.raw_ == 0) {
        throw std::domain_error("decimal division by zero");
    }
    const auto quotient = static_cast<__int128>(lhs.raw_) * Decimal::scale / rhs.raw_;
    return Decimal::from_raw(checked_from_wide(quotient));
}

} // namespace abex
