#pragma once

#include "abex/application/order_gateway.hpp"
#include "abex/infrastructure/file_order_store.hpp"
#include "abex/infrastructure/simulated_exchange_adapter.hpp"

#include <memory>
#include <string>
#include <unordered_map>

namespace abex::test {

inline RiskManager risk_manager() {
    return RiskManager({
        {"BTC-USDT",
         {.max_order_size = Decimal::parse("2"),
          .max_notional = Decimal::parse("200000"),
          .position_limit = Decimal::parse("4"),
          .market_reference_price = Decimal::parse("60000")}},
        {"ETH-USDT",
         {.max_order_size = Decimal::parse("20"),
          .max_notional = Decimal::parse("100000"),
          .position_limit = Decimal::parse("40"),
          .market_reference_price = Decimal::parse("3000")}},
    });
}

inline OrderRequest limit_order(std::string id,
                                Venue venue = Venue::Okx,
                                Side side = Side::Buy,
                                std::string quantity = "0.1",
                                std::string price = "50000") {
    return {
        .client_order_id = std::move(id),
        .venue = venue,
        .symbol = "BTC-USDT",
        .side = side,
        .type = OrderType::Limit,
        .price = Decimal::parse(price),
        .quantity = Decimal::parse(quantity),
        .time_in_force = TimeInForce::Gtc,
    };
}

struct GatewayFixture {
    std::shared_ptr<SimulatedExchangeAdapter> okx;
    std::shared_ptr<SimulatedExchangeAdapter> binance;
    std::shared_ptr<IOrderStore> store;
    std::unique_ptr<OrderGateway> gateway;

    explicit GatewayFixture(
        std::shared_ptr<IOrderStore> selected_store = std::make_shared<MemoryOrderStore>(),
        SimulatedExchangeAdapter::Config okx_config = {},
        bool reconcile_on_start = false)
        : okx(std::make_shared<SimulatedExchangeAdapter>(Venue::Okx, okx_config)),
          binance(std::make_shared<SimulatedExchangeAdapter>(Venue::Binance)),
          store(std::move(selected_store)),
          gateway(std::make_unique<OrderGateway>(
              std::vector<std::shared_ptr<IExchangeAdapter>>{okx, binance}, risk_manager(), store,
              OrderGateway::Options{.event_queue_capacity = 64,
                                    .reconcile_on_start = reconcile_on_start})) {
        gateway->start();
    }

    ~GatewayFixture() { gateway->stop(); }
};

} // namespace abex::test
