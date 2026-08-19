#include "abex/cli/command_processor.hpp"

#include "abex/domain/string_lookup.hpp"
#include "abex/presentation/json_views.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace abex {
namespace {

class TokenizedLine final {
public:
    explicit TokenizedLine(std::string_view line) {
        storage_.reserve(line.size());
        tokens_.reserve(16);
        bool quoted = false;
        char quote = '\0';
        bool escaped = false;
        std::size_t token_start = 0;
        bool in_token = false;
        const auto finish = [&] {
            if (!in_token) return;
            tokens_.emplace_back(storage_.data() + token_start,
                                 storage_.size() - token_start);
            in_token = false;
        };
        for (const char character : line) {
            if (escaped) {
                if (!in_token) { token_start = storage_.size(); in_token = true; }
                storage_.push_back(character);
                escaped = false;
            } else if (character == '\\') {
                if (!in_token) { token_start = storage_.size(); in_token = true; }
                escaped = true;
            } else if (quoted) {
                if (character == quote) quoted = false;
                else storage_.push_back(character);
            } else if (character == '\'' || character == '"') {
                if (!in_token) { token_start = storage_.size(); in_token = true; }
                quoted = true;
                quote = character;
            } else if (std::isspace(static_cast<unsigned char>(character))) {
                finish();
            } else {
                if (!in_token) { token_start = storage_.size(); in_token = true; }
                storage_.push_back(character);
            }
        }
        if (escaped || quoted) throw std::invalid_argument("unterminated quote or escape");
        finish();
    }

    [[nodiscard]] const std::vector<std::string_view>& tokens() const noexcept { return tokens_; }

private:
    std::string storage_;
    std::vector<std::string_view> tokens_;
};

using Options = std::vector<std::pair<std::string_view, std::string_view>>;

[[nodiscard]] Options parse_options(const std::vector<std::string_view>& tokens) {
    Options options;
    options.reserve(tokens.size() / 2);
    for (std::size_t index = 1; index < tokens.size(); ++index) {
        if (!tokens[index].starts_with("--")) {
            throw std::invalid_argument("expected option, found: " + std::string(tokens[index]));
        }
        const auto key = tokens[index].substr(2);
        if (key.empty()) throw std::invalid_argument("empty option name");
        if (index + 1 >= tokens.size() || tokens[index + 1].starts_with("--")) {
            throw std::invalid_argument("missing value for --" + std::string(key));
        }
        if (std::ranges::find(options, key, &Options::value_type::first) != options.end()) {
            throw std::invalid_argument("duplicate option");
        }
        options.emplace_back(key, tokens[++index]);
    }
    return options;
}

[[nodiscard]] std::string_view required(const Options& options,
                                        std::string_view long_name,
                                        std::string_view short_name = {}) {
    const auto find = [&](std::string_view name) {
        return std::ranges::find(options, name, &Options::value_type::first);
    };
    if (const auto found = find(long_name); found != options.end()) return found->second;
    if (!short_name.empty()) {
        if (const auto found = find(short_name); found != options.end()) return found->second;
    }
    throw std::invalid_argument("missing required option --" + std::string(long_name));
}

[[nodiscard]] std::optional<std::string_view>
optional(const Options& options,
         std::string_view long_name,
         std::string_view short_name = {}) {
    const auto find = [&](std::string_view name) {
        return std::ranges::find(options, name, &Options::value_type::first);
    };
    if (const auto found = find(long_name); found != options.end()) return found->second;
    if (!short_name.empty()) {
        if (const auto found = find(short_name); found != options.end()) return found->second;
    }
    return std::nullopt;
}

[[nodiscard]] std::string operation_json(const OperationResult& result) {
    return operation_view(result).dump(2);
}

[[nodiscard]] CommandResponse json_error(std::string code, std::string message) {
    return {nlohmann::json{{"ok", false},
                           {"code", std::move(code)},
                           {"message", std::move(message)}}
                .dump(2),
            false};
}

struct InstrumentAssets {
    std::string base;
    std::string quote;
};

[[nodiscard]] InstrumentAssets supported_instrument(std::string symbol) {
    std::ranges::transform(symbol, symbol.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    if (symbol == "BTC-USDT") return {"BTC", "USDT"};
    if (symbol == "ETH-USDT") return {"ETH", "USDT"};
    throw std::invalid_argument(
        "unsupported instrument; balance guidance supports BTC-USDT and ETH-USDT");
}

[[nodiscard]] Decimal balance_decimal_floor(std::string text) {
    if (text.find_first_of("eE") != std::string::npos) {
        throw std::invalid_argument("venue balance uses unsupported scientific notation");
    }
    if (const auto dot = text.find('.'); dot != std::string::npos) {
        const auto maximum_size = dot + 1 + static_cast<std::size_t>(Decimal::precision);
        if (text.size() > maximum_size) text.resize(maximum_size);
    }
    return Decimal::parse(text.empty() ? "0" : text);
}

[[nodiscard]] Decimal floor_to_step(Decimal value, const std::optional<Decimal>& step) {
    if (!step || !step->is_positive()) return value;
    return Decimal::from_raw(value.raw() / step->raw() * step->raw());
}

[[nodiscard]] Decimal ceil_to_step(Decimal value, const std::optional<Decimal>& step) {
    if (!step || !step->is_positive()) return value;
    const auto quotient = value.raw() / step->raw();
    const auto remainder = value.raw() % step->raw();
    return Decimal::from_raw((quotient + (remainder == 0 ? 0 : 1)) * step->raw());
}

} // namespace

CommandProcessor::CommandProcessor(
    OrderGateway& gateway,
    std::unordered_map<Venue, std::shared_ptr<SimulatedExchangeAdapter>> simulated)
    : gateway_(gateway), simulated_(std::move(simulated)) {}

CommandResponse CommandProcessor::execute(std::string_view line) {
    try {
        const TokenizedLine tokenized(line);
        const auto& tokens = tokenized.tokens();
        if (tokens.empty()) return {};
        const auto& command = tokens.front();
        if (command == "exit" || command == "quit") return {.exit_requested = true};
        if (command == "help") return {help_text(), false};

        const auto options = parse_options(tokens);
        if (command == "place") {
            OrderRequest request{
                .client_order_id = std::string(required(options, "client-order-id", "id")),
                .venue = venue_from_string(required(options, "venue")),
                .symbol = std::string(required(options, "symbol")),
                .side = side_from_string(required(options, "side")),
                .type = order_type_from_string(optional(options, "type").value_or("LIMIT")),
                .quantity = Decimal::parse(required(options, "quantity", "qty")),
                .time_in_force = time_in_force_from_string(
                    optional(options, "time-in-force", "tif").value_or("GTC")),
            };
            if (const auto price = optional(options, "price")) request.price = Decimal::parse(*price);
            return {operation_json(gateway_.place(request)), false};
        }
        if (command == "cancel") {
            CancelRequest request{
                .client_order_id = std::string(required(options, "client-order-id", "id")),
                .request_id = std::string(optional(options, "request-id").value_or("")),
            };
            auto result = gateway_.cancel(std::move(request));
            gateway_.flush_events();
            if (result.order) result.order = gateway_.get(result.order->client_order_id);
            return {operation_json(result), false};
        }
        if (command == "amend") {
            AmendRequest request{
                .client_order_id = std::string(required(options, "client-order-id", "id")),
                .request_id = std::string(optional(options, "request-id").value_or("")),
            };
            if (const auto price = optional(options, "new-price", "price")) {
                request.new_price = Decimal::parse(*price);
            }
            if (const auto quantity = optional(options, "new-quantity", "qty")) {
                request.new_quantity = Decimal::parse(*quantity);
            }
            auto result = gateway_.amend(std::move(request));
            gateway_.flush_events();
            if (result.order) result.order = gateway_.get(result.order->client_order_id);
            return {operation_json(result), false};
        }
        if (command == "get") {
            const auto id = required(options, "client-order-id", "id");
            const auto order = gateway_.get_snapshot(id);
            if (!order) return json_error("ORDER_NOT_FOUND", "order does not exist");
            return {nlohmann::json{{"ok", true}, {"order", order_view(*order)}}.dump(2), false};
        }
        if (command == "list") {
            const auto venue_text = optional(options, "venue");
            const auto status_text = optional(options, "status");
            const auto orders = gateway_.list_snapshots(
                venue_text ? std::optional(venue_from_string(*venue_text)) : std::nullopt,
                status_text ? std::optional(order_status_from_string(*status_text)) : std::nullopt);
            auto items = nlohmann::json::array();
            for (const auto& order : orders) items.push_back(order_view(order));
            return {nlohmann::json{{"ok", true}, {"orders", std::move(items)}}.dump(2), false};
        }
        if (command == "positions") {
            auto values = positions_view(gateway_.positions());
            return {nlohmann::json{{"ok", true}, {"positions", std::move(values)}}.dump(2), false};
        }
        if (command == "balance" || command == "balances") {
            const auto venue = venue_from_string(required(options, "venue"));
            if (optional(options, "currency", "ccy")) {
                throw std::invalid_argument(
                    "select --symbol and --side; direct all-currency balance queries are disabled");
            }
            auto symbol = std::string(required(options, "symbol"));
            std::ranges::transform(symbol, symbol.begin(), [](unsigned char character) {
                return static_cast<char>(std::toupper(character));
            });
            const auto assets = supported_instrument(symbol);
            const auto side = side_from_string(required(options, "side"));
            const auto type = order_type_from_string(optional(options, "type").value_or("MARKET"));
            const auto currency = side == Side::Buy ? assets.quote : assets.base;
            const auto result = gateway_.balances(venue, currency);
            const auto rules_result = gateway_.instrument_rules(venue, symbol);
            auto view = balance_view(result);
            view["symbol"] = symbol;
            view["side"] = to_string(side);
            view["type"] = to_string(type);
            view["fundingCurrency"] = currency;
            view["instrumentRules"] = instrument_rules_view(rules_result);

            if (result.ok && rules_result.ok) {
                const auto found = std::ranges::find_if(
                    result.snapshot.balances, [&](const AccountBalance& balance) {
                        return balance.currency == currency;
                    });
                const auto available = found == result.snapshot.balances.end()
                                           ? Decimal{}
                                           : balance_decimal_floor(found->available);
                nlohmann::json guidance{
                    {"available", available.to_string()},
                    {"reservePercent", "0.5"},
                };
                const auto usable = available * Decimal::parse("0.995");
                const auto price_text = optional(options, "price");
                std::optional<Decimal> price;
                if (price_text) {
                    price = Decimal::parse(*price_text);
                    if (!price->is_positive()) {
                        throw std::invalid_argument("--price must be positive");
                    }
                    guidance["referencePrice"] = price->to_string();
                }

                if (side == Side::Buy && !price) {
                    guidance["message"] =
                        "add --price PRICE to calculate the routeable buy quantity range";
                } else {
                    const auto& rules = rules_result.rules;
                    auto maximum = side == Side::Sell ? usable : usable / *price;
                    if (rules.maximum_quantity && maximum > *rules.maximum_quantity) {
                        maximum = *rules.maximum_quantity;
                    }
                    if (type == OrderType::Market && rules.market_maximum_quantity &&
                        maximum > *rules.market_maximum_quantity) {
                        maximum = *rules.market_maximum_quantity;
                    }
                    const auto& maximum_notional = type == OrderType::Market
                                                       ? rules.market_maximum_notional
                                                       : rules.maximum_notional;
                    if (price && maximum_notional) {
                        const auto notional_quantity = *maximum_notional / *price;
                        if (maximum > notional_quantity) maximum = notional_quantity;
                    }
                    maximum = floor_to_step(maximum, rules.quantity_step);
                    if (type == OrderType::Market) {
                        maximum = floor_to_step(maximum, rules.market_quantity_step);
                    }

                    auto minimum = rules.minimum_quantity.value_or(Decimal{});
                    if (type == OrderType::Market && rules.market_minimum_quantity &&
                        minimum < *rules.market_minimum_quantity) {
                        minimum = *rules.market_minimum_quantity;
                    }
                    const auto& minimum_notional = type == OrderType::Market
                                                       ? rules.market_minimum_notional
                                                       : rules.minimum_notional;
                    if (price && minimum_notional) {
                        auto notional_quantity = *minimum_notional / *price;
                        if (notional_quantity * *price < *minimum_notional) {
                            notional_quantity += Decimal::from_raw(1);
                        }
                        if (minimum < notional_quantity) minimum = notional_quantity;
                    }
                    minimum = ceil_to_step(minimum, rules.quantity_step);
                    if (type == OrderType::Market) {
                        minimum = ceil_to_step(minimum, rules.market_quantity_step);
                    }

                    guidance["minimumRouteableQuantity"] = minimum.to_string();
                    guidance["suggestedMaxQuantity"] = maximum.to_string();
                    guidance["routeable"] = maximum >= minimum && maximum.is_positive();
                    guidance["message"] = maximum >= minimum && maximum.is_positive()
                        ? "range satisfies current venue rules and keeps a 0.5% balance reserve"
                        : "available balance cannot meet the venue minimum order";
                }
                view["quantityGuidance"] = std::move(guidance);
            }
            return {view.dump(2), false};
        }
        if (command == "rules" || command == "instrument") {
            const auto venue = venue_from_string(required(options, "venue"));
            auto symbol = std::string(required(options, "symbol"));
            std::ranges::transform(symbol, symbol.begin(), [](unsigned char character) {
                return static_cast<char>(std::toupper(character));
            });
            (void)supported_instrument(symbol);
            return {instrument_rules_view(
                        gateway_.instrument_rules(venue, std::move(symbol))).dump(2),
                    false};
        }
        if (command == "health") {
            auto venues = health_view(gateway_.health());
            return {nlohmann::json{{"ok", true}, {"venues", std::move(venues)}}.dump(2), false};
        }
        if (command == "reconcile") {
            return {operation_json(gateway_.reconcile(
                        venue_from_string(required(options, "venue")))),
                    false};
        }
        if (command == "simulate") {
            const auto venue = venue_from_string(required(options, "venue"));
            const auto adapter = simulated_.find(venue);
            if (adapter == simulated_.end()) {
                return json_error("NOT_SIMULATED", "venue is using a live adapter");
            }
            const auto id = required(options, "client-order-id", "id");
            const auto status = order_status_from_string(required(options, "status"));
            const auto filled = Decimal::parse(required(options, "filled"));
            const auto last_price = optional(options, "last-price", "price");
            const auto event_id = optional(options, "event-id").value_or("");
            const auto sequence_text = optional(options, "sequence", "seq");
            const bool emitted = adapter->second->emit(
                id, status, filled,
                last_price ? std::optional(Decimal::parse(*last_price)) : std::nullopt,
                std::string(event_id),
                sequence_text ? std::optional(std::stoull(std::string(*sequence_text)))
                              : std::nullopt);
            if (!emitted) return json_error("ORDER_NOT_FOUND", "simulated order does not exist");
            gateway_.flush_events();
            return {nlohmann::json{{"ok", true},
                                   {"order", order_view(*gateway_.get_snapshot(id))}}
                        .dump(2),
                    false};
        }
        if (command == "disconnect" || command == "reconnect") {
            const auto venue = venue_from_string(required(options, "venue"));
            const auto adapter = simulated_.find(venue);
            if (adapter == simulated_.end()) {
                return json_error("NOT_SIMULATED", "venue is using a live adapter");
            }
            if (command == "disconnect") adapter->second->disconnect();
            else adapter->second->reconnect();
            return {nlohmann::json{{"ok", true},
                                   {"venue", to_string(venue)},
                                   {"connected", adapter->second->connected()}}
                        .dump(2),
                    false};
        }
        return json_error("UNKNOWN_COMMAND", "unknown command: " + std::string(command));
    } catch (const std::exception& error) {
        return json_error("INVALID_COMMAND", error.what());
    }
}

std::string CommandProcessor::help_text() {
    return R"(ABEX exchange gateway CLI

Commands:
  place --id ID --venue OKX|BINANCE --symbol BTC-USDT --side BUY|SELL
        --type LIMIT|MARKET [--price PRICE] --qty QUANTITY [--tif GTC|IOC|FOK]
  cancel --id ID [--request-id ID]
  amend --id ID [--request-id ID] [--new-price PRICE] [--new-quantity QUANTITY]
  get --id ID
  list [--venue VENUE] [--status STATUS]
  positions
  balances --venue OKX|BINANCE --symbol BTC-USDT|ETH-USDT --side BUY|SELL
           [--type LIMIT|MARKET] [--price PRICE]
  rules --venue OKX|BINANCE --symbol BTC-USDT|ETH-USDT
  health
  reconcile --venue VENUE

Simulation-only operations:
  simulate --venue VENUE --id ID --status STATUS --filled QUANTITY
           [--last-price PRICE] [--event-id ID] [--sequence N]
  disconnect --venue VENUE
  reconnect --venue VENUE

  help
  exit
)";
}

} // namespace abex
