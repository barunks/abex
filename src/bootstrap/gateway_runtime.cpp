#include "abex/bootstrap/gateway_runtime.hpp"

#include "abex/application/risk_manager.hpp"
#include "abex/bootstrap/environment.hpp"
#include "abex/infrastructure/binance_adapter.hpp"
#include "abex/infrastructure/file_order_store.hpp"
#include "abex/infrastructure/okx_adapter.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace abex {

RuntimeMode runtime_mode_from_string(std::string_view value) {
    std::string normalized(value);
    std::ranges::transform(normalized, normalized.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (normalized == "live") return RuntimeMode::Live;
    if (normalized == "simulation" || normalized == "simulated") {
        return RuntimeMode::Simulation;
    }
    throw std::invalid_argument("mode must be live or simulation");
}

std::string_view to_string(RuntimeMode value) noexcept {
    return value == RuntimeMode::Live ? "live" : "simulation";
}

GatewayRuntime::GatewayRuntime(const nlohmann::json& config,
                               std::optional<std::filesystem::path> state_path,
                               RuntimeMode mode,
                               std::filesystem::path environment_path) {
    store_ = std::make_shared<FileOrderStore>(
        state_path.value_or(config.at("journal").at("path").get<std::string>()),
        config.at("journal").value("durableWrites", true));
    auto risk = RiskManager::from_json(config.at("risk"));
    const auto market_config = config.value("marketData", nlohmann::json::object());
    market_data_ = std::make_shared<MarketDataBook>(
        std::chrono::milliseconds{market_config.value("maximumAgeMs", 5000)});
    market_data_feed_ = std::make_unique<MarketDataRingFeed>(
        market_config.value("ringPath", std::filesystem::path{"state/market-data.ring"}),
        market_data_,
        std::chrono::milliseconds{market_config.value("ringPollIntervalMs", 50)});

    mode_ = mode;
    if (mode_ == RuntimeMode::Live) {
        (void)load_environment_file(environment_path);
        const auto& live_config = config.at("live");
        adapters_ = {
            std::make_shared<OkxAdapter>(OkxAdapter::Config::from_environment(
                live_config.at("okx"), live_config.value("demo", true))),
            std::make_shared<BinanceAdapter>(
                BinanceAdapter::Config::from_environment(live_config.at("binance"))),
        };
    } else {
        auto okx = std::make_shared<SimulatedExchangeAdapter>(
            Venue::Okx, SimulatedExchangeAdapter::Config{}, market_data_);
        auto binance = std::make_shared<SimulatedExchangeAdapter>(
            Venue::Binance, SimulatedExchangeAdapter::Config{}, market_data_);
        adapters_ = {okx, binance};
        simulated_ = {{Venue::Okx, std::move(okx)}, {Venue::Binance, std::move(binance)}};
    }

    gateway_ = std::make_unique<OrderGateway>(
        adapters_, std::move(risk), store_,
        OrderGateway::Options{
            .event_queue_capacity = config.value("eventQueueCapacity", std::size_t{4096}),
            .reconcile_on_start = true,
            .reconcile_on_reconnect = true,
            .reconciliation_interval = std::chrono::milliseconds(
                config.value("reconciliationIntervalMs", std::int64_t{30000})),
        },
        market_data_);
    market_data_feed_->start();
    gateway_->start();
}

GatewayRuntime::~GatewayRuntime() {
    if (market_data_feed_) market_data_feed_->stop();
    if (gateway_) gateway_->stop();
}

} // namespace abex
