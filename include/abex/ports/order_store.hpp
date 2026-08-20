#pragma once

#include "abex/domain/operational_event.hpp"
#include "abex/domain/order.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace abex {

struct OrderJournalStatus {
    std::string location;
    bool durable_writes{false};
    std::uint64_t record_sequence{0};
};

class IOrderStore {
public:
    virtual ~IOrderStore() = default;
    virtual void append(const Order& order) = 0;
    // Direct-serialize path: writes the order to the journal without constructing
    // an intermediate nlohmann::json object. When intent_only=true only the
    // immutable fields present at placement time are written; the large idempotency
    // maps (processed_requests, processed_event_ids, aliases) are omitted because
    // they are empty at intent time and the ACK record that follows is the
    // authoritative snapshot. Recovery uses the latest record per clientOrderId.
    virtual void append_order(const Order& order, bool intent_only) = 0;
    // Two-phase write: caller serializes inside its own mutex (so sequence order
    // matches mutation order), then commits the pre-built buffer outside the mutex.
    // sequence must have been obtained from reserve_sequence() under the same lock.
    [[nodiscard]] virtual std::uint64_t reserve_sequence() = 0;
    virtual void commit_order(const Order& order,
                              std::string payload,
                              std::uint64_t sequence) = 0;
    [[nodiscard]] virtual OperationalEvent append_event(OperationalEvent event) = 0;
    [[nodiscard]] virtual std::vector<Order> load_latest() const = 0;
    [[nodiscard]] virtual std::vector<OperationalEvent>
    load_events(std::size_t limit = 200) const = 0;
    [[nodiscard]] virtual std::vector<OperationalEvent>
    load_order_events(std::string_view client_order_id, std::size_t limit = 500) const = 0;
    [[nodiscard]] virtual OrderJournalStatus status() const = 0;
};

} // namespace abex
