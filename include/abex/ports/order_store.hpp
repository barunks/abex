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
    [[nodiscard]] virtual OperationalEvent append_event(OperationalEvent event) = 0;
    [[nodiscard]] virtual std::vector<Order> load_latest() const = 0;
    [[nodiscard]] virtual std::vector<OperationalEvent>
    load_events(std::size_t limit = 200) const = 0;
    [[nodiscard]] virtual std::vector<OperationalEvent>
    load_order_events(std::string_view client_order_id, std::size_t limit = 500) const = 0;
    [[nodiscard]] virtual OrderJournalStatus status() const = 0;
};

} // namespace abex
