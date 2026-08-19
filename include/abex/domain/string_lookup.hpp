#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace abex {

// Lets owning string containers accept string_view lookups without constructing a
// temporary std::string. Values remain owned by the container; only lookup borrows.
struct TransparentStringHash final {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }
    [[nodiscard]] std::size_t operator()(const std::string& value) const noexcept {
        return (*this)(std::string_view{value});
    }
    [[nodiscard]] std::size_t operator()(const char* value) const noexcept {
        return (*this)(std::string_view{value});
    }
};

template <typename Value>
using StringMap =
    std::unordered_map<std::string, Value, TransparentStringHash, std::equal_to<>>;

using StringSet = std::unordered_set<std::string, TransparentStringHash, std::equal_to<>>;

} // namespace abex
