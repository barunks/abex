#pragma once

#include "abex/application/order_gateway.hpp"
#include "abex/infrastructure/market_data_feed.hpp"
#include "abex/infrastructure/simulated_exchange_adapter.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <unordered_map>

#include <nlohmann/json_fwd.hpp>

namespace abex {

enum class RuntimeMode { Live, Simulation };

[[nodiscard]] RuntimeMode runtime_mode_from_string(std::string_view value);
[[nodiscard]] std::string_view to_string(RuntimeMode value) noexcept;

class GatewayRuntime final {
public:
    GatewayRuntime(const nlohmann::json& config,
                   std::optional<std::filesystem::path> state_path = std::nullopt,
                   RuntimeMode mode = RuntimeMode::Live,
                   std::filesystem::path environment_path = ".env");
    ~GatewayRuntime();

    GatewayRuntime(const GatewayRuntime&) = delete;
    GatewayRuntime& operator=(const GatewayRuntime&) = delete;

    [[nodiscard]] OrderGateway& gateway() noexcept { return *gateway_; }
    [[nodiscard]] const OrderGateway& gateway() const noexcept { return *gateway_; }
    [[nodiscard]] RuntimeMode mode() const noexcept { return mode_; }
    [[nodiscard]] bool live() const noexcept { return mode_ == RuntimeMode::Live; }
    [[nodiscard]] MarketDataBook& market_data() noexcept { return *market_data_; }
    [[nodiscard]] const MarketDataBook& market_data() const noexcept { return *market_data_; }
    [[nodiscard]] const auto& simulated_adapters() const noexcept { return simulated_; }

private:
    RuntimeMode mode_{RuntimeMode::Live};
    std::shared_ptr<MarketDataBook> market_data_;
    std::unique_ptr<MarketDataRingFeed> market_data_feed_;
    std::shared_ptr<IOrderStore> store_;
    std::vector<std::shared_ptr<IExchangeAdapter>> adapters_;
    std::unordered_map<Venue, std::shared_ptr<SimulatedExchangeAdapter>> simulated_;
    std::unique_ptr<OrderGateway> gateway_;
};

} // namespace abex
