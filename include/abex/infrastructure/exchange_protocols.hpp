#pragma once

#include "abex/domain/execution_report.hpp"
#include "abex/ports/exchange_adapter.hpp"

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace abex {

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
