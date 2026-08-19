#pragma once

#include "abex/infrastructure/http_client.hpp"
#include "abex/domain/string_lookup.hpp"
#include "abex/infrastructure/rate_limiter.hpp"
#include "abex/infrastructure/reconnecting_websocket.hpp"
#include "abex/ports/exchange_adapter.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

#include <nlohmann/json_fwd.hpp>

namespace abex {

class OkxAdapter final : public IExchangeAdapter {
public:
    struct Config {
        std::string rest_url;
        std::string private_websocket_url;
        std::string api_key;
        std::string secret_key;
        std::string passphrase;
        bool demo{true};
        std::chrono::seconds heartbeat_idle{20};
        std::chrono::seconds heartbeat_timeout{8};
        std::chrono::seconds instrument_cache_ttl{30};
        double rate_limit_capacity{60.0};
        double rate_limit_rate{30.0};

        [[nodiscard]] static Config from_environment(const nlohmann::json& json, bool demo);
    };

    explicit OkxAdapter(Config config);
    ~OkxAdapter() override;

    [[nodiscard]] Venue venue() const noexcept override { return Venue::Okx; }
    void start(ExecutionCallback execution_callback,
               ConnectionCallback connection_callback) override;
    void stop() noexcept override;
    void restore(std::span<const Order> recovered_orders) override;

    [[nodiscard]] AdapterResult place(const Order& order) override;
    [[nodiscard]] AdapterResult cancel(const Order& order) override;
    [[nodiscard]] AdapterResult amend(const Order& order,
                                      std::optional<Decimal> new_price,
                                      std::optional<Decimal> new_quantity) override;
    [[nodiscard]] std::optional<ExecutionReport> query(const Order& order) override;
    [[nodiscard]] BalanceQueryResult
    query_balances(std::optional<std::string> currency = std::nullopt) override;
    [[nodiscard]] InstrumentRulesQueryResult
    query_instrument_rules(std::string symbol) override;
    [[nodiscard]] std::optional<std::vector<ExecutionReport>> query_open_orders() override;

private:
    [[nodiscard]] nlohmann::json rest_request(std::string method,
                                              std::string request_path,
                                              std::string body = {}) const;
    [[nodiscard]] AdapterResult write_operation(std::string_view path,
                                                const nlohmann::json& body);
    void websocket_opened();
    void websocket_message(std::string_view message);
    void websocket_connection(bool connected, std::string_view reason);

    Config config_;
    HttpClient http_;
    TokenBucket order_rate_limiter_;
    ReconnectingWebSocket websocket_;
    std::mutex instrument_cache_mutex_;
    std::unordered_map<std::string,
        std::pair<std::chrono::steady_clock::time_point, InstrumentRulesQueryResult>>
        instrument_cache_;
    mutable std::mutex alias_mutex_;
    StringMap<std::string> alias_to_client_;
    ExecutionCallback execution_callback_;
    ConnectionCallback connection_callback_;
    std::atomic<bool> authenticated_{false};
};

} // namespace abex
