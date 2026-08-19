#include "abex/infrastructure/binance_adapter.hpp"

#include "abex/infrastructure/crypto.hpp"
#include "abex/infrastructure/exchange_protocols.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace abex {
namespace {

[[nodiscard]] std::string environment_required(const char* name) {
    const auto* value = std::getenv(name);
    if (!value || std::string_view(value).empty()) {
        throw std::runtime_error(std::string("missing required environment variable ") + name);
    }
    return value;
}

[[nodiscard]] std::string json_id(const nlohmann::json& json) {
    if (!json.contains("id")) return {};
    if (json.at("id").is_string()) return json.at("id").get<std::string>();
    return json.at("id").dump();
}

[[nodiscard]] std::optional<std::string>
normalized_currency(std::optional<std::string> currency) {
    if (!currency || currency->empty()) return std::nullopt;
    std::ranges::transform(*currency, currency->begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    if (!std::ranges::all_of(*currency, [](unsigned char character) {
            return std::isalnum(character) != 0;
        })) {
        throw std::invalid_argument("balance currency must be alphanumeric");
    }
    return currency;
}

} // namespace

BinanceAdapter::Config BinanceAdapter::Config::from_environment(const nlohmann::json& json) {
    return {
        .websocket_url =
            json.value("webSocketUrl", "wss://ws-api.testnet.binance.vision/ws-api/v3"),
        .api_key = environment_required("ABEX_BINANCE_API_KEY"),
        .secret_key = environment_required("ABEX_BINANCE_SECRET_KEY"),
    };
}

BinanceAdapter::BinanceAdapter(Config config)
    : config_(std::move(config)), websocket_({.url = config_.websocket_url}) {
    if (config_.server_time_resync_interval <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Binance server-time resync interval must be positive");
    }
    if (config_.timestamp_safety_margin < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Binance timestamp safety margin cannot be negative");
    }
}

BinanceAdapter::~BinanceAdapter() { stop(); }

void BinanceAdapter::start(ExecutionCallback execution_callback,
                           ConnectionCallback connection_callback) {
    execution_callback_ = std::move(execution_callback);
    connection_callback_ = std::move(connection_callback);
    websocket_.start([this] { websocket_opened(); },
                     [this](std::string_view message) { websocket_message(message); },
                     [this](bool connected, std::string_view reason) {
                         websocket_connection(connected, reason);
                     });
    clock_sync_thread_ =
        std::jthread([this](std::stop_token token) { run_clock_sync(token); });
}

void BinanceAdapter::stop() noexcept {
    clock_sync_thread_.request_stop();
    clock_sync_condition_.notify_all();
    if (clock_sync_thread_.joinable()) clock_sync_thread_.join();
    websocket_.stop();
    subscribed_.store(false);
    subscription_condition_.notify_all();
    fail_pending("Binance adapter stopped");
}

void BinanceAdapter::restore(std::span<const Order> recovered_orders) {
    std::scoped_lock lock(alias_mutex_);
    for (const auto& order : recovered_orders) {
        if (order.venue != Venue::Binance) continue;
        alias_to_client_[order.client_order_id] = order.client_order_id;
        for (const auto& alias : order.exchange_client_id_aliases) {
            alias_to_client_[alias] = order.client_order_id;
        }
    }
}

AdapterResult BinanceAdapter::place(const Order& order) {
    if (!request_rate_limiter_.try_acquire()) {
        return {.accepted = false,
                .code = "LOCAL_RATE_LIMIT",
                .message = "Binance order request budget exhausted"};
    }
    {
        std::scoped_lock lock(alias_mutex_);
        alias_to_client_[order.client_order_id] = order.client_order_id;
    }
    try {
        return BinanceProtocol::parse_ack(
            signed_request("order.place", BinanceProtocol::place_params(order)));
    } catch (const std::exception& error) {
        return {.accepted = false,
                .outcome_uncertain = true,
                .code = "BINANCE_TRANSPORT_ERROR",
                .message = error.what()};
    }
}

AdapterResult BinanceAdapter::cancel(const Order& order) {
    if (!request_rate_limiter_.try_acquire()) {
        return {.accepted = false,
                .code = "LOCAL_RATE_LIMIT",
                .message = "Binance cancel request budget exhausted"};
    }
    try {
        return BinanceProtocol::parse_ack(
            signed_request("order.cancel", BinanceProtocol::cancel_params(order)));
    } catch (const std::exception& error) {
        return {.accepted = false,
                .outcome_uncertain = true,
                .code = "BINANCE_TRANSPORT_ERROR",
                .message = error.what()};
    }
}

AdapterResult BinanceAdapter::amend(const Order& order,
                                    std::optional<Decimal> new_price,
                                    std::optional<Decimal> new_quantity) {
    if (!request_rate_limiter_.try_acquire()) {
        return {.accepted = false,
                .code = "LOCAL_RATE_LIMIT",
                .message = "Binance cancel-replace request budget exhausted"};
    }
    const auto target_quantity = new_quantity.value_or(order.quantity);
    const auto replacement_quantity = target_quantity - order.filled_quantity;
    if (replacement_quantity <= Decimal{}) {
        return {.accepted = false,
                .code = "INVALID_REPLACEMENT_QUANTITY",
                .message = "replacement quantity must exceed the already-filled quantity"};
    }
    const auto alias = OkxProtocol::client_id_to_exchange(
        order.client_order_id + "v" + std::to_string(order.version + 1));
    {
        std::scoped_lock lock(alias_mutex_);
        alias_to_client_[alias] = order.client_order_id;
    }
    try {
        auto result = BinanceProtocol::parse_ack(signed_request(
            "order.cancelReplace",
            BinanceProtocol::amend_params(order, new_price, replacement_quantity, alias)));
        if (result.accepted && result.exchange_client_order_id.empty()) {
            result.exchange_client_order_id = alias;
        }
        return result;
    } catch (const std::exception& error) {
        return {.accepted = false,
                .outcome_uncertain = true,
                .code = "BINANCE_TRANSPORT_ERROR",
                .message = error.what()};
    }
}

std::optional<ExecutionReport> BinanceAdapter::query(const Order& order) {
    if (!request_rate_limiter_.try_acquire(4.0)) return std::nullopt;
    try {
        const auto response = signed_request("order.status", BinanceProtocol::cancel_params(order));
        if (response.value("status", 0) != 200 || !response.contains("result")) {
            return std::nullopt;
        }
        const auto& result = response.at("result");
        auto report = BinanceProtocol::parse_order_status(result);
        if (report.client_order_id.empty()) report.client_order_id = order.client_order_id;
        if (!report.order_quantity) report.order_quantity = order.quantity;
        return report;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

BalanceQueryResult BinanceAdapter::query_balances(std::optional<std::string> currency) {
    try {
        currency = normalized_currency(std::move(currency));
        if (!request_rate_limiter_.try_acquire(20.0)) {
            return {.code = "LOCAL_RATE_LIMIT",
                    .message = "Binance account-status budget exhausted",
                    .snapshot = {.venue = Venue::Binance}};
        }
        auto result = BinanceProtocol::parse_balances(
            signed_request("account.status", {{"omitZeroBalances", false}}));
        if (result.ok && currency) {
            std::erase_if(result.snapshot.balances, [&](const AccountBalance& balance) {
                return balance.currency != *currency;
            });
        }
        return result;
    } catch (const std::exception& error) {
        return {.code = "BINANCE_BALANCE_TRANSPORT_ERROR",
                .message = error.what(),
                .snapshot = {.venue = Venue::Binance}};
    }
}

InstrumentRulesQueryResult BinanceAdapter::query_instrument_rules(std::string symbol) {
    try {
        std::ranges::transform(symbol, symbol.begin(), [](unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
        constexpr auto cache_lifetime = std::chrono::seconds(30);
        {
            std::scoped_lock lock(instrument_cache_mutex_);
            if (const auto found = instrument_cache_.find(symbol);
                found != instrument_cache_.end() &&
                std::chrono::steady_clock::now() - found->second.first < cache_lifetime) {
                return found->second.second;
            }
        }
        if (!request_rate_limiter_.try_acquire(20.0)) {
            return {.code = "LOCAL_RATE_LIMIT",
                    .message = "Binance exchangeInfo query budget exhausted",
                    .rules = {.venue = Venue::Binance, .symbol = symbol}};
        }
        auto result = BinanceProtocol::parse_instrument_rules(request(
            "exchangeInfo", {{"symbol", BinanceProtocol::symbol_to_exchange(symbol)},
                              {"showPermissionSets", false}}));
        if (result.ok) {
            std::scoped_lock lock(instrument_cache_mutex_);
            instrument_cache_[symbol] = {std::chrono::steady_clock::now(), result};
        }
        return result;
    } catch (const std::exception& error) {
        return {.code = "BINANCE_INSTRUMENT_RULES_TRANSPORT_ERROR",
                .message = error.what(),
                .rules = {.venue = Venue::Binance, .symbol = std::move(symbol)}};
    }
}

std::optional<std::vector<ExecutionReport>> BinanceAdapter::query_open_orders() {
    if (!request_rate_limiter_.try_acquire(40.0)) return std::nullopt;
    try {
        const auto response = signed_request("openOrders.status", nlohmann::json::object());
        if (response.value("status", 0) != 200 || !response.contains("result") ||
            !response.at("result").is_array()) {
            return std::nullopt;
        }
        std::vector<ExecutionReport> reports;
        reports.reserve(response.at("result").size());
        for (const auto& result : response.at("result")) {
            reports.push_back(BinanceProtocol::parse_order_status(result));
        }
        return reports;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

nlohmann::json BinanceAdapter::request(std::string method,
                                       nlohmann::json parameters) {
    {
        std::unique_lock lock(pending_mutex_);
        if (!subscription_condition_.wait_for(lock, config_.request_timeout,
                                              [this] { return subscribed_.load(); })) {
            throw std::runtime_error("Binance user-data subscription is not ready");
        }
    }
    const auto id = next_request_id("request");
    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto future = promise->get_future();
    {
        std::scoped_lock lock(pending_mutex_);
        pending_[id] = promise;
    }
    const nlohmann::json request{
        {"id", id},
        {"method", std::move(method)},
        {"params", std::move(parameters)},
    };
    if (!websocket_.send(request.dump())) {
        std::scoped_lock lock(pending_mutex_);
        pending_.erase(id);
        throw std::runtime_error("Binance WebSocket is disconnected");
    }
    if (future.wait_for(config_.request_timeout) != std::future_status::ready) {
        std::scoped_lock lock(pending_mutex_);
        pending_.erase(id);
        throw std::runtime_error("Binance request acknowledgement timed out; outcome unknown");
    }
    auto response = future.get();
    observe_rate_limits(response);
    return response;
}

nlohmann::json BinanceAdapter::signed_request(std::string method,
                                              nlohmann::json parameters) {
    return request(std::move(method), sign_parameters(std::move(parameters)));
}

nlohmann::json BinanceAdapter::sign_parameters(nlohmann::json parameters) const {
    parameters["apiKey"] = config_.api_key;
    parameters["recvWindow"] = 5000;
    parameters["timestamp"] = signed_timestamp();
    parameters["signature"] = hmac_sha256_hex(config_.secret_key, canonical_query(parameters));
    return parameters;
}

std::int64_t BinanceAdapter::signed_timestamp() const {
    std::scoped_lock lock(clock_mutex_);
    if (!clock_synchronized_) {
        throw std::runtime_error("Binance server time is not synchronized");
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - clock_synchronized_at_);
    const auto backward_margin = std::max(
        config_.timestamp_safety_margin.count(), clock_uncertainty_ms_ + 25);
    return server_time_at_sync_ms_ + elapsed.count() - backward_margin;
}

std::string BinanceAdapter::next_request_id(std::string_view prefix) {
    return "abex-" + std::string(prefix) + '-' + std::to_string(next_id_.fetch_add(1));
}

void BinanceAdapter::observe_rate_limits(const nlohmann::json& response) {
    if (!response.contains("rateLimits") || !response.at("rateLimits").is_array()) return;
    for (const auto& limit : response.at("rateLimits")) {
        if (limit.value("rateLimitType", std::string{}) != "REQUEST_WEIGHT") continue;
        const auto capacity = limit.value("limit", 0.0);
        const auto count = limit.value("count", 0.0);
        const auto interval_number = limit.value("intervalNum", 1.0);
        const auto interval = limit.value("interval", std::string{"MINUTE"});
        double interval_seconds = 60.0;
        if (interval == "SECOND") interval_seconds = 1.0;
        else if (interval == "DAY") interval_seconds = 86400.0;
        else if (interval == "HOUR") interval_seconds = 3600.0;
        interval_seconds *= interval_number;
        if (capacity > 0.0 && interval_seconds > 0.0) {
            request_rate_limiter_.synchronize(
                capacity, std::max(0.0, capacity - count), capacity / interval_seconds);
            return;
        }
    }
}

void BinanceAdapter::request_server_time() {
    if (!websocket_.connected()) return;
    const auto id = next_request_id("time");
    {
        std::scoped_lock lock(pending_mutex_);
        time_request_id_ = id;
        time_request_sent_at_ = std::chrono::steady_clock::now();
    }
    const nlohmann::json request{
        {"id", id},
        {"method", "time"},
    };
    (void)websocket_.send(request.dump());
}

void BinanceAdapter::run_clock_sync(std::stop_token stop_token) {
    std::unique_lock lock(clock_sync_wait_mutex_);
    while (!stop_token.stop_requested()) {
        if (clock_sync_condition_.wait_for(
                lock, config_.server_time_resync_interval,
                [&stop_token] { return stop_token.stop_requested(); })) {
            break;
        }
        lock.unlock();
        request_server_time();
        lock.lock();
    }
}

void BinanceAdapter::websocket_opened() {
    subscribed_.store(false);
    {
        std::scoped_lock lock(pending_mutex_);
        subscription_request_id_.clear();
    }
    {
        std::scoped_lock lock(clock_mutex_);
        clock_synchronized_ = false;
    }
    request_server_time();
}

void BinanceAdapter::subscribe_user_data() {
    const auto id = next_request_id("subscribe");
    {
        std::scoped_lock lock(pending_mutex_);
        subscription_request_id_ = id;
    }
    const nlohmann::json request{
        {"id", id},
        {"method", "userDataStream.subscribe.signature"},
        {"params", sign_parameters(nlohmann::json::object())},
    };
    (void)websocket_.send(request.dump());
}

void BinanceAdapter::websocket_message(std::string_view message) {
    try {
        const auto received_at = std::chrono::steady_clock::now();
        auto json = nlohmann::json::parse(message);
        const auto id = json_id(json);
        if (!id.empty()) {
            std::shared_ptr<std::promise<nlohmann::json>> promise;
            std::chrono::steady_clock::time_point time_request_sent_at;
            bool time_response = false;
            bool subscription_response = false;
            bool subscription_requested = false;
            {
                std::scoped_lock lock(pending_mutex_);
                time_response = id == time_request_id_;
                if (time_response) time_request_sent_at = time_request_sent_at_;
                subscription_response = id == subscription_request_id_;
                subscription_requested = !subscription_request_id_.empty();
                if (const auto found = pending_.find(id); found != pending_.end()) {
                    promise = found->second;
                    pending_.erase(found);
                }
            }
            if (time_response) {
                const bool accepted = json.value("status", 0) == 200 &&
                                      json.contains("result") &&
                                      json.at("result").contains("serverTime");
                if (!accepted) {
                    if (connection_callback_) {
                        connection_callback_(Venue::Binance, false,
                                             "Binance server-time synchronization was rejected");
                    }
                    return;
                }
                {
                    std::scoped_lock lock(clock_mutex_);
                    server_time_at_sync_ms_ =
                        json.at("result").at("serverTime").get<std::int64_t>();
                    // Midpoint compensation estimates the instant represented by
                    // serverTime without depending on the host wall clock. The
                    // signing path applies an additional backward safety margin.
                    clock_synchronized_at_ =
                        time_request_sent_at <= received_at
                            ? time_request_sent_at + (received_at - time_request_sent_at) / 2
                            : received_at;
                    clock_uncertainty_ms_ =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            received_at - time_request_sent_at)
                            .count() /
                        2;
                    clock_synchronized_ = true;
                }
                if (!subscribed_.load() && !subscription_requested) subscribe_user_data();
                return;
            }
            if (subscription_response) {
                const bool accepted = json.value("status", 0) == 200;
                subscribed_.store(accepted);
                subscription_condition_.notify_all();
                std::string reason;
                if (!accepted) {
                    reason = "Binance user-data subscription rejected";
                    if (json.contains("error")) {
                        const auto& error = json.at("error");
                        if (error.contains("code")) reason += " (" + error.at("code").dump() + ")";
                        if (error.contains("msg") && error.at("msg").is_string()) {
                            reason += ": " + error.at("msg").get<std::string>();
                        }
                    }
                }
                if (connection_callback_) {
                    connection_callback_(Venue::Binance, accepted, std::move(reason));
                }
            }
            if (promise) promise->set_value(std::move(json));
            return;
        }

        const auto& event = json.contains("event") ? json.at("event") : json;
        if (event.value("e", std::string{}) != "executionReport") return;
        auto report = BinanceProtocol::parse_execution_report(json);
        {
            std::scoped_lock lock(alias_mutex_);
            if (const auto alias = alias_to_client_.find(report.client_order_id);
                alias != alias_to_client_.end()) {
                report.client_order_id = alias->second;
            }
        }
        if (execution_callback_) execution_callback_(Venue::Binance, std::move(report));
    } catch (const std::exception&) {
        // Ignore an isolated malformed frame; startup/reconnect reconciliation is authoritative.
    }
}

void BinanceAdapter::websocket_connection(bool connected, std::string_view reason) {
    if (connected) return; // report ready only after signed user-data subscription succeeds
    subscribed_.store(false);
    {
        std::scoped_lock lock(clock_mutex_);
        clock_synchronized_ = false;
    }
    subscription_condition_.notify_all();
    fail_pending(reason.empty() ? "Binance WebSocket disconnected" : reason);
    if (connection_callback_) {
        connection_callback_(Venue::Binance, false, std::string(reason));
    }
}

void BinanceAdapter::fail_pending(std::string_view reason) {
    StringMap<std::shared_ptr<std::promise<nlohmann::json>>> pending;
    {
        std::scoped_lock lock(pending_mutex_);
        pending.swap(pending_);
    }
    for (auto& [id, promise] : pending) {
        (void)id;
        try {
            promise->set_exception(
                std::make_exception_ptr(std::runtime_error(std::string(reason))));
        } catch (const std::future_error&) {
        }
    }
}

} // namespace abex
