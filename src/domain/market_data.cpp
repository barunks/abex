#include "abex/domain/market_data.hpp"

namespace abex {

bool valid_quote(const MarketQuote& quote) noexcept {
    return !quote.symbol.empty() && quote.bid_price.is_positive() &&
           quote.ask_price.is_positive() && quote.bid_price <= quote.ask_price;
}

Decimal executable_price(const MarketQuote& quote, Side side) noexcept {
    return side == Side::Buy ? quote.ask_price : quote.bid_price;
}

} // namespace abex
