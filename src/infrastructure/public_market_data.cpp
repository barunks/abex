#include "abex/infrastructure/public_market_data.hpp"

#include <array>
#include <stdexcept>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace abex {
namespace {

const std::array<std::string_view, 2> canonical_symbols{"BTC-USDT", "ETH-USDT"};

[[nodiscard]] bool supported_symbol(std::string_view symbol) {
    return std::ranges::find(canonical_symbols, symbol) != canonical_symbols.end();
}

[[nodiscard]] std::string binance_symbol(std::string_view symbol) {
    if (symbol == "BTCUSDT") return "BTC-USDT";
    if (symbol == "ETHUSDT") return "ETH-USDT";
    return {};
}

[[nodiscard]] std::int64_t parse_time(const nlohmann::json& value,
                                      std::string_view field,
                                      std::int64_t fallback) {
    if (!value.contains(field)) return fallback;
    if (value.at(field).is_number_integer()) return value.at(field).get<std::int64_t>();
    if (value.at(field).is_string()) return std::stoll(value.at(field).get<std::string>());
    return fallback;
}

void require_success(const HttpResponse& response, std::string_view venue) {
    if (response.status != 200) {
        throw std::runtime_error(std::string(venue) + " market-data HTTP " +
                                 std::to_string(response.status));
    }
}

} // namespace

PublicMarketDataClient::PublicMarketDataClient() : PublicMarketDataClient(Config{}) {}

PublicMarketDataClient::PublicMarketDataClient(Config config) : config_(std::move(config)) {}

std::vector<MarketQuote> PublicMarketDataClient::fetch_okx() const {
    const auto response = http_.perform({
        .url = config_.okx_rest_url + "/api/v5/market/tickers?instType=SPOT",
    });
    require_success(response, "OKX");
    return parse_okx(response.body);
}

std::vector<MarketQuote> PublicMarketDataClient::fetch_binance() const {
    const auto response = http_.perform({
        .url = config_.binance_rest_url +
               "/api/v3/ticker/bookTicker?symbols=%5B%22BTCUSDT%22,%22ETHUSDT%22%5D",
    });
    require_success(response, "Binance");
    return parse_binance(response.body);
}

std::vector<MarketQuote> PublicMarketDataClient::parse_okx(std::string_view body) {
    const auto response = nlohmann::json::parse(body);
    if (response.value("code", std::string{}) != "0" || !response.contains("data")) {
        throw std::runtime_error("OKX market-data response was rejected");
    }
    const auto now = unix_time_ms();
    std::vector<MarketQuote> result;
    for (const auto& item : response.at("data")) {
        const auto symbol = item.value("instId", std::string{});
        if (!supported_symbol(symbol)) continue;
        result.push_back({
            .venue = Venue::Okx,
            .symbol = symbol,
            .bid_price = Decimal::parse(item.at("bidPx").get<std::string>()),
            .ask_price = Decimal::parse(item.at("askPx").get<std::string>()),
            .source_time_ms = parse_time(item, "ts", now),
            .published_at_ms = now,
        });
    }
    if (result.size() != canonical_symbols.size()) {
        throw std::runtime_error("OKX response did not contain both configured symbols");
    }
    return result;
}

std::vector<MarketQuote> PublicMarketDataClient::parse_binance(std::string_view body) {
    const auto response = nlohmann::json::parse(body);
    if (!response.is_array()) throw std::runtime_error("Binance market-data response is not an array");
    const auto now = unix_time_ms();
    std::vector<MarketQuote> result;
    for (const auto& item : response) {
        const auto symbol = binance_symbol(item.value("symbol", std::string{}));
        if (symbol.empty()) continue;
        result.push_back({
            .venue = Venue::Binance,
            .symbol = symbol,
            .bid_price = Decimal::parse(item.at("bidPrice").get<std::string>()),
            .ask_price = Decimal::parse(item.at("askPrice").get<std::string>()),
            .source_time_ms = now,
            .published_at_ms = now,
        });
    }
    if (result.size() != canonical_symbols.size()) {
        throw std::runtime_error("Binance response did not contain both configured symbols");
    }
    return result;
}

} // namespace abex
