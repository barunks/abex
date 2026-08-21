#pragma once

#include "abex/domain/decimal.hpp"
#include "abex/domain/string_lookup.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace abex {

enum class Venue { Okx, Binance };
enum class Side { Buy, Sell };
enum class OrderType { Market, Limit };
enum class TimeInForce { Gtc, Ioc, Fok };
enum class OrderStatus { Live, PartiallyFilled, Filled, Canceled, Rejected, Unknown };
enum class PendingAction { None, New, Cancel, Amend, Reconcile };

[[nodiscard]] std::string_view to_string(Venue value);
[[nodiscard]] std::string_view to_string(Side value);
[[nodiscard]] std::string_view to_string(OrderType value);
[[nodiscard]] std::string_view to_string(TimeInForce value);
[[nodiscard]] std::string_view to_string(OrderStatus value);
[[nodiscard]] std::string_view to_string(PendingAction value);

[[nodiscard]] Venue venue_from_string(std::string_view value);
[[nodiscard]] Side side_from_string(std::string_view value);
[[nodiscard]] OrderType order_type_from_string(std::string_view value);
[[nodiscard]] TimeInForce time_in_force_from_string(std::string_view value);
[[nodiscard]] OrderStatus order_status_from_string(std::string_view value);
[[nodiscard]] PendingAction pending_action_from_string(std::string_view value);

[[nodiscard]] bool is_terminal(OrderStatus status) noexcept;
[[nodiscard]] bool is_open(OrderStatus status) noexcept;

struct OrderRequest {
    std::string client_order_id;
    Venue venue{Venue::Okx};
    std::string symbol;
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    std::optional<Decimal> price;
    Decimal quantity;
    TimeInForce time_in_force{TimeInForce::Gtc};
};

struct AmendRequest {
    std::string client_order_id;
    std::string request_id;
    std::optional<Decimal> new_price;
    std::optional<Decimal> new_quantity;
};

struct CancelRequest {
    std::string client_order_id;
    std::string request_id;
};

struct Order {
    std::string client_order_id;
    std::string exchange_order_id;
    StringSet exchange_client_id_aliases;
    // A canonical order may span multiple physical venue orders (Binance
    // cancel-replace). Historical ids remain addressable so late fills can be
    // merged without allowing an old CANCELED event to cancel the replacement.
    StringSet exchange_order_id_aliases;
    StringMap<Decimal> exchange_fill_offsets;
    StringMap<Decimal> exchange_quote_offsets;
    Venue venue{Venue::Okx};
    std::string symbol;
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    std::optional<Decimal> price;
    Decimal quantity;
    TimeInForce time_in_force{TimeInForce::Gtc};
    OrderStatus status{OrderStatus::Unknown};
    PendingAction pending_action{PendingAction::None};
    std::optional<Decimal> pending_amend_price;
    std::optional<Decimal> pending_amend_quantity;
    Decimal filled_quantity;
    Decimal cumulative_quote;
    std::optional<Decimal> average_fill_price;
    std::string rejection_reason;
    std::uint64_t version{1};
    std::optional<std::uint64_t> last_sequence;
    std::int64_t created_at_ms{0};
    std::int64_t updated_at_ms{0};
    std::string create_fingerprint;
    StringMap<std::string> processed_requests;
    StringMap<std::string> processed_request_outcomes;
    StringSet processed_event_ids;
};

[[nodiscard]] std::int64_t unix_time_ms();
[[nodiscard]] std::string fingerprint(const OrderRequest& request);
[[nodiscard]] std::string fingerprint(const AmendRequest& request);
[[nodiscard]] std::string fingerprint(const CancelRequest& request);
[[nodiscard]] bool fingerprint_matches(std::string_view stored, const OrderRequest& request);
[[nodiscard]] bool fingerprint_matches(std::string_view stored, const AmendRequest& request);
[[nodiscard]] bool fingerprint_matches(std::string_view stored, const CancelRequest& request);
[[nodiscard]] Order make_order(const OrderRequest& request,
                               std::string create_fingerprint = {},
                               std::int64_t now_ms = 0);

void to_json(nlohmann::json& json, const Decimal& value);
void from_json(const nlohmann::json& json, Decimal& value);
void to_json(nlohmann::json& json, const OrderRequest& value);
void from_json(const nlohmann::json& json, OrderRequest& value);
void to_json(nlohmann::json& json, const Order& value);
void from_json(const nlohmann::json& json, Order& value);

} // namespace abex
