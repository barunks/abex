#pragma once

#include "abex/domain/market_data.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace abex {

struct MarketDataCursor {
    std::uint64_t generation{0};
    std::uint64_t sequence{0};
};

class MarketDataRingWriter final {
public:
    explicit MarketDataRingWriter(std::filesystem::path path,
                                  std::size_t capacity = 1024);
    ~MarketDataRingWriter();

    MarketDataRingWriter(const MarketDataRingWriter&) = delete;
    MarketDataRingWriter& operator=(const MarketDataRingWriter&) = delete;

    void publish(std::span<const MarketQuote> quotes);
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] std::uint64_t sequence() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class MarketDataRingReader final {
public:
    explicit MarketDataRingReader(std::filesystem::path path);
    ~MarketDataRingReader();

    MarketDataRingReader(const MarketDataRingReader&) = delete;
    MarketDataRingReader& operator=(const MarketDataRingReader&) = delete;

    [[nodiscard]] std::vector<MarketQuote> read_available(MarketDataCursor& cursor) const;
    [[nodiscard]] std::size_t read_available(MarketDataCursor& cursor,
                                             std::span<MarketQuote> output) const;
    [[nodiscard]] std::size_t capacity() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace abex
