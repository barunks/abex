#pragma once

#include "abex/application/market_data_book.hpp"
#include "abex/application/order_gateway.hpp"

#include <nlohmann/json.hpp>

namespace abex {

[[nodiscard]] nlohmann::json order_view(const Order& order);
[[nodiscard]] nlohmann::json order_view(const OrderSnapshot& order);
[[nodiscard]] nlohmann::json operation_view(const OperationResult& result);
[[nodiscard]] nlohmann::json positions_view(
    const OrderGateway::PositionSnapshot& positions);
[[nodiscard]] nlohmann::json health_view(
    const std::unordered_map<Venue, VenueHealth>& health);
[[nodiscard]] nlohmann::json balance_view(const BalanceQueryResult& result);
[[nodiscard]] nlohmann::json
instrument_rules_view(const InstrumentRulesQueryResult& result);
[[nodiscard]] nlohmann::json operational_event_view(const OperationalEvent& event);
[[nodiscard]] nlohmann::json system_view(const OrderGateway& gateway,
                                         std::size_t event_limit = 100);
[[nodiscard]] nlohmann::json market_quote_view(const MarketDataBook& book,
                                                const MarketQuote& quote);
[[nodiscard]] nlohmann::json market_data_view(const MarketDataBook& book);

} // namespace abex
