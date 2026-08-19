#pragma once

#include "abex/domain/order.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace abex {

struct ExecutionReport {
    std::string event_id;
    std::string client_order_id;
    std::string exchange_order_id;
    OrderStatus status{OrderStatus::Unknown};
    Decimal cumulative_filled;
    std::optional<Decimal> cumulative_quote;
    std::optional<Decimal> last_fill_price;
    // Authoritative venue terms. These are especially important for amend:
    // an API acknowledgement alone does not prove the new terms are live.
    std::optional<Decimal> order_price;
    std::optional<Decimal> order_quantity;
    std::optional<std::uint64_t> sequence;
    std::int64_t event_time_ms{0};
    std::string reason;
};

enum class ApplyDisposition { Applied, Duplicate, Stale, Invalid };

struct ApplyResult {
    ApplyDisposition disposition{ApplyDisposition::Invalid};
    bool state_changed{false};
    std::string_view reason;
};

} // namespace abex
