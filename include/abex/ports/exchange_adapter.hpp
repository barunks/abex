#pragma once

#include "abex/domain/execution_report.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace abex {

struct AdapterResult {
    bool accepted{false};
    bool outcome_uncertain{false};
    bool replacement{false};
    bool original_order_canceled{false};
    std::string exchange_order_id;
    std::string exchange_client_order_id;
    std::string code;
    std::string message;
    std::vector<ExecutionReport> authoritative_reports;
};

struct AccountBalance {
    std::string currency;
    std::string total;
    std::string available;
    std::string frozen;
    std::string order_frozen;
};

struct AccountBalanceSnapshot {
    Venue venue{Venue::Okx};
    std::string account_id;
    std::string main_account_id;
    std::int64_t observed_at_ms{0};
    std::vector<AccountBalance> balances;
};

struct BalanceQueryResult {
    bool ok{false};
    std::string code;
    std::string message;
    AccountBalanceSnapshot snapshot;
};

struct InstrumentRules {
    Venue venue{Venue::Okx};
    std::string symbol;
    std::string status;
    bool trading{false};
    std::optional<Decimal> minimum_price;
    std::optional<Decimal> maximum_price;
    std::optional<Decimal> price_tick;
    std::optional<Decimal> minimum_quantity;
    std::optional<Decimal> maximum_quantity;
    std::optional<Decimal> quantity_step;
    std::optional<Decimal> market_minimum_quantity;
    std::optional<Decimal> market_maximum_quantity;
    std::optional<Decimal> market_quantity_step;
    std::optional<Decimal> minimum_notional;
    std::optional<Decimal> maximum_notional;
    std::optional<Decimal> market_minimum_notional;
    std::optional<Decimal> market_maximum_notional;
    std::int64_t observed_at_ms{0};
};

struct InstrumentRulesQueryResult {
    bool ok{false};
    std::string code;
    std::string message;
    InstrumentRules rules;
};

using ExecutionCallback = std::function<void(Venue, ExecutionReport)>;
using ConnectionCallback = std::function<void(Venue, bool, std::string)>;

class IExchangeAdapter {
public:
    virtual ~IExchangeAdapter() = default;

    [[nodiscard]] virtual Venue venue() const noexcept = 0;
    virtual void start(ExecutionCallback execution_callback,
                       ConnectionCallback connection_callback) = 0;
    virtual void stop() noexcept = 0;
    virtual void restore(std::span<const Order> recovered_orders) = 0;

    [[nodiscard]] virtual AdapterResult place(const Order& order) = 0;
    [[nodiscard]] virtual AdapterResult cancel(const Order& order) = 0;
    [[nodiscard]] virtual AdapterResult amend(const Order& order,
                                              std::optional<Decimal> new_price,
                                              std::optional<Decimal> new_quantity) = 0;
    [[nodiscard]] virtual std::optional<ExecutionReport> query(const Order& order) = 0;
    [[nodiscard]] virtual BalanceQueryResult
    query_balances(std::optional<std::string> currency = std::nullopt) = 0;
    [[nodiscard]] virtual InstrumentRulesQueryResult
    query_instrument_rules(std::string symbol) = 0;
    // nullopt means the venue could not provide an authoritative snapshot;
    // an empty vector means it successfully reported no open orders.
    [[nodiscard]] virtual std::optional<std::vector<ExecutionReport>> query_open_orders() = 0;
};

} // namespace abex
