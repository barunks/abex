#include "abex/domain/operational_event.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace abex {

std::string_view to_string(OperationalSeverity severity) noexcept {
    switch (severity) {
    case OperationalSeverity::Info: return "INFO";
    case OperationalSeverity::Warning: return "WARNING";
    case OperationalSeverity::Critical: return "CRITICAL";
    }
    return "CRITICAL";
}

OperationalSeverity operational_severity_from_string(std::string_view value) {
    const auto equal = [value](std::string_view expected) {
        return value.size() == expected.size() &&
               std::ranges::equal(value, expected, [](unsigned char lhs, unsigned char rhs) {
                   return std::toupper(lhs) == std::toupper(rhs);
               });
    };
    if (equal("INFO")) return OperationalSeverity::Info;
    if (equal("WARNING")) return OperationalSeverity::Warning;
    if (equal("CRITICAL")) return OperationalSeverity::Critical;
    throw std::invalid_argument("invalid operational severity: " + std::string(value));
}

void to_json(nlohmann::json& json, const OrderEventContext& context) {
    json = nlohmann::json{
        {"exchangeOrderId", context.exchange_order_id},
        {"symbol", context.symbol},
        {"side", to_string(context.side)},
        {"type", to_string(context.type)},
        {"quantity", context.quantity},
        {"filledQuantity", context.filled_quantity},
        {"status", to_string(context.status)},
        {"pendingAction", to_string(context.pending_action)},
        {"rejectionReason", context.rejection_reason},
        {"version", context.version},
        {"exchangeTime", context.exchange_time_ms},
    };
    if (context.price) json["price"] = *context.price;
    if (context.average_fill_price) json["averageFillPrice"] = *context.average_fill_price;
    if (context.venue_sequence) json["venueSequence"] = *context.venue_sequence;
}

void from_json(const nlohmann::json& json, OrderEventContext& context) {
    context.exchange_order_id = json.value("exchangeOrderId", std::string{});
    context.symbol = json.value("symbol", std::string{});
    context.side = side_from_string(json.value("side", "BUY"));
    context.type = order_type_from_string(json.value("type", "LIMIT"));
    if (json.contains("price") && !json.at("price").is_null()) {
        const auto& pv = json.at("price");
        const auto ps = pv.is_string() ? pv.get<std::string>() : std::string{};
        context.price = (!pv.is_string() || !ps.empty()) ? std::optional(pv.get<Decimal>()) : std::nullopt;
    } else {
        context.price.reset();
    }
    if (json.contains("averageFillPrice") && !json.at("averageFillPrice").is_null()) {
        const auto& av = json.at("averageFillPrice");
        const auto as = av.is_string() ? av.get<std::string>() : std::string{};
        context.average_fill_price = (!av.is_string() || !as.empty()) ? std::optional(av.get<Decimal>()) : std::nullopt;
    } else {
        context.average_fill_price = std::nullopt;
    }
    context.quantity = json.contains("quantity") && !json.at("quantity").get<std::string>().empty()
                           ? json.at("quantity").get<Decimal>()
                           : Decimal{};
    context.filled_quantity =
        json.contains("filledQuantity") && !json.at("filledQuantity").get<std::string>().empty()
            ? json.at("filledQuantity").get<Decimal>()
            : Decimal{};
    context.status = order_status_from_string(json.value("status", "UNKNOWN"));
    context.pending_action = pending_action_from_string(json.value("pendingAction", "NONE"));
    context.rejection_reason = json.value("rejectionReason", std::string{});
    context.version = json.value("version", std::uint64_t{0});
    context.venue_sequence = json.contains("venueSequence")
                                 ? std::optional(json.at("venueSequence").get<std::uint64_t>())
                                 : std::nullopt;
    context.exchange_time_ms = json.value("exchangeTime", std::int64_t{0});
}

void to_json(nlohmann::json& json, const OperationalEvent& event) {
    json = nlohmann::json{
        {"occurredAt", event.occurred_at_ms},
        {"severity", to_string(event.severity)},
        {"category", event.category},
        {"code", event.code},
        {"message", event.message},
        {"instanceId", event.instance_id},
        {"clientOrderId", event.client_order_id},
        {"requestId", event.request_id},
    };
    if (event.venue) json["venue"] = to_string(*event.venue);
    if (event.order) json["order"] = *event.order;
}

void from_json(const nlohmann::json& json, OperationalEvent& event) {
    event.occurred_at_ms = json.at("occurredAt").get<std::int64_t>();
    event.severity = operational_severity_from_string(json.at("severity").get<std::string>());
    event.category = json.at("category").get<std::string>();
    event.code = json.at("code").get<std::string>();
    event.message = json.at("message").get<std::string>();
    event.instance_id = json.value("instanceId", std::string{});
    event.client_order_id = json.value("clientOrderId", std::string{});
    event.request_id = json.value("requestId", std::string{});
    event.venue = json.contains("venue")
                      ? std::optional(venue_from_string(json.at("venue").get<std::string>()))
                      : std::nullopt;
    event.order = json.contains("order")
                      ? std::optional(json.at("order").get<OrderEventContext>())
                      : std::nullopt;
}

} // namespace abex
