#pragma once

#include "abex/application/order_gateway.hpp"
#include "abex/infrastructure/simulated_exchange_adapter.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace abex {

struct CommandResponse {
    std::string output;
    bool exit_requested{false};
};

class CommandProcessor final {
public:
    CommandProcessor(OrderGateway& gateway,
                     std::unordered_map<Venue, std::shared_ptr<SimulatedExchangeAdapter>> simulated);

    [[nodiscard]] CommandResponse execute(std::string_view line);
    [[nodiscard]] static std::string help_text();

private:
    OrderGateway& gateway_;
    std::unordered_map<Venue, std::shared_ptr<SimulatedExchangeAdapter>> simulated_;
};

} // namespace abex
