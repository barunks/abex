#pragma once

#include "abex/domain/order.hpp"
#include "abex/domain/string_lookup.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace abex {

struct InstrumentRiskLimits {
    Decimal max_order_size;
    Decimal max_notional;
    Decimal position_limit;
    std::optional<Decimal> market_reference_price;
};

struct RiskDecision {
    bool accepted{false};
    std::string code;
    std::string reason;

    [[nodiscard]] static RiskDecision accept() { return {.accepted = true}; }
    [[nodiscard]] static RiskDecision reject(std::string code, std::string reason) {
        return {.accepted = false, .code = std::move(code), .reason = std::move(reason)};
    }
};

class RiskManager final {
public:
    explicit RiskManager(StringMap<InstrumentRiskLimits> limits);

    [[nodiscard]] RiskDecision check_new(const OrderRequest& request,
                                         const std::vector<Order>& current_orders,
                                         std::optional<Decimal> market_price = std::nullopt) const;
    [[nodiscard]] RiskDecision check_amend(const Order& order,
                                           std::optional<Decimal> new_price,
                                           std::optional<Decimal> new_quantity,
                                           const std::vector<Order>& current_orders) const;
    [[nodiscard]] RiskDecision
    check_new_with_position(const OrderRequest& request,
                            Decimal conservative_position,
                            std::optional<Decimal> market_price = std::nullopt) const;
    [[nodiscard]] RiskDecision
    check_amend_with_position(const Order& order,
                              std::optional<Decimal> new_price,
                              std::optional<Decimal> new_quantity,
                              Decimal conservative_position_excluding_order) const;
    [[nodiscard]] std::unordered_map<std::string, Decimal>
    conservative_positions(const std::vector<Order>& orders) const;
    [[nodiscard]] const auto& limits() const noexcept { return limits_; }

    [[nodiscard]] static RiskManager from_json(const nlohmann::json& json);

private:
    [[nodiscard]] RiskDecision validate(std::string_view symbol,
                                        Side side,
                                        OrderType type,
                                        std::optional<Decimal> price,
                                        Decimal quantity,
                                        Decimal conservative_position,
                                        std::optional<Decimal> market_price) const;

    StringMap<InstrumentRiskLimits> limits_;
};

} // namespace abex
