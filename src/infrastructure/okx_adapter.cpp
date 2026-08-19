#include "abex/infrastructure/okx_adapter.hpp"

#include "abex/infrastructure/crypto.hpp"
#include "abex/infrastructure/exchange_protocols.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
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

[[nodiscard]] std::string normalized_symbol(std::string symbol) {
    std::ranges::transform(symbol, symbol.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    if (symbol.empty() || !std::ranges::all_of(symbol, [](unsigned char character) {
            return std::isalnum(character) != 0 || character == '-';
        })) {
        throw std::invalid_argument("instrument symbol must be canonical alphanumeric ASSET-ASSET");
    }
    return symbol;
}

} // namespace

OkxAdapter::Config OkxAdapter::Config::from_environment(const nlohmann::json& json, bool demo) {
    return {
        .rest_url = json.value("restUrl", "https://openapi.okx.com"),
        .private_websocket_url =
            json.value("privateWebSocketUrl", "wss://wspap.okx.com:8443/ws/v5/private"),
        .api_key = environment_required("ABEX_OKX_API_KEY"),
        .secret_key = environment_required("ABEX_OKX_SECRET_KEY"),
        .passphrase = environment_required("ABEX_OKX_PASSPHRASE"),
        .demo = demo,
    };
}

OkxAdapter::OkxAdapter(Config config)
    : config_(std::move(config)),
      websocket_({.url = config_.private_websocket_url,
                  .application_heartbeat_request = "ping",
                  .application_heartbeat_response = "pong",
                  .application_heartbeat_idle = std::chrono::seconds{20},
                  .application_heartbeat_timeout = std::chrono::seconds{8}}) {}

OkxAdapter::~OkxAdapter() { stop(); }

void OkxAdapter::start(ExecutionCallback execution_callback,
                       ConnectionCallback connection_callback) {
    {
        std::scoped_lock lock(callback_mutex_);
        execution_callback_ = std::move(execution_callback);
        connection_callback_ = std::move(connection_callback);
    }
    websocket_.start([this] { websocket_opened(); },
                     [this](std::string_view message) { websocket_message(message); },
                     [this](bool connected, std::string_view reason) {
                         websocket_connection(connected, reason);
                     });
}

void OkxAdapter::stop() noexcept {
    websocket_.stop();
    authenticated_.store(false);
}

void OkxAdapter::restore(std::span<const Order> recovered_orders) {
    std::scoped_lock lock(callback_mutex_);
    for (const auto& order : recovered_orders) {
        if (order.venue != Venue::Okx) continue;
        alias_to_client_[order.client_order_id] = order.client_order_id;
        alias_to_client_[OkxProtocol::client_id_to_exchange(order.client_order_id)] =
            order.client_order_id;
        for (const auto& alias : order.exchange_client_id_aliases) {
            alias_to_client_[alias] = order.client_order_id;
        }
    }
}

AdapterResult OkxAdapter::place(const Order& order) {
    if (!authenticated_.load()) {
        return {.accepted = false,
                .code = "OKX_NOT_READY",
                .message = "OKX private order stream is not subscribed"};
    }
    if (!order_rate_limiter_.try_acquire()) {
        return {.accepted = false,
                .code = "LOCAL_RATE_LIMIT",
                .message = "OKX place-order budget exhausted"};
    }
    {
        std::scoped_lock lock(callback_mutex_);
        alias_to_client_[OkxProtocol::client_id_to_exchange(order.client_order_id)] =
            order.client_order_id;
    }
    auto result = write_operation("/api/v5/trade/order", OkxProtocol::place_request(order));
    if (result.accepted && order.type == OrderType::Market &&
        !result.exchange_order_id.empty()) {
        // The placement ACK is not an execution ACK. Query once immediately so
        // a market order reaches its authoritative terminal state even if a
        // private WebSocket update is delayed. A later identical push is deduplicated.
        try {
            auto acknowledged = order;
            acknowledged.exchange_order_id = result.exchange_order_id;
            if (auto report = query(acknowledged)) {
                ExecutionCallback callback;
                {
                    std::scoped_lock lock(callback_mutex_);
                    callback = execution_callback_;
                }
                if (callback) callback(Venue::Okx, std::move(*report));
            }
        } catch (const std::exception&) {
            // The accepted ACK remains valid; the subscribed stream and normal
            // reconciliation path remain authoritative after a failed spot query.
        }
    }
    return result;
}

AdapterResult OkxAdapter::cancel(const Order& order) {
    if (!order_rate_limiter_.try_acquire()) {
        return {.accepted = false,
                .code = "LOCAL_RATE_LIMIT",
                .message = "OKX cancel-order budget exhausted"};
    }
    return write_operation("/api/v5/trade/cancel-order", OkxProtocol::cancel_request(order));
}

AdapterResult OkxAdapter::amend(const Order& order,
                                std::optional<Decimal> new_price,
                                std::optional<Decimal> new_quantity) {
    if (!order_rate_limiter_.try_acquire()) {
        return {.accepted = false,
                .code = "LOCAL_RATE_LIMIT",
                .message = "OKX amend-order budget exhausted"};
    }
    auto result = write_operation("/api/v5/trade/amend-order",
                                  OkxProtocol::amend_request(order, new_price, new_quantity));
    // OKX explicitly defines this response as request acceptance, not final
    // amendment state. Query promptly; the private order stream remains the
    // normal authoritative path if the query still shows the old terms.
    if (result.accepted) {
        if (auto report = query(order)) {
            ExecutionCallback callback;
            {
                std::scoped_lock lock(callback_mutex_);
                callback = execution_callback_;
            }
            if (callback) callback(Venue::Okx, std::move(*report));
        }
    }
    return result;
}

std::optional<ExecutionReport> OkxAdapter::query(const Order& order) {
    if (!order_rate_limiter_.try_acquire()) return std::nullopt;
    std::string path = "/api/v5/trade/order?instId=" + order.symbol;
    if (!order.exchange_order_id.empty()) path += "&ordId=" + order.exchange_order_id;
    else path += "&clOrdId=" + OkxProtocol::client_id_to_exchange(order.client_order_id);
    const auto response = rest_request("GET", path);
    if (response.value("code", std::string{}) != "0" || !response.contains("data") ||
        response.at("data").empty()) {
        return std::nullopt;
    }
    auto report = OkxProtocol::parse_order_update(response.at("data").front());
    report.client_order_id = order.client_order_id;
    if (order.type == OrderType::Market && report.status == OrderStatus::Filled &&
        report.cumulative_filled < order.quantity) {
        report.status = OrderStatus::Canceled;
        report.reason = "OKX completed an automatically reduced market order: filled " +
                        report.cumulative_filled.to_string() + " of requested " +
                        order.quantity.to_string();
    }
    return report;
}

BalanceQueryResult OkxAdapter::query_balances(std::optional<std::string> currency) {
    try {
        currency = normalized_currency(std::move(currency));
        if (!order_rate_limiter_.try_acquire(2.0)) {
            return {.code = "LOCAL_RATE_LIMIT",
                    .message = "OKX balance-query budget exhausted",
                    .snapshot = {.venue = Venue::Okx}};
        }
        const auto account_config = rest_request("GET", "/api/v5/account/config");
        auto path = std::string{"/api/v5/account/balance"};
        if (currency) path += "?ccy=" + *currency;
        return OkxProtocol::parse_balances(account_config, rest_request("GET", std::move(path)));
    } catch (const std::exception& error) {
        return {.code = "OKX_BALANCE_TRANSPORT_ERROR",
                .message = error.what(),
                .snapshot = {.venue = Venue::Okx}};
    }
}

InstrumentRulesQueryResult OkxAdapter::query_instrument_rules(std::string symbol) {
    try {
        symbol = normalized_symbol(std::move(symbol));
        constexpr auto cache_lifetime = std::chrono::seconds(30);
        {
            std::scoped_lock lock(instrument_cache_mutex_);
            if (const auto found = instrument_cache_.find(symbol);
                found != instrument_cache_.end() &&
                std::chrono::steady_clock::now() - found->second.first < cache_lifetime) {
                return found->second.second;
            }
        }
        if (!order_rate_limiter_.try_acquire()) {
            return {.code = "LOCAL_RATE_LIMIT",
                    .message = "OKX instrument-rule query budget exhausted",
                    .rules = {.venue = Venue::Okx, .symbol = symbol}};
        }
        auto result = OkxProtocol::parse_instrument_rules(rest_request(
            "GET", "/api/v5/account/instruments?instType=SPOT&instId=" + symbol));
        if (result.ok) {
            std::scoped_lock lock(instrument_cache_mutex_);
            instrument_cache_[symbol] = {std::chrono::steady_clock::now(), result};
        }
        return result;
    } catch (const std::exception& error) {
        return {.code = "OKX_INSTRUMENT_RULES_TRANSPORT_ERROR",
                .message = error.what(),
                .rules = {.venue = Venue::Okx, .symbol = std::move(symbol)}};
    }
}

std::optional<std::vector<ExecutionReport>> OkxAdapter::query_open_orders() {
    if (!order_rate_limiter_.try_acquire()) return std::nullopt;
    try {
        const auto response = rest_request(
            "GET", "/api/v5/trade/orders-pending?instType=SPOT");
        if (response.value("code", std::string{}) != "0" ||
            !response.contains("data") || !response.at("data").is_array()) {
            return std::nullopt;
        }
        std::vector<ExecutionReport> reports;
        reports.reserve(response.at("data").size());
        for (const auto& item : response.at("data")) {
            reports.push_back(OkxProtocol::parse_order_update(item));
        }
        return reports;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

nlohmann::json OkxAdapter::rest_request(std::string method,
                                        std::string request_path,
                                        std::string body) const {
    const auto timestamp = iso8601_utc_now();
    const auto signature = hmac_sha256_base64(
        config_.secret_key, timestamp + method + request_path + body);
    HttpRequest request{
        .method = std::move(method),
        .url = config_.rest_url + request_path,
        .headers = {
            {"Accept", "application/json"},
            {"Content-Type", "application/json"},
            {"OK-ACCESS-KEY", config_.api_key},
            {"OK-ACCESS-SIGN", signature},
            {"OK-ACCESS-TIMESTAMP", timestamp},
            {"OK-ACCESS-PASSPHRASE", config_.passphrase},
        },
        .body = std::move(body),
    };
    if (config_.demo) request.headers["x-simulated-trading"] = "1";
    const auto response = http_.perform(request);
    if (response.status == 429) throw std::runtime_error("OKX HTTP rate limit exceeded");
    if (response.status >= 500) {
        throw std::runtime_error("OKX server error HTTP " + std::to_string(response.status));
    }
    if (response.body.empty()) throw std::runtime_error("OKX returned an empty response");
    return nlohmann::json::parse(response.body);
}

AdapterResult OkxAdapter::write_operation(std::string_view path, const nlohmann::json& body) {
    try {
        const auto response = rest_request("POST", std::string(path), body.dump());
        return OkxProtocol::parse_ack(response);
    } catch (const std::exception& error) {
        return {.accepted = false,
                .outcome_uncertain = true,
                .code = "OKX_TRANSPORT_ERROR",
                .message = error.what()};
    }
}

void OkxAdapter::websocket_opened() {
    const auto timestamp = std::to_string(std::time(nullptr));
    const auto signature = hmac_sha256_base64(
        config_.secret_key, timestamp + "GET/users/self/verify");
    const nlohmann::json login{
        {"op", "login"},
        {"args", nlohmann::json::array({{
             {"apiKey", config_.api_key},
             {"passphrase", config_.passphrase},
             {"timestamp", timestamp},
             {"sign", signature},
         }})},
    };
    (void)websocket_.send(login.dump());
}

void OkxAdapter::websocket_message(std::string_view message) {
    try {
        const auto json = nlohmann::json::parse(message);
        if (json.value("event", std::string{}) == "login") {
            if (json.value("code", std::string{}) != "0") {
                websocket_connection(false, json.value("msg", "OKX login rejected"));
                return;
            }
            authenticated_.store(false);
            const nlohmann::json subscription{
                {"id", "abexorders"},
                {"op", "subscribe"},
                {"args", nlohmann::json::array({{
                     {"channel", "orders"},
                     {"instType", "ANY"},
                 }})},
            };
            (void)websocket_.send(subscription.dump());
            return;
        }
        const auto event = json.value("event", std::string{});
        const bool orders_channel = json.contains("arg") &&
                                    json.at("arg").value("channel", std::string{}) == "orders";
        if (event == "subscribe" && orders_channel) {
            authenticated_.store(true);
            ConnectionCallback callback;
            {
                std::scoped_lock lock(callback_mutex_);
                callback = connection_callback_;
            }
            if (callback) callback(Venue::Okx, true, {});
            return;
        }
        if (event == "error" || event == "channel-conn-count-error") {
            authenticated_.store(false);
            std::string reason = "OKX private order subscription rejected";
            const auto code = json.value("code", std::string{});
            const auto detail = json.value("msg", std::string{});
            if (!code.empty()) reason += " (" + code + ')';
            if (!detail.empty()) reason += ": " + detail;
            ConnectionCallback callback;
            {
                std::scoped_lock lock(callback_mutex_);
                callback = connection_callback_;
            }
            if (callback) callback(Venue::Okx, false, std::move(reason));
            return;
        }
        if (!json.contains("arg") || json.at("arg").value("channel", std::string{}) != "orders" ||
            !json.contains("data")) {
            return;
        }
        ExecutionCallback callback;
        {
            std::scoped_lock lock(callback_mutex_);
            callback = execution_callback_;
        }
        if (callback) {
            for (const auto& item : json.at("data")) {
                auto report = OkxProtocol::parse_order_update(item);
                {
                    std::scoped_lock lock(callback_mutex_);
                    if (const auto alias = alias_to_client_.find(report.client_order_id);
                        alias != alias_to_client_.end()) {
                        report.client_order_id = alias->second;
                    }
                }
                callback(Venue::Okx, std::move(report));
            }
        }
    } catch (const std::exception&) {
        // A malformed venue frame is isolated; reconciliation remains the recovery path.
    }
}

void OkxAdapter::websocket_connection(bool connected, std::string_view reason) {
    if (connected) return; // application-level connected is reported only after login
    authenticated_.store(false);
    ConnectionCallback callback;
    {
        std::scoped_lock lock(callback_mutex_);
        callback = connection_callback_;
    }
    if (callback) callback(Venue::Okx, false, std::string(reason));
}

} // namespace abex
