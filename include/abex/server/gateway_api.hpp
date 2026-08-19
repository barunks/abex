#pragma once

#include "abex/application/market_data_book.hpp"
#include "abex/application/order_gateway.hpp"

#include <string>
#include <unordered_map>
#include <utility>

#include <nlohmann/json_fwd.hpp>

namespace abex {

struct ApiRequest {
    std::string method;
    std::string target;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
};

struct ApiResponse {
    unsigned status{200};
    std::string content_type{"application/json"};
    std::string body;
    std::unordered_map<std::string, std::string> headers;
};

class GatewayApi final {
public:
    explicit GatewayApi(OrderGateway& gateway,
                        MarketDataBook* market_data = nullptr,
                        std::string runtime_mode = "unknown")
        : gateway_(gateway), market_data_(market_data), runtime_mode_(std::move(runtime_mode)) {}

    [[nodiscard]] ApiResponse handle(const ApiRequest& request);
    [[nodiscard]] static nlohmann::json openapi_document();

private:
    OrderGateway& gateway_;
    MarketDataBook* market_data_{nullptr};
    std::string runtime_mode_;
};

} // namespace abex
