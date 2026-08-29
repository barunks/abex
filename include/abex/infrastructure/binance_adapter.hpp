#pragma once

#include "abex/infrastructure/rate_limiter.hpp"
#include "abex/domain/string_lookup.hpp"
#include "abex/infrastructure/reconnecting_websocket.hpp"
#include "abex/ports/exchange_adapter.hpp"

#include <atomic>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <nlohmann/json_fwd.hpp>

namespace abex {

class BinanceAdapter final : public IExchangeAdapter {
public:
    struct Config {
        std::string websocket_url;
        std::string api_key;
        std::string secret_key;
        std::chrono::milliseconds request_timeout{5000};
        std::chrono::milliseconds server_time_resync_interval{5000};
        std::chrono::milliseconds timestamp_safety_margin{100};
        std::chrono::milliseconds recv_window{5000};
        std::chrono::seconds instrument_cache_ttl{30};
        double rate_limit_capacity{100.0};
        double rate_limit_rate{20.0};

        [[nodiscard]] static Config from_environment(const nlohmann::json& json);
    };

    explicit BinanceAdapter(Config config);
    ~BinanceAdapter() override;

    [[nodiscard]] Venue venue() const noexcept override { return Venue::Binance; }
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
    [[nodiscard]] nlohmann::json request(std::string method,
                                         nlohmann::json parameters);
    [[nodiscard]] nlohmann::json signed_request(std::string method,
                                                nlohmann::json parameters);
    void wait_until_ready();
    [[nodiscard]] nlohmann::json sign_parameters(nlohmann::json parameters) const;
    [[nodiscard]] std::int64_t signed_timestamp() const;
    [[nodiscard]] std::string next_request_id(std::string_view prefix);
    void observe_rate_limits(const nlohmann::json& response);
    void request_server_time();
    void run_clock_sync(std::stop_token stop_token);
    void websocket_opened();
    void subscribe_user_data();
    void websocket_message(std::string_view message);
    void websocket_connection(bool connected, std::string_view reason);
    void fail_pending(std::string_view reason);

    Config config_;
    TokenBucket request_rate_limiter_;
    ReconnectingWebSocket websocket_;
    std::mutex instrument_cache_mutex_;
    std::unordered_map<std::string,
        std::pair<std::chrono::steady_clock::time_point, InstrumentRulesQueryResult>>
        instrument_cache_;
    std::atomic<std::uint64_t> next_id_{1};
    std::atomic<bool> subscribed_{false};
    mutable std::mutex pending_mutex_;
    mutable std::mutex alias_mutex_;
    std::condition_variable subscription_condition_;
    StringMap<std::shared_ptr<std::promise<nlohmann::json>>> pending_;
    StringMap<std::string> alias_to_client_;
    std::string readiness_error_;
    std::string time_request_id_;
    std::chrono::steady_clock::time_point time_request_sent_at_{};
    std::string subscription_request_id_;
    mutable std::mutex clock_mutex_;
    std::chrono::steady_clock::time_point clock_synchronized_at_{};
    std::int64_t server_time_at_sync_ms_{0};
    std::int64_t clock_uncertainty_ms_{0};
    bool clock_synchronized_{false};
    std::mutex clock_sync_wait_mutex_;
    std::condition_variable clock_sync_condition_;
    std::jthread clock_sync_thread_;
    ExecutionCallback execution_callback_;
    ConnectionCallback connection_callback_;
};

} // namespace abex
