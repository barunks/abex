#include "abex/infrastructure/exchange_protocols.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <stdexcept>

namespace abex {
namespace {

[[nodiscard]] std::string_view okx_order_type(const Order& order) {
    if (order.type == OrderType::Market) return "market";
    switch (order.time_in_force) {
    case TimeInForce::Gtc: return "limit";
    case TimeInForce::Ioc: return "ioc";
    case TimeInForce::Fok: return "fok";
    }
    throw std::logic_error("unsupported OKX time in force");
}

[[nodiscard]] OrderStatus okx_status(std::string_view status) {
    if (status == "live") return OrderStatus::Live;
    if (status == "partially_filled") return OrderStatus::PartiallyFilled;
    if (status == "filled") return OrderStatus::Filled;
    if (status == "canceled" || status == "mmp_canceled") return OrderStatus::Canceled;
    if (status == "rejected" || status == "order_failed") return OrderStatus::Rejected;
    return OrderStatus::Unknown;
}

[[nodiscard]] OrderStatus binance_status(std::string_view status) {
    if (status == "NEW" || status == "PENDING_NEW") return OrderStatus::Live;
    if (status == "PARTIALLY_FILLED") return OrderStatus::PartiallyFilled;
    if (status == "FILLED") return OrderStatus::Filled;
    if (status == "CANCELED" || status == "EXPIRED" || status == "EXPIRED_IN_MATCH") {
        return OrderStatus::Canceled;
    }
    if (status == "REJECTED") return OrderStatus::Rejected;
    return OrderStatus::Unknown;
}

[[nodiscard]] Decimal decimal_field(const nlohmann::json& json,
                                    std::string_view field,
                                    std::string_view fallback = "0") {
    const auto found = json.find(field);
    if (found == json.end() || found->is_null()) return Decimal::parse(fallback);
    if (found->is_string()) return Decimal::parse(found->get_ref<const std::string&>());
    return Decimal::parse(found->dump());
}

[[nodiscard]] std::string string_field(const nlohmann::json& json,
                                       std::string_view field,
                                       std::string fallback = {}) {
    const auto found = json.find(field);
    if (found == json.end() || found->is_null()) return fallback;
    if (found->is_string()) return found->get_ref<const std::string&>();
    return found->dump();
}

[[nodiscard]] bool okx_client_id_character(unsigned char character) noexcept {
    return (character >= '0' && character <= '9') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z');
}

[[nodiscard]] std::uint64_t fnv1a_64(std::string_view value) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char character : value) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    return hash;
}

void add_numeric_order_id(nlohmann::json& params,
                          std::string_view key,
                          const std::string& value) {
    std::uint64_t parsed{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error == std::errc{} && end == value.data() + value.size()) {
        params[std::string(key)] = parsed;
    }
}

[[nodiscard]] ExecutionReport binance_order_response_report(const nlohmann::json& result,
                                                            std::string_view operation) {
    const auto order_id = string_field(result, "orderId");
    const auto event_time = string_field(
        result, "transactTime", string_field(result, "updateTime", "0"));
    const auto status = string_field(result, "status");
    const auto cumulative = decimal_field(result, "executedQty");
    ExecutionReport report{
        .event_id = "binance-" + std::string(operation) + '-' + order_id + '-' + event_time +
                    '-' + status + '-' + cumulative.to_string(),
        .client_order_id = string_field(result, "clientOrderId",
                                        string_field(result, "origClientOrderId")),
        .exchange_order_id = order_id,
        .status = binance_status(status),
        .cumulative_filled = cumulative,
        .cumulative_quote = decimal_field(result, "cummulativeQuoteQty"),
        .event_time_ms = std::stoll(event_time),
        .reason = string_field(result, "rejectReason"),
    };
    if (result.contains("price")) report.order_price = decimal_field(result, "price");
    if (result.contains("origQty")) report.order_quantity = decimal_field(result, "origQty");
    return report;
}

[[nodiscard]] std::pair<std::string, std::string>
binance_nested_error(const nlohmann::json& value,
                     std::string fallback_code,
                     std::string fallback_message) {
    if (!value.is_object()) return {std::move(fallback_code), std::move(fallback_message)};
    return {string_field(value, "code", std::move(fallback_code)),
            string_field(value, "msg", std::move(fallback_message))};
}

[[nodiscard]] Decimal venue_decimal(std::string_view text) {
    const auto exponent = text.find_first_of("eE");
    if (exponent != std::string_view::npos) {
        throw std::invalid_argument("scientific notation is not supported for venue balances");
    }
    if (const auto dot = text.find('.'); dot != std::string_view::npos) {
        const auto maximum_size = dot + 1 + static_cast<std::size_t>(Decimal::precision);
        if (text.size() > maximum_size) text = text.substr(0, maximum_size);
    }
    return Decimal::parse(text.empty() ? "0" : text);
}

[[nodiscard]] std::optional<Decimal> positive_decimal_field(const nlohmann::json& json,
                                                            std::string_view field) {
    const auto text = string_field(json, field);
    if (text.empty()) return std::nullopt;
    try {
        const auto value = venue_decimal(text);
        return value.is_positive() ? std::optional(value) : std::nullopt;
    } catch (const std::overflow_error&) {
        // An effectively-unbounded venue maximum can exceed ABEX's monetary domain.
        // Omitting that ceiling is safe because configured gateway maximums remain active.
        return std::nullopt;
    }
}

void keep_larger(std::optional<Decimal>& target, std::optional<Decimal> candidate) {
    if (candidate && (!target || *candidate > *target)) target = candidate;
}

void keep_smaller(std::optional<Decimal>& target, std::optional<Decimal> candidate) {
    if (candidate && (!target || *candidate < *target)) target = candidate;
}

} // namespace

std::string OkxProtocol::client_id_to_exchange(std::string_view client_order_id) {
    if (!client_order_id.empty() && client_order_id.size() <= 32 &&
        std::ranges::all_of(client_order_id, okx_client_id_character)) {
        return std::string(client_order_id);
    }

    // Keep a readable prefix and a deterministic hash suffix so different
    // normalized IDs remain exceedingly unlikely to collide across restarts.
    std::string prefix;
    prefix.reserve(13);
    for (const unsigned char character : client_order_id) {
        if (okx_client_id_character(character)) prefix.push_back(static_cast<char>(character));
        if (prefix.size() == 13) break;
    }

    std::array<char, 16> hash{};
    hash.fill('0');
    std::array<char, 16> encoded{};
    const auto [end, error] =
        std::to_chars(encoded.data(), encoded.data() + encoded.size(), fnv1a_64(client_order_id), 16);
    if (error != std::errc{}) throw std::logic_error("failed to encode OKX client id hash");
    const auto encoded_size = static_cast<std::size_t>(end - encoded.data());
    std::copy_n(encoded.data(), encoded_size, hash.end() - static_cast<std::ptrdiff_t>(encoded_size));

    std::string result;
    result.reserve(3 + prefix.size() + hash.size());
    result += "abx";
    result += prefix;
    result.append(hash.data(), hash.size());
    return result;
}

nlohmann::json OkxProtocol::place_request(const Order& order) {
    nlohmann::json request{
        {"instId", order.symbol},
        {"tdMode", "cash"},
        {"clOrdId", client_id_to_exchange(order.client_order_id)},
        {"side", order.side == Side::Buy ? "buy" : "sell"},
        {"ordType", okx_order_type(order)},
        {"sz", order.quantity.to_string()},
    };
    if (order.price) request["px"] = order.price->to_string();
    if (order.type == OrderType::Market) {
        request["tgtCcy"] = "base_ccy";
        // Preserve the exchange-neutral quantity contract. Without this flag,
        // OKX may silently reduce an underfunded SPOT market order and report
        // the smaller venue quantity as fully filled.
        request["banAmend"] = true;
    }
    return request;
}

nlohmann::json OkxProtocol::cancel_request(const Order& order) {
    nlohmann::json request{{"instId", order.symbol}};
    if (!order.exchange_order_id.empty()) request["ordId"] = order.exchange_order_id;
    else request["clOrdId"] = client_id_to_exchange(order.client_order_id);
    return request;
}

nlohmann::json OkxProtocol::amend_request(const Order& order,
                                          std::optional<Decimal> new_price,
                                          std::optional<Decimal> new_quantity) {
    auto request = cancel_request(order);
    request["cxlOnFail"] = false;
    request["reqId"] =
        client_id_to_exchange(order.client_order_id + "v" + std::to_string(order.version + 1));
    if (new_price) request["newPx"] = new_price->to_string();
    if (new_quantity) request["newSz"] = new_quantity->to_string();
    return request;
}

ExecutionReport OkxProtocol::parse_order_update(const nlohmann::json& data) {
    const auto order_id = string_field(data, "ordId");
    const auto update_time = string_field(data, "uTime", "0");
    const auto state = string_field(data, "state");
    const auto trade_id = string_field(data, "tradeId");
    const auto average_price = string_field(data, "avgPx");
    const auto cumulative_filled = decimal_field(data, "accFillSz");

    ExecutionReport report{
        .event_id = !trade_id.empty() && trade_id != "0"
                        ? "okx-trade-" + trade_id
                        : "okx-order-" + order_id + '-' + update_time + '-' + state + '-' +
                              cumulative_filled.to_string(),
        .client_order_id = string_field(data, "clOrdId"),
        .exchange_order_id = order_id,
        .status = okx_status(state),
        .cumulative_filled = cumulative_filled,
        .event_time_ms = std::stoll(update_time),
        .reason = string_field(data, "cancelSourceReason",
                               string_field(data, "amendResult")),
    };
    if (!average_price.empty()) {
        report.cumulative_quote = cumulative_filled * Decimal::parse(average_price);
    }
    const auto fill_price = string_field(data, "fillPx");
    if (!fill_price.empty()) report.last_fill_price = Decimal::parse(fill_price);
    const auto order_price = string_field(data, "px");
    if (!order_price.empty()) report.order_price = Decimal::parse(order_price);
    const auto order_quantity = string_field(data, "sz");
    if (!order_quantity.empty()) report.order_quantity = Decimal::parse(order_quantity);
    return report;
}

AdapterResult OkxProtocol::parse_ack(const nlohmann::json& response) {
    // OKX uses top-level code 1/2 to summarize batch outcomes. Even the
    // single-order REST endpoints can return that envelope, while the
    // actionable rejection is carried by data[].sCode/sMsg/subCode.
    if (response.contains("data") && response.at("data").is_array() &&
        !response.at("data").empty()) {
        const auto& item = response.at("data").front();
        const auto item_code = string_field(item, "sCode");
        if (!item_code.empty() && item_code != "0") {
            auto message = string_field(item, "sMsg", "OKX rejected the order operation");
            const auto sub_code = string_field(item, "subCode");
            if (!sub_code.empty()) message += " (subCode " + sub_code + ')';
            return {.accepted = false,
                    .code = item_code,
                    .message = std::move(message)};
        }
    }
    if (string_field(response, "code") != "0") {
        return {.accepted = false,
                .code = string_field(response, "code", "OKX_ERROR"),
                .message = string_field(response, "msg", "OKX request failed")};
    }
    if (!response.contains("data") || response.at("data").empty()) {
        return {.accepted = false,
                .outcome_uncertain = true,
                .code = "OKX_EMPTY_ACK",
                .message = "OKX returned no per-order acknowledgement"};
    }
    const auto& item = response.at("data").front();
    return {.accepted = true,
            .exchange_order_id = string_field(item, "ordId"),
            .exchange_client_order_id = string_field(item, "clOrdId")};
}

BalanceQueryResult OkxProtocol::parse_balances(const nlohmann::json& account_config,
                                               const nlohmann::json& balance_response) {
    BalanceQueryResult result;
    result.snapshot.venue = Venue::Okx;
    if (string_field(balance_response, "code") != "0") {
        result.code = string_field(balance_response, "code", "OKX_BALANCE_ERROR");
        result.message = string_field(balance_response, "msg", "OKX balance query failed");
        return result;
    }
    if (!balance_response.contains("data") || !balance_response.at("data").is_array() ||
        balance_response.at("data").empty()) {
        result.code = "OKX_EMPTY_BALANCE";
        result.message = "OKX returned no account balance snapshot";
        return result;
    }

    if (string_field(account_config, "code") == "0" && account_config.contains("data") &&
        account_config.at("data").is_array() && !account_config.at("data").empty()) {
        const auto& identity = account_config.at("data").front();
        result.snapshot.account_id = string_field(identity, "uid");
        result.snapshot.main_account_id = string_field(identity, "mainUid");
    }

    const auto& account = balance_response.at("data").front();
    if (account.contains("uTime")) {
        result.snapshot.observed_at_ms =
            std::stoll(string_field(account, "uTime", std::to_string(unix_time_ms())));
    } else {
        result.snapshot.observed_at_ms = unix_time_ms();
    }
    if (account.contains("details") && account.at("details").is_array()) {
        for (const auto& detail : account.at("details")) {
            result.snapshot.balances.push_back({
                .currency = string_field(detail, "ccy"),
                .total = string_field(detail, "eq", "0"),
                .available = string_field(detail, "availBal", "0"),
                .frozen = string_field(detail, "frozenBal", "0"),
                .order_frozen = string_field(detail, "ordFrozen", "0"),
            });
        }
    }
    result.ok = true;
    return result;
}

InstrumentRulesQueryResult
OkxProtocol::parse_instrument_rules(const nlohmann::json& response) {
    InstrumentRulesQueryResult parsed;
    parsed.rules.venue = Venue::Okx;
    if (string_field(response, "code") != "0") {
        parsed.code = string_field(response, "code", "OKX_INSTRUMENT_RULES_ERROR");
        parsed.message = string_field(response, "msg", "OKX instrument query failed");
        return parsed;
    }
    if (!response.contains("data") || !response.at("data").is_array() ||
        response.at("data").empty()) {
        parsed.code = "OKX_INSTRUMENT_NOT_FOUND";
        parsed.message = "OKX returned no rules for the requested SPOT instrument";
        return parsed;
    }

    const auto& item = response.at("data").front();
    parsed.rules.symbol = string_field(item, "instId");
    parsed.rules.status = string_field(item, "state");
    parsed.rules.trading = parsed.rules.status == "live";
    parsed.rules.minimum_quantity = positive_decimal_field(item, "minSz");
    parsed.rules.quantity_step = positive_decimal_field(item, "lotSz");
    parsed.rules.maximum_quantity = positive_decimal_field(item, "maxLmtSz");
    parsed.rules.market_maximum_quantity = positive_decimal_field(item, "maxMktSz");
    parsed.rules.price_tick = positive_decimal_field(item, "tickSz");
    parsed.rules.maximum_notional = positive_decimal_field(item, "maxLmtAmt");
    parsed.rules.market_maximum_notional = positive_decimal_field(item, "maxMktAmt");
    parsed.rules.observed_at_ms = unix_time_ms();
    parsed.ok = true;
    return parsed;
}

std::string BinanceProtocol::symbol_to_exchange(std::string_view symbol) {
    std::string result;
    result.reserve(symbol.size());
    for (const unsigned char character : symbol) {
        if (std::isalnum(character)) {
            result.push_back(static_cast<char>(std::toupper(character)));
        }
    }
    return result;
}

nlohmann::json BinanceProtocol::place_params(const Order& order) {
    nlohmann::json params{
        {"symbol", symbol_to_exchange(order.symbol)},
        {"side", to_string(order.side)},
        {"type", to_string(order.type)},
        {"quantity", order.quantity.to_string()},
        {"newClientOrderId", order.client_order_id},
        {"newOrderRespType", "ACK"},
    };
    if (order.type == OrderType::Limit) {
        params["price"] = order.price->to_string();
        params["timeInForce"] = to_string(order.time_in_force);
    }
    return params;
}

nlohmann::json BinanceProtocol::cancel_params(const Order& order) {
    nlohmann::json params{{"symbol", symbol_to_exchange(order.symbol)}};
    if (!order.exchange_order_id.empty()) {
        add_numeric_order_id(params, "orderId", order.exchange_order_id);
        if (!params.contains("orderId")) params["origClientOrderId"] = order.client_order_id;
    } else {
        params["origClientOrderId"] = order.client_order_id;
    }
    return params;
}

nlohmann::json BinanceProtocol::amend_params(const Order& order,
                                             std::optional<Decimal> new_price,
                                             std::optional<Decimal> new_quantity,
                                             std::string new_exchange_client_id) {
    // Binance Spot cannot keep queue priority when price changes or quantity increases;
    // cancelReplace gives the common API deterministic replace semantics for those cases.
    nlohmann::json params{
        {"symbol", symbol_to_exchange(order.symbol)},
        {"side", to_string(order.side)},
        {"type", to_string(order.type)},
        {"cancelReplaceMode", "STOP_ON_FAILURE"},
        {"newClientOrderId", std::move(new_exchange_client_id)},
        {"newOrderRespType", "FULL"},
        {"quantity", new_quantity.value_or(order.quantity).to_string()},
    };
    if (!order.exchange_order_id.empty()) {
        add_numeric_order_id(params, "cancelOrderId", order.exchange_order_id);
        if (!params.contains("cancelOrderId")) {
            params["cancelOrigClientOrderId"] = order.client_order_id;
        }
    } else {
        params["cancelOrigClientOrderId"] = order.client_order_id;
    }
    if (order.type == OrderType::Limit) {
        params["price"] = new_price.value_or(*order.price).to_string();
        params["timeInForce"] = to_string(order.time_in_force);
    }
    return params;
}

ExecutionReport BinanceProtocol::parse_execution_report(const nlohmann::json& raw_event) {
    const auto& event = raw_event.contains("event") ? raw_event.at("event") : raw_event;
    const auto order_id = string_field(event, "i");
    const auto execution_id = string_field(event, "I");
    const auto event_time = string_field(event, "E", string_field(event, "T", "0"));
    const auto cumulative = decimal_field(event, "z");
    auto client_id = string_field(event, "c");
    if (client_id.empty()) client_id = string_field(event, "C");

    ExecutionReport report{
        .event_id = !execution_id.empty()
                        ? "binance-execution-" + execution_id
                        : "binance-order-" + order_id + '-' + event_time + '-' +
                              string_field(event, "x") + '-' + cumulative.to_string(),
        .client_order_id = std::move(client_id),
        .exchange_order_id = order_id,
        .status = binance_status(string_field(event, "X")),
        .cumulative_filled = cumulative,
        .cumulative_quote = decimal_field(event, "Z"),
        .event_time_ms = std::stoll(event_time),
        .reason = string_field(event, "r"),
    };
    const auto last_price = decimal_field(event, "L");
    if (last_price > Decimal{}) report.last_fill_price = last_price;
    if (event.contains("p")) report.order_price = decimal_field(event, "p");
    if (event.contains("q")) report.order_quantity = decimal_field(event, "q");
    return report;
}

AdapterResult BinanceProtocol::parse_ack(const nlohmann::json& response) {
    const auto status = response.value("status", 0);
    if (response.contains("result") && response.at("result").is_object()) {
        const auto& envelope = response.at("result");
        if (envelope.contains("cancelResult") || envelope.contains("newOrderResult")) {
            const bool cancel_succeeded =
                envelope.value("cancelResult", std::string{}) == "SUCCESS";
            const bool replacement_succeeded =
                envelope.value("newOrderResult", std::string{}) == "SUCCESS";
            AdapterResult parsed{
                .accepted = replacement_succeeded,
                .replacement = replacement_succeeded,
                .original_order_canceled = cancel_succeeded,
            };
            if (cancel_succeeded && envelope.contains("cancelResponse") &&
                envelope.at("cancelResponse").is_object()) {
                parsed.authoritative_reports.push_back(binance_order_response_report(
                    envelope.at("cancelResponse"), "cancel-replace-cancel"));
            }
            if (replacement_succeeded && envelope.contains("newOrderResponse") &&
                envelope.at("newOrderResponse").is_object()) {
                const auto& replacement = envelope.at("newOrderResponse");
                parsed.exchange_order_id = string_field(replacement, "orderId");
                parsed.exchange_client_order_id = string_field(replacement, "clientOrderId");
                parsed.authoritative_reports.push_back(binance_order_response_report(
                    replacement, "cancel-replace-new"));
            }
            if (!replacement_succeeded) {
                const auto [code, message] = binance_nested_error(
                    envelope.value("newOrderResponse", nlohmann::json::object()),
                    "BINANCE_REPLACEMENT_FAILED", "replacement order failed");
                parsed.code = code;
                parsed.message = cancel_succeeded
                                     ? "original order canceled; replacement rejected: " +
                                           code + ": " + message
                                     : "cancel and replacement failed: " + code + ": " +
                                           message;
            }
            return parsed;
        }
    }
    if (status < 200 || status >= 300) {
        const auto& error = response.contains("error") ? response.at("error") : response;
        return {.accepted = false,
                .code = string_field(error, "code", "BINANCE_ERROR"),
                .message = string_field(error, "msg", "Binance request failed")};
    }
    if (!response.contains("result") || response.at("result").is_null()) {
        return {.accepted = true};
    }
    const auto& result = response.at("result");
    return {.accepted = true,
            .exchange_order_id = string_field(result, "orderId"),
            .exchange_client_order_id = string_field(result, "clientOrderId")};
}

BalanceQueryResult BinanceProtocol::parse_balances(const nlohmann::json& response) {
    BalanceQueryResult parsed;
    parsed.snapshot.venue = Venue::Binance;
    const auto status = response.value("status", 0);
    if (status < 200 || status >= 300) {
        const auto& error = response.contains("error") ? response.at("error") : response;
        parsed.code = string_field(error, "code", "BINANCE_BALANCE_ERROR");
        parsed.message = string_field(error, "msg", "Binance balance query failed");
        return parsed;
    }
    if (!response.contains("result") || !response.at("result").is_object()) {
        parsed.code = "BINANCE_EMPTY_BALANCE";
        parsed.message = "Binance returned no account balance snapshot";
        return parsed;
    }

    const auto& account = response.at("result");
    parsed.snapshot.observed_at_ms =
        account.value("updateTime", static_cast<std::int64_t>(unix_time_ms()));
    if (account.contains("balances") && account.at("balances").is_array()) {
        for (const auto& balance : account.at("balances")) {
            const auto available = string_field(balance, "free", "0");
            const auto frozen = string_field(balance, "locked", "0");
            parsed.snapshot.balances.push_back({
                .currency = string_field(balance, "asset"),
                .total = (venue_decimal(available) + venue_decimal(frozen)).to_string(),
                .available = available,
                .frozen = frozen,
                .order_frozen = frozen,
            });
        }
    }
    parsed.ok = true;
    return parsed;
}

InstrumentRulesQueryResult
BinanceProtocol::parse_instrument_rules(const nlohmann::json& response) {
    InstrumentRulesQueryResult parsed;
    parsed.rules.venue = Venue::Binance;
    const auto response_status = response.value("status", 0);
    if (response_status < 200 || response_status >= 300) {
        const auto& error = response.contains("error") ? response.at("error") : response;
        parsed.code = string_field(error, "code", "BINANCE_INSTRUMENT_RULES_ERROR");
        parsed.message = string_field(error, "msg", "Binance exchangeInfo query failed");
        return parsed;
    }
    if (!response.contains("result") || !response.at("result").is_object() ||
        !response.at("result").contains("symbols") ||
        !response.at("result").at("symbols").is_array() ||
        response.at("result").at("symbols").empty()) {
        parsed.code = "BINANCE_INSTRUMENT_NOT_FOUND";
        parsed.message = "Binance returned no rules for the requested SPOT instrument";
        return parsed;
    }

    const auto& item = response.at("result").at("symbols").front();
    const auto base = string_field(item, "baseAsset");
    const auto quote = string_field(item, "quoteAsset");
    parsed.rules.symbol = !base.empty() && !quote.empty()
                              ? base + '-' + quote
                              : string_field(item, "symbol");
    parsed.rules.status = string_field(item, "status");
    parsed.rules.trading = parsed.rules.status == "TRADING";

    if (item.contains("filters") && item.at("filters").is_array()) {
        for (const auto& filter : item.at("filters")) {
            const auto type = string_field(filter, "filterType");
            if (type == "PRICE_FILTER") {
                parsed.rules.minimum_price = positive_decimal_field(filter, "minPrice");
                parsed.rules.maximum_price = positive_decimal_field(filter, "maxPrice");
                parsed.rules.price_tick = positive_decimal_field(filter, "tickSize");
            } else if (type == "LOT_SIZE") {
                parsed.rules.minimum_quantity = positive_decimal_field(filter, "minQty");
                parsed.rules.maximum_quantity = positive_decimal_field(filter, "maxQty");
                parsed.rules.quantity_step = positive_decimal_field(filter, "stepSize");
            } else if (type == "MARKET_LOT_SIZE") {
                parsed.rules.market_minimum_quantity = positive_decimal_field(filter, "minQty");
                parsed.rules.market_maximum_quantity = positive_decimal_field(filter, "maxQty");
                parsed.rules.market_quantity_step = positive_decimal_field(filter, "stepSize");
            } else if (type == "MIN_NOTIONAL") {
                const auto minimum = positive_decimal_field(filter, "minNotional");
                keep_larger(parsed.rules.minimum_notional, minimum);
                if (filter.value("applyToMarket", false)) {
                    keep_larger(parsed.rules.market_minimum_notional, minimum);
                }
            } else if (type == "NOTIONAL") {
                const auto minimum = positive_decimal_field(filter, "minNotional");
                const auto maximum = positive_decimal_field(filter, "maxNotional");
                keep_larger(parsed.rules.minimum_notional, minimum);
                keep_smaller(parsed.rules.maximum_notional, maximum);
                if (filter.value("applyMinToMarket", false)) {
                    keep_larger(parsed.rules.market_minimum_notional, minimum);
                }
                if (filter.value("applyMaxToMarket", false)) {
                    keep_smaller(parsed.rules.market_maximum_notional, maximum);
                }
            }
        }
    }
    parsed.rules.observed_at_ms = unix_time_ms();
    parsed.ok = true;
    return parsed;
}

} // namespace abex
