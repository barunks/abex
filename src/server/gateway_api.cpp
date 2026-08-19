#include "abex/server/gateway_api.hpp"

#include "abex/presentation/json_views.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <optional>
#include <stdexcept>
#include <string_view>

#include <nlohmann/json.hpp>

namespace abex {
namespace {

struct ParsedTarget {
    std::string_view raw_path;
    std::string decoded_path;
    StringMap<std::string> query;

    [[nodiscard]] std::string_view path() const noexcept {
        return decoded_path.empty() ? raw_path : std::string_view(decoded_path);
    }
};

[[nodiscard]] std::string decode_uri_component(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '+') {
            result.push_back(' ');
        } else if (text[index] == '%' && index + 2 < text.size()) {
            unsigned value = 0;
            const auto* begin = text.data() + index + 1;
            const auto [end, error] = std::from_chars(begin, begin + 2, value, 16);
            if (error != std::errc{} || end != begin + 2) {
                throw std::invalid_argument("invalid percent encoding in URI");
            }
            result.push_back(static_cast<char>(value));
            index += 2;
        } else {
            result.push_back(text[index]);
        }
    }
    return result;
}

[[nodiscard]] ParsedTarget parse_target(std::string_view target) {
    const auto question = target.find('?');
    const auto path = target.substr(0, question);
    ParsedTarget parsed{.raw_path = path};
    if (path.find_first_of("%+") != std::string_view::npos) {
        parsed.decoded_path = decode_uri_component(path);
    }
    if (question == std::string_view::npos) return parsed;
    auto query = target.substr(question + 1);
    while (!query.empty()) {
        const auto ampersand = query.find('&');
        const auto pair = query.substr(0, ampersand);
        const auto equals = pair.find('=');
        const auto key = decode_uri_component(pair.substr(0, equals));
        const auto value = equals == std::string_view::npos
                               ? std::string{}
                               : decode_uri_component(pair.substr(equals + 1));
        parsed.query[std::move(key)] = std::move(value);
        if (ampersand == std::string_view::npos) break;
        query.remove_prefix(ampersand + 1);
    }
    return parsed;
}

[[nodiscard]] ApiResponse json_response(unsigned status, nlohmann::json body) {
    return {
        .status = status,
        .body = body.dump(2),
        .headers = {
            {"Cache-Control", "no-store"},
            {"Access-Control-Allow-Origin", "*"},
            {"Access-Control-Allow-Headers", "Content-Type, Idempotency-Key"},
            {"Access-Control-Allow-Methods", "GET, POST, PATCH, DELETE, OPTIONS"},
        },
    };
}

[[nodiscard]] ApiResponse error_response(unsigned status,
                                         std::string code,
                                         std::string message) {
    return json_response(status, {
                                     {"ok", false},
                                     {"code", std::move(code)},
                                     {"message", std::move(message)},
                                 });
}

[[nodiscard]] unsigned error_status(std::string_view code) {
    static constexpr std::array mappings{
        std::pair{std::string_view{"ORDER_NOT_FOUND"}, 404U},
        std::pair{std::string_view{"IDEMPOTENCY_CONFLICT"}, 409U},
        std::pair{std::string_view{"ORDER_TERMINAL"}, 409U},
        std::pair{std::string_view{"OPERATION_PENDING"}, 409U},
        std::pair{std::string_view{"ORDER_STATE_UNKNOWN"}, 409U},
        std::pair{std::string_view{"LOCAL_RATE_LIMIT"}, 429U},
        std::pair{std::string_view{"RATE_LIMITED"}, 429U},
        std::pair{std::string_view{"MAX_ORDER_SIZE"}, 422U},
        std::pair{std::string_view{"MAX_NOTIONAL"}, 422U},
        std::pair{std::string_view{"POSITION_LIMIT"}, 422U},
        std::pair{std::string_view{"RISK_CONFIG_MISSING"}, 422U},
        std::pair{std::string_view{"REFERENCE_PRICE_MISSING"}, 422U},
        std::pair{std::string_view{"INSUFFICIENT_AVAILABLE_BALANCE"}, 422U},
        std::pair{std::string_view{"MIN_ORDER_QUANTITY"}, 422U},
        std::pair{std::string_view{"MAX_VENUE_ORDER_QUANTITY"}, 422U},
        std::pair{std::string_view{"INVALID_QUANTITY_STEP"}, 422U},
        std::pair{std::string_view{"MIN_ORDER_NOTIONAL"}, 422U},
        std::pair{std::string_view{"MAX_VENUE_ORDER_NOTIONAL"}, 422U},
        std::pair{std::string_view{"INVALID_PRICE_TICK"}, 422U},
        std::pair{std::string_view{"PRICE_OUT_OF_RANGE"}, 422U},
        std::pair{std::string_view{"INSTRUMENT_NOT_TRADING"}, 422U},
        std::pair{std::string_view{"ORDER_REJECTED"}, 422U},
    };
    for (const auto& [known_code, status] : mappings) {
        if (known_code == code) return status;
    }
    if (code == "OUTCOME_UNKNOWN" || code == "DISCONNECTED" ||
        code == "MARKET_DATA_UNAVAILABLE" || code == "BALANCE_UNAVAILABLE" ||
        code == "INSTRUMENT_RULES_UNAVAILABLE" ||
        code == "RECONCILIATION_INCOMPLETE" || code.find("TRANSPORT") != std::string_view::npos ||
        code.find("BALANCE_ERROR") != std::string_view::npos ||
        code.find("EMPTY_BALANCE") != std::string_view::npos ||
        code == "ADAPTER_EXCEPTION") {
        return 503;
    }
    return 400;
}

[[nodiscard]] ApiResponse operation_response(OperationResult result, unsigned success_status = 200) {
    return json_response(result.ok ? success_status : error_status(result.code),
                         operation_view(result));
}

[[nodiscard]] std::string header_value(const ApiRequest& request, std::string_view key) {
    const bool already_lowercase = std::ranges::none_of(key, [](unsigned char character) {
        return std::isupper(character) != 0;
    });
    if (already_lowercase) {
        if (const auto found = request.headers.find(key); found != request.headers.end()) {
            return found->second;
        }
        return {};
    }
    std::string normalized(key);
    std::ranges::transform(normalized, normalized.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (const auto found = request.headers.find(normalized); found != request.headers.end()) {
        return found->second;
    }
    return {};
}

[[nodiscard]] nlohmann::json parse_optional_body(const ApiRequest& request) {
    return request.body.empty() ? nlohmann::json::object() : nlohmann::json::parse(request.body);
}

void require_only_fields(const nlohmann::json& body,
                         std::initializer_list<std::string_view> allowed_fields) {
    if (!body.is_object()) throw std::invalid_argument("request body must be a JSON object");
    for (auto field = body.begin(); field != body.end(); ++field) {
        if (std::ranges::find(allowed_fields, std::string_view(field.key())) ==
            allowed_fields.end()) {
            throw std::invalid_argument("unsupported field in common schema: " + field.key());
        }
    }
}

} // namespace

ApiResponse GatewayApi::handle(const ApiRequest& request) {
    try {
        if (request.method == "OPTIONS") return json_response(204, nlohmann::json::object());
        const auto target = parse_target(request.target);

        if (request.method == "GET" && target.path() == "/api/v1/openapi.json") {
            return json_response(200, openapi_document());
        }
        if (request.method == "GET" && target.path() == "/api/v1/health") {
            return json_response(200, {
                                          {"ok", true},
                                          {"mode", runtime_mode_},
                                          {"venues", health_view(gateway_.health())},
                                          {"serverTime", unix_time_ms()},
                                      });
        }
        if (request.method == "GET" && target.path() == "/api/v1/positions") {
            return json_response(200, {
                                          {"ok", true},
                                          {"positions", positions_view(gateway_.positions())},
                                      });
        }
        if (request.method == "GET" && target.path() == "/api/v1/balances") {
            const auto venue = target.query.find("venue");
            if (venue == target.query.end() || venue->second.empty()) {
                return error_response(400, "VENUE_REQUIRED",
                                      "balance query requires venue=OKX or venue=BINANCE");
            }
            const auto currency = target.query.find("currency");
            if (currency == target.query.end() || currency->second.empty()) {
                return error_response(
                    400, "CURRENCY_REQUIRED",
                    "balance query requires the selected order currency: BTC, ETH, or USDT");
            }
            auto normalized_currency = currency->second;
            std::ranges::transform(
                normalized_currency, normalized_currency.begin(), [](unsigned char character) {
                    return static_cast<char>(std::toupper(character));
                });
            if (normalized_currency != "BTC" && normalized_currency != "ETH" &&
                normalized_currency != "USDT") {
                return error_response(
                    400, "UNSUPPORTED_CURRENCY",
                    "ABEX supports balances only for BTC-USDT and ETH-USDT order routes");
            }
            const auto result = gateway_.balances(venue_from_string(venue->second),
                                                  normalized_currency);
            return json_response(result.ok ? 200 : error_status(result.code),
                                 balance_view(result));
        }
        if (request.method == "GET" && target.path() == "/api/v1/instruments") {
            const auto venue = target.query.find("venue");
            const auto symbol = target.query.find("symbol");
            if (venue == target.query.end() || venue->second.empty()) {
                return error_response(400, "VENUE_REQUIRED",
                                      "instrument query requires venue=OKX or venue=BINANCE");
            }
            if (symbol == target.query.end() || symbol->second.empty()) {
                return error_response(400, "SYMBOL_REQUIRED",
                                      "instrument query requires symbol=BTC-USDT or ETH-USDT");
            }
            auto normalized_symbol = symbol->second;
            std::ranges::transform(
                normalized_symbol, normalized_symbol.begin(), [](unsigned char character) {
                    return static_cast<char>(std::toupper(character));
                });
            if (normalized_symbol != "BTC-USDT" && normalized_symbol != "ETH-USDT") {
                return error_response(400, "UNSUPPORTED_INSTRUMENT",
                                      "ABEX supports BTC-USDT and ETH-USDT order routes");
            }
            const auto result = gateway_.instrument_rules(
                venue_from_string(venue->second), std::move(normalized_symbol));
            return json_response(result.ok ? 200 : error_status(result.code),
                                 instrument_rules_view(result));
        }
        if (request.method == "GET" && target.path() == "/api/v1/system") {
            return json_response(200, system_view(gateway_));
        }
        if (request.method == "GET" && target.path() == "/api/v1/market-data") {
            if (!market_data_) {
                return error_response(503, "MARKET_DATA_UNAVAILABLE",
                                      "market-data ring consumer is not configured");
            }
            return json_response(200, market_data_view(*market_data_));
        }
        if (request.method == "GET" && target.path() == "/api/v1/orders") {
            std::optional<Venue> venue;
            std::optional<OrderStatus> status;
            if (const auto found = target.query.find("venue"); found != target.query.end()) {
                venue = venue_from_string(found->second);
            }
            if (const auto found = target.query.find("status"); found != target.query.end()) {
                status = order_status_from_string(found->second);
            }
            auto items = nlohmann::json::array();
            for (const auto& order : gateway_.list_snapshots(venue, status)) {
                items.push_back(order_view(order));
            }
            return json_response(200, {{"ok", true}, {"orders", std::move(items)}});
        }
        if (request.method == "POST" && target.path() == "/api/v1/orders") {
            const auto body = nlohmann::json::parse(request.body);
            require_only_fields(body, {"clientOrderId", "venue", "symbol", "side", "type",
                                       "price", "quantity", "timeInForce"});
            auto order_request = body.get<OrderRequest>();
            const auto result = gateway_.place(order_request);
            return operation_response(result, result.idempotent_replay ? 200 : 201);
        }

        constexpr std::string_view order_prefix = "/api/v1/orders/";
        if (target.path().starts_with(order_prefix) && target.path().size() > order_prefix.size()) {
            constexpr std::string_view pipeline_suffix = "/pipeline";
            const bool pipeline_request = target.path().ends_with(pipeline_suffix);
            const auto id_end = pipeline_request
                                    ? target.path().size() - pipeline_suffix.size()
                                    : target.path().size();
            const auto client_order_id = target.path().substr(order_prefix.size(),
                                                             id_end - order_prefix.size());
            if (client_order_id.find('/') != std::string::npos) {
                return error_response(404, "ROUTE_NOT_FOUND", "API route does not exist");
            }
            if (pipeline_request) {
                if (request.method != "GET") {
                    return error_response(405, "METHOD_NOT_ALLOWED",
                                          "order pipeline is read-only");
                }
                const auto order = gateway_.get_snapshot(client_order_id);
                if (!order) return error_response(404, "ORDER_NOT_FOUND", "order does not exist");
                auto events = nlohmann::json::array();
                for (const auto& event : gateway_.order_events(client_order_id)) {
                    events.push_back(operational_event_view(event));
                }
                return json_response(200, {
                                              {"ok", true},
                                              {"clientOrderId", client_order_id},
                                              {"order", order_view(*order)},
                                              {"events", std::move(events)},
                                              {"serverTime", unix_time_ms()},
                                          });
            }
            if (request.method == "GET") {
                const auto order = gateway_.get_snapshot(client_order_id);
                if (!order) return error_response(404, "ORDER_NOT_FOUND", "order does not exist");
                return json_response(200, {{"ok", true}, {"order", order_view(*order)}});
            }
            if (request.method == "DELETE") {
                const auto body = parse_optional_body(request);
                require_only_fields(body, {"requestId"});
                auto request_id = body.value("requestId", std::string{});
                if (request_id.empty()) request_id = header_value(request, "idempotency-key");
                auto result = gateway_.cancel(
                    {std::string(client_order_id), std::move(request_id)});
                gateway_.flush_events();
                if (result.order) result.order = gateway_.get(result.order->client_order_id);
                return operation_response(std::move(result));
            }
            if (request.method == "PATCH") {
                const auto body = nlohmann::json::parse(request.body);
                require_only_fields(body, {"requestId", "newPrice", "newQuantity"});
                AmendRequest amend{
                    .client_order_id = std::string(client_order_id),
                    .request_id = body.value("requestId", std::string{}),
                };
                if (amend.request_id.empty()) amend.request_id = header_value(request, "idempotency-key");
                if (body.contains("newPrice") && !body.at("newPrice").is_null()) {
                    amend.new_price = Decimal::parse(body.at("newPrice").get<std::string>());
                }
                if (body.contains("newQuantity") && !body.at("newQuantity").is_null()) {
                    amend.new_quantity = Decimal::parse(body.at("newQuantity").get<std::string>());
                }
                auto result = gateway_.amend(std::move(amend));
                gateway_.flush_events();
                if (result.order) result.order = gateway_.get(result.order->client_order_id);
                return operation_response(std::move(result));
            }
        }

        constexpr std::string_view reconcile_prefix = "/api/v1/reconcile/";
        if (request.method == "POST" && target.path().starts_with(reconcile_prefix) &&
            target.path().size() > reconcile_prefix.size()) {
            return operation_response(
                gateway_.reconcile(venue_from_string(target.path().substr(reconcile_prefix.size()))));
        }
        return error_response(404, "ROUTE_NOT_FOUND", "API route does not exist");
    } catch (const nlohmann::json::exception& error) {
        return error_response(400, "INVALID_JSON", error.what());
    } catch (const std::invalid_argument& error) {
        return error_response(400, "INVALID_REQUEST", error.what());
    } catch (const std::exception& error) {
        return error_response(500, "INTERNAL_ERROR", error.what());
    }
}

nlohmann::json GatewayApi::openapi_document() {
    const nlohmann::json order_schema{
        {"type", "object"},
        {"required", {"clientOrderId", "venue", "symbol", "side", "type", "quantity"}},
        {"properties", {
             {"clientOrderId", {{"type", "string"}}},
             {"venue", {{"type", "string"}, {"enum", {"OKX", "BINANCE"}}}},
             {"symbol", {{"type", "string"}, {"example", "BTC-USDT"}}},
             {"side", {{"type", "string"}, {"enum", {"BUY", "SELL"}}}},
             {"type", {{"type", "string"}, {"enum", {"MARKET", "LIMIT"}}}},
             {"price", {{"type", "string"}, {"description", "Required for LIMIT"}}},
             {"quantity", {{"type", "string"}}},
             {"timeInForce", {{"type", "string"}, {"enum", {"GTC", "IOC", "FOK"}}}},
         }},
        {"additionalProperties", false},
    };
    return {
        {"openapi", "3.1.0"},
        {"info", {{"title", "ABEX Exchange Gateway"}, {"version", "1.0.0"}}},
        {"servers", {{{"url", "/"}}}},
        {"paths", {
             {"/api/v1/orders", {
                  {"get", {{"summary", "List normalized orders"}}},
                  {"post", {{"summary", "Place an exchange-neutral order"},
                             {"requestBody", {{"required", true},
                                              {"content", {{"application/json", {
                                                   {"schema", order_schema}}}}}}}}},
              }},
             {"/api/v1/orders/{clientOrderId}", {
                  {"get", {{"summary", "Get an order"}}},
                  {"patch", {{"summary", "Amend or replace an order"}}},
                  {"delete", {{"summary", "Cancel an order"}}},
              }},
             {"/api/v1/orders/{clientOrderId}/pipeline", {
                  {"get", {{"summary", "Get the durable sequenced order pipeline"}}},
              }},
             {"/api/v1/health", {{"get", {{"summary", "Get venue health"}}}}},
             {"/api/v1/positions", {{"get", {{"summary", "Get conservative positions"}}}}},
             {"/api/v1/balances", {
                  {"get", {{"summary", "Get the selected order-currency venue balance"},
                           {"parameters", nlohmann::json::array({
                                {{"name", "venue"}, {"in", "query"}, {"required", true},
                                 {"schema", {{"type", "string"},
                                             {"enum", {"OKX", "BINANCE"}}}}},
                                {{"name", "currency"}, {"in", "query"}, {"required", true},
                                 {"schema", {{"type", "string"},
                                             {"enum", {"BTC", "ETH", "USDT"}}}}},
                           })}}},
              }},
             {"/api/v1/instruments", {
                  {"get", {{"summary", "Get authoritative venue rules for an ABEX instrument"}}},
              }},
             {"/api/v1/system", {{"get", {{"summary", "Get OMS stability and audit log"}}}}},
             {"/api/v1/market-data", {{"get", {{"summary", "Get mapped market quotes"}}}}},
             {"/api/v1/reconcile/{venue}", {
                  {"post", {{"summary", "Reconcile non-terminal venue orders"}}},
              }},
         }},
        {"components", {{"schemas", {{"OrderRequest", order_schema}}}}},
    };
}

} // namespace abex
