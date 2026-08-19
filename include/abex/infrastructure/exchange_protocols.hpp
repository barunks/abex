#pragma once

#include "abex/domain/execution_report.hpp"
#include "abex/ports/exchange_adapter.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace abex {

// Single user-agent string used by both HTTP and WebSocket transports.
constexpr std::string_view k_user_agent = "abex-gateway/0.1";

// Shared adapter utilities used by both OKX and Binance adapters.
namespace adapter_util {

[[nodiscard]] inline std::string environment_required(const char* name) {
    const auto* value = std::getenv(name);
    if (!value || std::string_view(value).empty())
        throw std::runtime_error(std::string("missing required environment variable ") + name);
    return value;
}

[[nodiscard]] inline std::optional<std::string>
normalized_currency(std::optional<std::string> currency) {
    if (!currency || currency->empty()) return std::nullopt;
    std::ranges::transform(*currency, currency->begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    if (!std::ranges::all_of(*currency, [](unsigned char c) {
            return std::isalnum(c) != 0;
        }))
        throw std::invalid_argument("balance currency must be alphanumeric");
    return currency;
}

[[nodiscard]] inline std::string normalized_symbol(std::string symbol) {
    std::ranges::transform(symbol, symbol.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    if (symbol.empty() || !std::ranges::all_of(symbol, [](unsigned char c) {
            return std::isalnum(c) != 0 || c == '-';
        }))
        throw std::invalid_argument(
            "instrument symbol must be canonical alphanumeric ASSET-ASSET");
    return symbol;
}

} // namespace adapter_util
class OkxProtocol final {
public:
    [[nodiscard]] static std::string client_id_to_exchange(std::string_view client_order_id);
    [[nodiscard]] static nlohmann::json place_request(const Order& order);
    [[nodiscard]] static nlohmann::json cancel_request(const Order& order);
    [[nodiscard]] static nlohmann::json amend_request(const Order& order,
                                                      std::optional<Decimal> new_price,
                                                      std::optional<Decimal> new_quantity);
    [[nodiscard]] static ExecutionReport parse_order_update(const nlohmann::json& data);
    [[nodiscard]] static AdapterResult parse_ack(const nlohmann::json& response);
    [[nodiscard]] static BalanceQueryResult
    parse_balances(const nlohmann::json& account_config,
                   const nlohmann::json& balance_response);
    [[nodiscard]] static InstrumentRulesQueryResult
    parse_instrument_rules(const nlohmann::json& response);
};

class BinanceProtocol final {
public:
    [[nodiscard]] static std::string symbol_to_exchange(std::string_view symbol);
    [[nodiscard]] static nlohmann::json place_params(const Order& order);
    [[nodiscard]] static nlohmann::json cancel_params(const Order& order);
    [[nodiscard]] static nlohmann::json amend_params(const Order& order,
                                                     std::optional<Decimal> new_price,
                                                     std::optional<Decimal> new_quantity,
                                                     std::string new_exchange_client_id);
    [[nodiscard]] static ExecutionReport parse_execution_report(const nlohmann::json& event);
    [[nodiscard]] static ExecutionReport parse_order_status(const nlohmann::json& result);
    [[nodiscard]] static AdapterResult parse_ack(const nlohmann::json& response);
    [[nodiscard]] static BalanceQueryResult parse_balances(const nlohmann::json& response);
    [[nodiscard]] static InstrumentRulesQueryResult
    parse_instrument_rules(const nlohmann::json& response);
};

} // namespace abex
