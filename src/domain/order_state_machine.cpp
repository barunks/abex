#include "abex/domain/order_state_machine.hpp"

#include <algorithm>

namespace abex {
namespace {

[[nodiscard]] bool stale_sequence(const Order& order, const ExecutionReport& report) {
    return order.last_sequence && report.sequence && *report.sequence <= *order.last_sequence;
}

[[nodiscard]] Decimal offset_for(const std::unordered_map<std::string, Decimal>& offsets,
                                 const std::string& exchange_order_id) {
    if (exchange_order_id.empty()) return {};
    const auto found = offsets.find(exchange_order_id);
    return found == offsets.end() ? Decimal{} : found->second;
}

[[nodiscard]] bool is_historical_generation(const Order& order,
                                            const ExecutionReport& report) {
    return !report.exchange_order_id.empty() && !order.exchange_order_id.empty() &&
           report.exchange_order_id != order.exchange_order_id &&
           order.exchange_order_id_aliases.contains(report.exchange_order_id);
}

[[nodiscard]] OrderStatus derive_status(const Order& order,
                                        const ExecutionReport& report,
                                        Decimal aggregate_filled,
                                        bool historical,
                                        bool stale) {
    if (aggregate_filled >= order.quantity) return OrderStatus::Filled;
    if (order.status == OrderStatus::Filled) return OrderStatus::Filled;

    // A superseded physical order can still deliver a late fill. Its terminal
    // lifecycle belongs to that generation, not to the active replacement.
    if (historical) {
        if (is_terminal(order.status)) return order.status;
        return aggregate_filled > Decimal{} ? OrderStatus::PartiallyFilled : order.status;
    }

    if (is_terminal(order.status)) return order.status;

    // An older report may contribute a late fill, but cannot overwrite a newer
    // lifecycle decision.
    if (stale) {
        return aggregate_filled > Decimal{} ? OrderStatus::PartiallyFilled : order.status;
    }

    if (report.status == OrderStatus::Canceled || report.status == OrderStatus::Rejected) {
        return report.status;
    }
    if (aggregate_filled > Decimal{} || report.status == OrderStatus::PartiallyFilled) {
        return OrderStatus::PartiallyFilled;
    }
    if (report.status == OrderStatus::Live) return OrderStatus::Live;
    return order.status;
}

[[nodiscard]] bool amendment_confirmed(const Order& order,
                                       const ExecutionReport& report) {
    const bool price_confirmed =
        !order.pending_amend_price ||
        (report.order_price && order.price && *order.price == *order.pending_amend_price);
    const bool quantity_confirmed =
        !order.pending_amend_quantity ||
        (report.order_quantity && order.quantity == *order.pending_amend_quantity);
    return price_confirmed && quantity_confirmed;
}

} // namespace

ApplyResult OrderStateMachine::apply(Order& order, const ExecutionReport& report) {
    if (!report.event_id.empty() && order.processed_event_ids.contains(report.event_id)) {
        return {.disposition = ApplyDisposition::Duplicate,
                .state_changed = false,
                .reason = "duplicate event id"};
    }
    if (!report.client_order_id.empty() && report.client_order_id != order.client_order_id &&
        !order.exchange_client_id_aliases.contains(report.client_order_id)) {
        return {.disposition = ApplyDisposition::Invalid,
                .state_changed = false,
                .reason = "execution report client order id does not match"};
    }

    const bool historical = is_historical_generation(order, report);
    if (!report.exchange_order_id.empty() && !order.exchange_order_id.empty() &&
        report.exchange_order_id != order.exchange_order_id && !historical) {
        return {.disposition = ApplyDisposition::Invalid,
                .state_changed = false,
                .reason = "execution report contains an unexpected exchange order id"};
    }

    const auto fill_offset = offset_for(order.exchange_fill_offsets, report.exchange_order_id);
    const auto quote_offset = offset_for(order.exchange_quote_offsets, report.exchange_order_id);
    const auto aggregate_filled = fill_offset + report.cumulative_filled;
    const auto observed_quantity = report.order_quantity
                                       ? std::optional(fill_offset + *report.order_quantity)
                                       : std::nullopt;
    const auto effective_quantity = observed_quantity && !historical
                                        ? *observed_quantity
                                        : order.quantity;
    if (report.cumulative_filled < Decimal{} || aggregate_filled > effective_quantity) {
        return {.disposition = ApplyDisposition::Invalid,
                .state_changed = false,
                .reason = "cumulative fill is outside the canonical order quantity"};
    }

    const bool stale = stale_sequence(order, report);
    const auto old_status = order.status;
    const auto old_filled = order.filled_quantity;
    const auto old_quote = order.cumulative_quote;
    const auto old_exchange_id = order.exchange_order_id;
    const auto old_price = order.price;
    const auto old_quantity = order.quantity;
    const auto old_pending = order.pending_action;

    if (!historical) {
        if (report.order_price) order.price = report.order_price;
        if (observed_quantity) order.quantity = *observed_quantity;
    }

    const auto merged_filled = std::max(order.filled_quantity, aggregate_filled);
    if (report.cumulative_quote) {
        const auto aggregate_quote = quote_offset + *report.cumulative_quote;
        if (aggregate_quote >= order.cumulative_quote) order.cumulative_quote = aggregate_quote;
    } else if (merged_filled > order.filled_quantity && report.last_fill_price) {
        order.cumulative_quote += (merged_filled - order.filled_quantity) * *report.last_fill_price;
    }
    order.filled_quantity = merged_filled;

    if (order.filled_quantity > Decimal{} && order.cumulative_quote > Decimal{}) {
        order.average_fill_price = order.cumulative_quote / order.filled_quantity;
    }

    order.status = derive_status(order, report, aggregate_filled, historical, stale);
    if (order.status == OrderStatus::Filled) order.filled_quantity = order.quantity;
    if (order.status == OrderStatus::Rejected && !report.reason.empty()) {
        order.rejection_reason = report.reason;
    }
    if (!historical && !report.exchange_order_id.empty()) {
        order.exchange_order_id = report.exchange_order_id;
    }
    if (report.sequence && (!order.last_sequence || *report.sequence > *order.last_sequence)) {
        order.last_sequence = report.sequence;
    }
    if (!report.event_id.empty()) order.processed_event_ids.insert(report.event_id);

    bool clear_pending = false;
    switch (order.pending_action) {
    case PendingAction::None: break;
    case PendingAction::New:
        clear_pending = !historical && report.status != OrderStatus::Unknown;
        break;
    case PendingAction::Cancel:
        clear_pending = !historical && is_terminal(order.status);
        break;
    case PendingAction::Amend:
        clear_pending = !historical &&
                        (is_terminal(order.status) || amendment_confirmed(order, report));
        break;
    case PendingAction::Reconcile:
        clear_pending = !historical && report.status != OrderStatus::Unknown;
        break;
    }
    if (clear_pending) {
        order.pending_action = PendingAction::None;
        order.pending_amend_price.reset();
        order.pending_amend_quantity.reset();
    }

    order.updated_at_ms = std::max(unix_time_ms(), report.event_time_ms);

    const bool changed = old_status != order.status || old_filled != order.filled_quantity ||
                         old_quote != order.cumulative_quote ||
                         old_exchange_id != order.exchange_order_id || old_price != order.price ||
                         old_quantity != order.quantity || old_pending != order.pending_action;
    if (changed) ++order.version;

    return {.disposition = stale ? ApplyDisposition::Stale : ApplyDisposition::Applied,
            .state_changed = changed,
            .reason = stale ? "stale sequence merged without status regression" : ""};
}

} // namespace abex
