#pragma once

#include "abex/domain/order.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace abex {

enum class OperationalSeverity { Info, Warning, Critical };

struct OrderEventContext {
    std::string exchange_order_id;
    std::string symbol;
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    std::optional<Decimal> price;
    std::optional<Decimal> average_fill_price;
    Decimal quantity;
    Decimal filled_quantity;
    OrderStatus status{OrderStatus::Unknown};
    PendingAction pending_action{PendingAction::None};
    std::string rejection_reason;
    std::uint64_t version{0};
    std::optional<std::uint64_t> venue_sequence;
    std::int64_t exchange_time_ms{0};
};

struct OperationalEvent {
    std::uint64_t sequence{0};
    std::int64_t occurred_at_ms{0};
    OperationalSeverity severity{OperationalSeverity::Info};
    std::string category;
    std::string code;
    std::string message;
    std::string instance_id;
    std::optional<Venue> venue;
    std::string client_order_id;
    std::string request_id;
    std::optional<OrderEventContext> order;
};

[[nodiscard]] std::string_view to_string(OperationalSeverity severity) noexcept;
[[nodiscard]] OperationalSeverity operational_severity_from_string(std::string_view value);

void to_json(nlohmann::json& json, const OperationalEvent& event);
void from_json(const nlohmann::json& json, OperationalEvent& event);
void to_json(nlohmann::json& json, const OrderEventContext& context);
void from_json(const nlohmann::json& json, OrderEventContext& context);

} // namespace abex
