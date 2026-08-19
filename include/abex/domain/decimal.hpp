#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace abex {

// Fixed-point decimal used for every monetary value. It avoids binary floating-point
// drift and provides deterministic serialization across processes and restarts.
class Decimal final {
public:
    static constexpr std::int64_t scale = 100'000'000;
    static constexpr int precision = 8;

    constexpr Decimal() noexcept = default;

    [[nodiscard]] static Decimal parse(std::string_view text);
    [[nodiscard]] static constexpr Decimal from_raw(std::int64_t raw) noexcept {
        return Decimal(raw, RawTag{});
    }
    [[nodiscard]] static constexpr Decimal from_integer(std::int64_t value) noexcept {
        return Decimal(value * scale, RawTag{});
    }

    [[nodiscard]] constexpr std::int64_t raw() const noexcept { return raw_; }
    [[nodiscard]] std::string to_string() const;
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
