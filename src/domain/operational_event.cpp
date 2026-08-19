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
    std::string normalized(value);
    std::ranges::transform(normalized, normalized.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    if (normalized == "INFO") return OperationalSeverity::Info;
    if (normalized == "WARNING") return OperationalSeverity::Warning;
    if (normalized == "CRITICAL") return OperationalSeverity::Critical;
    throw std::invalid_argument("invalid operational severity: " + std::string(value));
}

void to_json(nlohmann::json& json, const OrderEventContext& context) {
    json = nlohmann::json{
        {"exchangeOrderId", context.exchange_order_id},
        {"symbol", context.symbol},
        {"side", context.side},
        {"type", context.type},
        {"price", context.price},
        {"quantity", context.quantity},
        {"filledQuantity", context.filled_quantity},
        {"status", context.status},
        {"pendingAction", context.pending_action},
        {"rejectionReason", context.rejection_reason},
        {"version", context.version},
        {"exchangeTime", context.exchange_time_ms},
    };
    if (context.venue_sequence) json["venueSequence"] = *context.venue_sequence;
}

void from_json(const nlohmann::json& json, OrderEventContext& context) {
    context.exchange_order_id = json.value("exchangeOrderId", std::string{});
    context.symbol = json.value("symbol", std::string{});
    context.side = json.value("side", std::string{});
    context.type = json.value("type", std::string{});
    context.price = json.value("price", std::string{});
    context.quantity = json.value("quantity", std::string{});
    context.filled_quantity = json.value("filledQuantity", std::string{});
    context.status = json.value("status", std::string{});
    context.pending_action = json.value("pendingAction", std::string{});
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
