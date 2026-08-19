#include "abex/application/risk_manager.hpp"

#include <sstream>

#include <nlohmann/json.hpp>

namespace abex {
namespace {

[[nodiscard]] Decimal signed_quantity(Side side, Decimal quantity) {
    return side == Side::Buy ? quantity : -quantity;
}

[[nodiscard]] Decimal conservative_exposure(const Order& order) {
    return is_terminal(order.status) ? order.filled_quantity : order.quantity;
}

} // namespace

RiskManager::RiskManager(StringMap<InstrumentRiskLimits> limits)
    : limits_(std::move(limits)) {}

RiskDecision RiskManager::check_new(const OrderRequest& request,
                                    const std::vector<Order>& current_orders,
                                    std::optional<Decimal> market_price) const {
    Decimal position;
    for (const auto& order : current_orders) {
        if (order.symbol == request.symbol) {
            position += signed_quantity(order.side, conservative_exposure(order));
        }
    }
    return check_new_with_position(request, position, market_price);
}

RiskDecision RiskManager::check_new_with_position(
    const OrderRequest& request,
    Decimal conservative_position,
    std::optional<Decimal> market_price) const {
    if (request.client_order_id.empty()) {
        return RiskDecision::reject("INVALID_ORDER", "clientOrderId is required");
    }
    if (request.symbol.empty()) {
        return RiskDecision::reject("INVALID_ORDER", "symbol is required");
    }
    if (request.type == OrderType::Limit && !request.price) {
        return RiskDecision::reject("INVALID_ORDER", "LIMIT orders require price");
    }
    if (request.type == OrderType::Market && request.price) {
        return RiskDecision::reject("INVALID_ORDER", "MARKET orders must not specify price");
    }
    if (request.price && !request.price->is_positive()) {
        return RiskDecision::reject("INVALID_ORDER", "price must be positive");
    }
    return validate(request.symbol, request.side, request.type, request.price, request.quantity,
                    conservative_position, market_price);
}

RiskDecision RiskManager::check_amend(const Order& order,
                                      std::optional<Decimal> new_price,
                                      std::optional<Decimal> new_quantity,
                                      const std::vector<Order>& current_orders) const {
    Decimal position;
    for (const auto& current : current_orders) {
        if (current.symbol == order.symbol && current.client_order_id != order.client_order_id) {
            position += signed_quantity(current.side, conservative_exposure(current));
        }
    }
    return check_amend_with_position(order, new_price, new_quantity, position);
}

RiskDecision RiskManager::check_amend_with_position(
    const Order& order,
    std::optional<Decimal> new_price,
    std::optional<Decimal> new_quantity,
    Decimal conservative_position_excluding_order) const {
    if (!new_price && !new_quantity) {
        return RiskDecision::reject("INVALID_AMEND", "newPrice or newQuantity is required");
    }
    if (is_terminal(order.status)) {
        return RiskDecision::reject("ORDER_TERMINAL", "terminal orders cannot be amended");
    }
    if (order.status == OrderStatus::Unknown) {
        return RiskDecision::reject("ORDER_STATE_UNKNOWN",
                                    "reconcile an UNKNOWN order before amending it");
    }
    if (order.type == OrderType::Market) {
        return RiskDecision::reject("INVALID_AMEND", "MARKET orders cannot be amended");
    }
    const auto price = new_price ? new_price : order.price;
    const auto quantity = new_quantity.value_or(order.quantity);
    if (quantity < order.filled_quantity) {
        return RiskDecision::reject("INVALID_AMEND",
                                    "new quantity cannot be below already filled quantity");
    }
    if (new_price && !new_price->is_positive()) {
        return RiskDecision::reject("INVALID_AMEND", "new price must be positive");
    }
    if (order.pending_action != PendingAction::None) {
        return RiskDecision::reject("OPERATION_PENDING",
                                    "another order operation is still pending");
    }
    return validate(order.symbol, order.side, order.type, price, quantity,
                    conservative_position_excluding_order, std::nullopt);
}

std::unordered_map<std::string, Decimal>
RiskManager::conservative_positions(const std::vector<Order>& orders) const {
    std::unordered_map<std::string, Decimal> positions;
    positions.reserve(limits_.size());
    for (const auto& order : orders) {
        positions[order.symbol] += signed_quantity(order.side, conservative_exposure(order));
    }
    return positions;
}

RiskDecision RiskManager::validate(std::string_view symbol,
                                   Side side,
                                   OrderType type,
                                   std::optional<Decimal> price,
                                   Decimal quantity,
                                   Decimal conservative_position,
                                   std::optional<Decimal> market_price) const {
    const auto limits_it = limits_.find(symbol);
    if (limits_it == limits_.end()) {
        return RiskDecision::reject("RISK_CONFIG_MISSING",
                                    "no risk limits configured for " + std::string(symbol));
    }
    const auto& limits = limits_it->second;
    if (!quantity.is_positive()) {
        return RiskDecision::reject("INVALID_ORDER", "quantity must be positive");
    }
    if (quantity > limits.max_order_size) {
        return RiskDecision::reject(
            "MAX_ORDER_SIZE",
            "quantity " + quantity.to_string() + " exceeds max order size " +
                limits.max_order_size.to_string());
    }

    const auto notional_price = type == OrderType::Market
                                    ? (market_price ? market_price : limits.market_reference_price)
                                    : price;
    if (!notional_price) {
        return RiskDecision::reject(
            "REFERENCE_PRICE_MISSING",
            "MARKET order requires a configured reference price for notional risk");
    }
    const auto notional = quantity * *notional_price;
    if (notional > limits.max_notional) {
        return RiskDecision::reject(
            "MAX_NOTIONAL",
            "notional " + notional.to_string() + " exceeds max notional " +
                limits.max_notional.to_string());
    }

    auto projected = conservative_position;
    projected += signed_quantity(side, quantity);
    if (projected.abs() > limits.position_limit) {
        return RiskDecision::reject(
            "POSITION_LIMIT",
            "worst-case position " + projected.to_string() + " exceeds absolute limit " +
                limits.position_limit.to_string());
    }
    return RiskDecision::accept();
}

RiskManager RiskManager::from_json(const nlohmann::json& json) {
    StringMap<InstrumentRiskLimits> limits;
    for (auto it = json.begin(); it != json.end(); ++it) {
        const auto& item = it.value();
        InstrumentRiskLimits parsed{
            .max_order_size = Decimal::parse(item.at("maxOrderSize").get<std::string>()),
            .max_notional = Decimal::parse(item.at("maxNotional").get<std::string>()),
            .position_limit = Decimal::parse(item.at("positionLimit").get<std::string>()),
        };
        if (item.contains("marketReferencePrice")) {
            parsed.market_reference_price =
                Decimal::parse(item.at("marketReferencePrice").get<std::string>());
        }
        limits.emplace(it.key(), parsed);
    }
    return RiskManager(std::move(limits));
}

} // namespace abex
