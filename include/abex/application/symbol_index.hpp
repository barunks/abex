#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace abex {

// Compile-time symbol registry.
//
// Every supported trading symbol is listed here. Adding a symbol requires only
// adding an enumerator and a corresponding entry in kSymbolNames. All mapping
// is resolved at compile time — zero runtime cost, zero allocation, zero scan.
enum class SymbolId : std::uint8_t {
    BtcUsdt = 0,
    EthUsdt = 1,
    Unknown = 0xFF,
};

inline constexpr std::uint8_t kSymbolCount = 2;

inline constexpr std::array<std::string_view, kSymbolCount> kSymbolNames = {
    "BTC-USDT",  // BtcUsdt = 0
    "ETH-USDT",  // EthUsdt = 1
};

// Compile-time string → SymbolId. Returns SymbolId::Unknown for unrecognized symbols.
[[nodiscard]] consteval SymbolId symbol_id(std::string_view s) noexcept {
    for (std::uint8_t i = 0; i < kSymbolCount; ++i) {
        if (kSymbolNames[i] == s) return static_cast<SymbolId>(i);
    }
    return SymbolId::Unknown;
}

// Runtime fallback for symbols arriving from the network/journal at runtime.
// Returns SymbolId::Unknown for unrecognized symbols — caller must handle.
[[nodiscard]] constexpr SymbolId symbol_id_rt(std::string_view s) noexcept {
    for (std::uint8_t i = 0; i < kSymbolCount; ++i) {
        if (kSymbolNames[i] == s) return static_cast<SymbolId>(i);
    }
    return SymbolId::Unknown;
}

[[nodiscard]] constexpr std::uint8_t to_slot(SymbolId id) noexcept {
    return static_cast<std::uint8_t>(id);
}

// Verify compile-time mappings are correct.
static_assert(symbol_id("BTC-USDT") == SymbolId::BtcUsdt);
static_assert(symbol_id("ETH-USDT") == SymbolId::EthUsdt);
static_assert(symbol_id("UNKNOWN")  == SymbolId::Unknown);

} // namespace abex
