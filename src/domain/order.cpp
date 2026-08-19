#include "abex/domain/order.hpp"

#include <cctype>
#include <array>
#include <charconv>
#include <limits>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace abex {
namespace {

[[nodiscard]] bool ascii_iequals(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        const auto left = static_cast<unsigned char>(lhs[index]);
        const auto right = static_cast<unsigned char>(rhs[index]);
        if (std::toupper(left) != std::toupper(right)) return false;
    }
    return true;
}

template <typename T>
[[noreturn]] void invalid_enum(std::string_view name, std::string_view value) {
    throw std::invalid_argument(std::string("invalid ") + std::string(name) + ": " +
                                std::string(value));
}

} // namespace

std::string_view to_string(Venue value) {
    switch (value) {
    case Venue::Okx: return "OKX";
    case Venue::Binance: return "BINANCE";
    }
    throw std::logic_error("unknown venue");
}

std::string_view to_string(Side value) {
    switch (value) {
    case Side::Buy: return "BUY";
    case Side::Sell: return "SELL";
    }
    throw std::logic_error("unknown side");
}

std::string_view to_string(OrderType value) {
    switch (value) {
    case OrderType::Market: return "MARKET";
    case OrderType::Limit: return "LIMIT";
    }
    throw std::logic_error("unknown order type");
}

std::string_view to_string(TimeInForce value) {
    switch (value) {
    case TimeInForce::Gtc: return "GTC";
    case TimeInForce::Ioc: return "IOC";
    case TimeInForce::Fok: return "FOK";
    }
    throw std::logic_error("unknown time in force");
}

std::string_view to_string(OrderStatus value) {
    switch (value) {
    case OrderStatus::Live: return "LIVE";
    case OrderStatus::PartiallyFilled: return "PARTIALLY_FILLED";
    case OrderStatus::Filled: return "FILLED";
    case OrderStatus::Canceled: return "CANCELED";
    case OrderStatus::Rejected: return "REJECTED";
    case OrderStatus::Unknown: return "UNKNOWN";
    }
    throw std::logic_error("unknown order status");
}

std::string_view to_string(PendingAction value) {
    switch (value) {
    case PendingAction::None: return "NONE";
    case PendingAction::New: return "NEW";
    case PendingAction::Cancel: return "CANCEL";
    case PendingAction::Amend: return "AMEND";
    case PendingAction::Reconcile: return "RECONCILE";
    }
    throw std::logic_error("unknown pending action");
}

Venue venue_from_string(std::string_view value) {
    if (ascii_iequals(value, "OKX")) return Venue::Okx;
    if (ascii_iequals(value, "BINANCE")) return Venue::Binance;
    invalid_enum<Venue>("venue", value);
}

Side side_from_string(std::string_view value) {
    if (ascii_iequals(value, "BUY")) return Side::Buy;
    if (ascii_iequals(value, "SELL")) return Side::Sell;
    invalid_enum<Side>("side", value);
}

OrderType order_type_from_string(std::string_view value) {
    if (ascii_iequals(value, "MARKET")) return OrderType::Market;
    if (ascii_iequals(value, "LIMIT")) return OrderType::Limit;
    invalid_enum<OrderType>("order type", value);
}

TimeInForce time_in_force_from_string(std::string_view value) {
    if (ascii_iequals(value, "GTC")) return TimeInForce::Gtc;
    if (ascii_iequals(value, "IOC")) return TimeInForce::Ioc;
    if (ascii_iequals(value, "FOK")) return TimeInForce::Fok;
    invalid_enum<TimeInForce>("time in force", value);
}

OrderStatus order_status_from_string(std::string_view value) {
    if (ascii_iequals(value, "NEW") || ascii_iequals(value, "LIVE")) {
        return OrderStatus::Live;
    }
    if (ascii_iequals(value, "PARTIALLY_FILLED") ||
        ascii_iequals(value, "PARTIALLY FILLED")) {
        return OrderStatus::PartiallyFilled;
    }
    if (ascii_iequals(value, "FILLED")) return OrderStatus::Filled;
    if (ascii_iequals(value, "CANCELED") || ascii_iequals(value, "CANCELLED") ||
        ascii_iequals(value, "EXPIRED")) {
        return OrderStatus::Canceled;
    }
    if (ascii_iequals(value, "REJECTED")) return OrderStatus::Rejected;
    if (ascii_iequals(value, "UNKNOWN")) return OrderStatus::Unknown;
    invalid_enum<OrderStatus>("order status", value);
}

PendingAction pending_action_from_string(std::string_view value) {
    if (ascii_iequals(value, "NONE")) return PendingAction::None;
    if (ascii_iequals(value, "NEW")) return PendingAction::New;
    if (ascii_iequals(value, "CANCEL")) return PendingAction::Cancel;
    if (ascii_iequals(value, "AMEND")) return PendingAction::Amend;
    if (ascii_iequals(value, "RECONCILE")) return PendingAction::Reconcile;
    invalid_enum<PendingAction>("pending action", value);
}

bool is_terminal(OrderStatus status) noexcept {
    return status == OrderStatus::Filled || status == OrderStatus::Canceled ||
           status == OrderStatus::Rejected;
}

bool is_open(OrderStatus status) noexcept {
    return status == OrderStatus::Live || status == OrderStatus::PartiallyFilled ||
           status == OrderStatus::Unknown;
}

std::int64_t unix_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string fingerprint(const OrderRequest& request) {
    std::string result;
    result.reserve(request.client_order_id.size() + request.symbol.size() + 96);
    const auto append_text = [&result](std::string_view value) {
        result += std::to_string(value.size());
        result += ':';
        result += value;
        result += '|';
    };
    const auto append_number = [&result](auto value) {
        std::array<char, 32> buffer{};
        const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        if (error != std::errc{}) throw std::logic_error("fingerprint formatting failed");
        result.append(buffer.data(), end);
        result += '|';
    };
    result += "v2|CREATE|";
    append_text(request.client_order_id);
    append_number(static_cast<int>(request.venue));
    append_text(request.symbol);
    append_number(static_cast<int>(request.side));
    append_number(static_cast<int>(request.type));
    append_number(request.price ? request.price->raw()
                                : std::numeric_limits<std::int64_t>::min());
    append_number(request.quantity.raw());
    append_number(static_cast<int>(request.time_in_force));
    return result;
}

std::string fingerprint(const AmendRequest& request) {
    std::string result;
    result.reserve(request.client_order_id.size() + 80);
    result += "v2|AMEND|";
    result += std::to_string(request.client_order_id.size());
    result += ':';
    result += request.client_order_id;
    result += '|';
    result += std::to_string(request.new_price ? request.new_price->raw()
                                               : std::numeric_limits<std::int64_t>::min());
    result += '|';
    result += std::to_string(request.new_quantity ? request.new_quantity->raw()
                                                  : std::numeric_limits<std::int64_t>::min());
    return result;
}

std::string fingerprint(const CancelRequest& request) {
    std::string result;
    result.reserve(request.client_order_id.size() + 16);
    result += "v2|CANCEL|";
    result += request.client_order_id;
    return result;
}

namespace {

[[nodiscard]] std::string legacy_fingerprint(const OrderRequest& request) {
    return nlohmann::ordered_json{
        {"clientOrderId", request.client_order_id}, {"venue", to_string(request.venue)},
        {"symbol", request.symbol}, {"side", to_string(request.side)},
        {"type", to_string(request.type)},
        {"price", request.price ? request.price->to_string() : ""},
        {"quantity", request.quantity.to_string()},
        {"timeInForce", to_string(request.time_in_force)}}.dump();
}

[[nodiscard]] std::string legacy_fingerprint(const AmendRequest& request) {
    return nlohmann::ordered_json{
        {"clientOrderId", request.client_order_id},
        {"newPrice", request.new_price ? request.new_price->to_string() : ""},
        {"newQuantity", request.new_quantity ? request.new_quantity->to_string() : ""}}.dump();
}

} // namespace

bool fingerprint_matches(std::string_view stored, const OrderRequest& request) {
    return stored == fingerprint(request) ||
           (stored.starts_with('{') && stored == legacy_fingerprint(request));
}

bool fingerprint_matches(std::string_view stored, const AmendRequest& request) {
    return stored == fingerprint(request) ||
           (stored.starts_with('{') && stored == legacy_fingerprint(request));
}

bool fingerprint_matches(std::string_view stored, const CancelRequest& request) {
    if (stored == fingerprint(request)) return true;
    std::string legacy;
    legacy.reserve(request.client_order_id.size() + 7);
    legacy += "cancel:";
    legacy += request.client_order_id;
    return stored == legacy;
}

Order make_order(const OrderRequest& request, std::string create_fingerprint) {
    const auto now = unix_time_ms();
    if (create_fingerprint.empty()) create_fingerprint = fingerprint(request);
    return Order{
        .client_order_id = request.client_order_id,
        .venue = request.venue,
        .symbol = request.symbol,
        .side = request.side,
        .type = request.type,
        .price = request.price,
        .quantity = request.quantity,
        .time_in_force = request.time_in_force,
        .status = OrderStatus::Unknown,
        .pending_action = PendingAction::New,
        .created_at_ms = now,
        .updated_at_ms = now,
        .create_fingerprint = std::move(create_fingerprint),
    };
}

void to_json(nlohmann::json& json, const Decimal& value) { json = value.to_string(); }

void from_json(const nlohmann::json& json, Decimal& value) {
    value = Decimal::parse(json.get<std::string>());
}

void to_json(nlohmann::json& json, const OrderRequest& value) {
    json = nlohmann::json{
        {"clientOrderId", value.client_order_id},
        {"venue", to_string(value.venue)},
        {"symbol", value.symbol},
        {"side", to_string(value.side)},
        {"type", to_string(value.type)},
        {"quantity", value.quantity},
        {"timeInForce", to_string(value.time_in_force)},
    };
    if (value.price) json["price"] = *value.price;
}

void from_json(const nlohmann::json& json, OrderRequest& value) {
    value.client_order_id = json.at("clientOrderId").get<std::string>();
    value.venue = venue_from_string(json.at("venue").get<std::string>());
    value.symbol = json.at("symbol").get<std::string>();
    value.side = side_from_string(json.at("side").get<std::string>());
    value.type = order_type_from_string(json.at("type").get<std::string>());
    if (json.contains("price") && !json.at("price").is_null()) {
        value.price = json.at("price").get<Decimal>();
    } else {
        value.price.reset();
    }
    value.quantity = json.at("quantity").get<Decimal>();
    value.time_in_force = time_in_force_from_string(json.value("timeInForce", "GTC"));
}

void to_json(nlohmann::json& json, const Order& value) {
    json = nlohmann::json{
        {"clientOrderId", value.client_order_id},
        {"exchangeOrderId", value.exchange_order_id},
        {"exchangeClientIdAliases", value.exchange_client_id_aliases},
        {"exchangeOrderIdAliases", value.exchange_order_id_aliases},
        {"exchangeFillOffsets", value.exchange_fill_offsets},
        {"exchangeQuoteOffsets", value.exchange_quote_offsets},
        {"venue", to_string(value.venue)},
        {"symbol", value.symbol},
        {"side", to_string(value.side)},
        {"type", to_string(value.type)},
        {"quantity", value.quantity},
        {"timeInForce", to_string(value.time_in_force)},
        {"status", to_string(value.status)},
        {"pendingAction", to_string(value.pending_action)},
        {"filledQuantity", value.filled_quantity},
        {"cumulativeQuote", value.cumulative_quote},
        {"rejectionReason", value.rejection_reason},
        {"version", value.version},
        {"createdAt", value.created_at_ms},
        {"updatedAt", value.updated_at_ms},
        {"createFingerprint", value.create_fingerprint},
        {"processedRequests", value.processed_requests},
        {"processedRequestOutcomes", value.processed_request_outcomes},
        {"processedEventIds", value.processed_event_ids},
    };
    if (value.price) json["price"] = *value.price;
    if (value.pending_amend_price) json["pendingAmendPrice"] = *value.pending_amend_price;
    if (value.pending_amend_quantity) {
        json["pendingAmendQuantity"] = *value.pending_amend_quantity;
    }
    if (value.average_fill_price) json["averageFillPrice"] = *value.average_fill_price;
    if (value.last_sequence) json["lastSequence"] = *value.last_sequence;
}

void from_json(const nlohmann::json& json, Order& value) {
    value.client_order_id = json.at("clientOrderId").get<std::string>();
    value.exchange_order_id = json.value("exchangeOrderId", "");
    value.exchange_client_id_aliases = json.value(
        "exchangeClientIdAliases", StringSet{});
    value.exchange_order_id_aliases = json.value(
        "exchangeOrderIdAliases", StringSet{});
    value.exchange_fill_offsets = json.value(
        "exchangeFillOffsets", StringMap<Decimal>{});
    value.exchange_quote_offsets = json.value(
        "exchangeQuoteOffsets", StringMap<Decimal>{});
    value.venue = venue_from_string(json.at("venue").get<std::string>());
    value.symbol = json.at("symbol").get<std::string>();
    value.side = side_from_string(json.at("side").get<std::string>());
    value.type = order_type_from_string(json.at("type").get<std::string>());
    value.price = json.contains("price") ? std::optional(json.at("price").get<Decimal>())
                                          : std::nullopt;
    value.quantity = json.at("quantity").get<Decimal>();
    value.time_in_force = time_in_force_from_string(json.at("timeInForce").get<std::string>());
    value.status = order_status_from_string(json.at("status").get<std::string>());
    value.pending_action = pending_action_from_string(json.value("pendingAction", "NONE"));
    value.pending_amend_price = json.contains("pendingAmendPrice")
                                    ? std::optional(json.at("pendingAmendPrice").get<Decimal>())
                                    : std::nullopt;
    value.pending_amend_quantity =
        json.contains("pendingAmendQuantity")
            ? std::optional(json.at("pendingAmendQuantity").get<Decimal>())
            : std::nullopt;
    value.filled_quantity = json.value("filledQuantity", Decimal{});
    value.cumulative_quote = json.value("cumulativeQuote", Decimal{});
    value.average_fill_price = json.contains("averageFillPrice")
                                   ? std::optional(json.at("averageFillPrice").get<Decimal>())
                                   : std::nullopt;
    value.rejection_reason = json.value("rejectionReason", "");
    value.version = json.value("version", std::uint64_t{1});
    value.last_sequence = json.contains("lastSequence")
                              ? std::optional(json.at("lastSequence").get<std::uint64_t>())
                              : std::nullopt;
    value.created_at_ms = json.value("createdAt", std::int64_t{0});
    value.updated_at_ms = json.value("updatedAt", std::int64_t{0});
    value.create_fingerprint = json.value("createFingerprint", "");
    value.processed_requests = json.value(
        "processedRequests", StringMap<std::string>{});
    value.processed_request_outcomes = json.value(
        "processedRequestOutcomes", StringMap<std::string>{});
    value.processed_event_ids = json.value(
        "processedEventIds", StringSet{});
}

} // namespace abex
