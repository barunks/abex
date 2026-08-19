#pragma once

#include "abex/application/market_data_book.hpp"
#include "abex/application/order_gateway.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace abex {

class HttpServer final {
public:
    struct Config {
        std::string address{"127.0.0.1"};
        std::uint16_t port{8080};
        std::size_t io_threads{2};
        std::filesystem::path web_root{"web"};
        std::string runtime_mode{"unknown"};
        std::chrono::seconds request_timeout{30};
    };

    explicit HttpServer(OrderGateway& gateway);
    HttpServer(OrderGateway& gateway, Config config);
    HttpServer(OrderGateway& gateway, MarketDataBook& market_data, Config config);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void start();
    void stop() noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;

private:
    class Impl;
    OrderGateway& gateway_;
    MarketDataBook* market_data_{nullptr};
    std::unique_ptr<Impl> impl_;
    OrderGateway::ObserverToken observer_token_{0};
    OrderGateway::ObserverToken operational_observer_token_{0};
    MarketDataBook::ObserverToken market_observer_token_{0};
};

} // namespace abex
