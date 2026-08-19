#pragma once

#include "abex/domain/market_data.hpp"
#include "abex/infrastructure/http_client.hpp"

#include <string>
#include <vector>

namespace abex {

class PublicMarketDataClient final {
public:
    struct Config {
        std::string okx_rest_url{"https://www.okx.com"};
        std::string binance_rest_url{"https://data-api.binance.vision"};
    };

    PublicMarketDataClient();
    explicit PublicMarketDataClient(Config config);

    [[nodiscard]] std::vector<MarketQuote> fetch_okx() const;
    [[nodiscard]] std::vector<MarketQuote> fetch_binance() const;

private:
    [[nodiscard]] static std::vector<MarketQuote> parse_okx(std::string_view body);
    [[nodiscard]] static std::vector<MarketQuote> parse_binance(std::string_view body);

    Config config_;
    HttpClient http_;
};

} // namespace abex
