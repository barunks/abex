#pragma once

#include "abex/domain/order.hpp"

#include <cstdint>
#include <string>

namespace abex {

struct MarketQuote {
    Venue venue{Venue::Okx};
    std::string symbol;
    Decimal bid_price;
    Decimal ask_price;
    std::int64_t source_time_ms{0};
    std::int64_t published_at_ms{0};
    std::uint64_t sequence{0};
};

[[nodiscard]] inline bool valid_quote(const MarketQuote& quote) noexcept {
    return !quote.symbol.empty() && quote.bid_price.is_positive() &&
           quote.ask_price.is_positive() && quote.bid_price <= quote.ask_price;
}

[[nodiscard]] inline Decimal executable_price(const MarketQuote& quote, Side side) noexcept {
    return side == Side::Buy ? quote.ask_price : quote.bid_price;
}

} // namespace abex
