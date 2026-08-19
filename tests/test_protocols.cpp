#include "abex/infrastructure/exchange_protocols.hpp"
#include "abex/infrastructure/crypto.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace abex;

TEST_CASE("OKX protocol translates the common schema", "[protocol]") {
    auto order = make_order(test::limit_order("okx-protocol", Venue::Okx, Side::Buy, "0.25",
                                               "30000"));
    const auto request = OkxProtocol::place_request(order);
    CHECK(request.at("instId") == "BTC-USDT");
    CHECK(request.at("tdMode") == "cash");
    CHECK(request.at("side") == "buy");
    CHECK(request.at("ordType") == "limit");
    CHECK(request.at("px") == "30000");
    CHECK(request.at("sz") == "0.25");
    const auto exchange_client_id = request.at("clOrdId").get<std::string>();
    CHECK(exchange_client_id == OkxProtocol::client_id_to_exchange("okx-protocol"));
    CHECK(exchange_client_id.size() <= 32);
    CHECK(std::ranges::all_of(exchange_client_id, [](unsigned char character) {
        return std::isalnum(character) != 0;
    }));
}

TEST_CASE("OKX client identifiers are deterministic and venue-safe", "[protocol]") {
    CHECK(OkxProtocol::client_id_to_exchange("AlreadyValid123") == "AlreadyValid123");

    const auto ui_id = OkxProtocol::client_id_to_exchange("ui-msyxrh83-1f3kvgqo2gu30");
    CHECK(ui_id == OkxProtocol::client_id_to_exchange("ui-msyxrh83-1f3kvgqo2gu30"));
    CHECK(ui_id != OkxProtocol::client_id_to_exchange("ui-msyxrh83-1f3kvgqo2gu31"));
    CHECK(ui_id.size() <= 32);
    CHECK(std::ranges::all_of(ui_id, [](unsigned char character) {
        return std::isalnum(character) != 0;
    }));
}

TEST_CASE("OKX market orders express quantity in the common base asset", "[protocol]") {
    auto request = test::limit_order("okx-market", Venue::Okx, Side::Buy, "0.25");
    request.type = OrderType::Market;
    request.price.reset();
    const auto wire = OkxProtocol::place_request(make_order(request));
    CHECK(wire.at("ordType") == "market");
    CHECK(wire.at("sz") == "0.25");
    CHECK(wire.at("tgtCcy") == "base_ccy");
    CHECK(wire.at("banAmend") == true);
    CHECK_FALSE(wire.contains("px"));
}

TEST_CASE("OKX amend request serializes only authoritative common fields", "[protocol]") {
    auto order = make_order(test::limit_order("okx-amend", Venue::Okx));
    order.exchange_order_id = "123456";
    order.version = 7;
    const auto wire = OkxProtocol::amend_request(
        order, Decimal::parse("49000"), Decimal::parse("0.08"));
    CHECK(wire.at("instId") == "BTC-USDT");
    CHECK(wire.at("ordId") == "123456");
    CHECK(wire.at("cxlOnFail") == false);
    CHECK(wire.at("newPx") == "49000");
    CHECK(wire.at("newSz") == "0.08");
    CHECK(wire.at("reqId").get<std::string>().size() <= 32);
    CHECK_FALSE(wire.contains("clOrdId"));
}

TEST_CASE("OKX order updates normalize fills", "[protocol]") {
    const nlohmann::json update{
        {"ordId", "12345"},
        {"clOrdId", "client-1"},
        {"state", "partially_filled"},
        {"accFillSz", "0.04"},
        {"avgPx", "29950"},
        {"fillPx", "30000"},
        {"px", "30100"},
        {"sz", "0.25"},
        {"tradeId", "77"},
        {"uTime", "1700000000000"},
    };
    const auto report = OkxProtocol::parse_order_update(update);
    CHECK(report.status == OrderStatus::PartiallyFilled);
    CHECK(report.cumulative_filled == Decimal::parse("0.04"));
    CHECK(report.cumulative_quote == Decimal::parse("1198"));
    CHECK(report.event_id == "okx-trade-77");
    CHECK(report.order_price == Decimal::parse("30100"));
    CHECK(report.order_quantity == Decimal::parse("0.25"));
}

TEST_CASE("OKX acknowledgement exposes the per-order rejection", "[protocol]") {
    const nlohmann::json response{
        {"code", "1"},
        {"msg", "All operations failed"},
        {"data", nlohmann::json::array({{
             {"clOrdId", "rejected-order"},
             {"ordId", ""},
             {"sCode", "51008"},
             {"sMsg", "Order failed. Insufficient balance."},
             {"subCode", "51008-A"},
         }})},
    };

    const auto result = OkxProtocol::parse_ack(response);
    CHECK_FALSE(result.accepted);
    CHECK(result.code == "51008");
    CHECK(result.message == "Order failed. Insufficient balance. (subCode 51008-A)");
}

TEST_CASE("OKX balances preserve account identity and venue precision", "[protocol][balance]") {
    const nlohmann::json config{{"code", "0"}, {"data", nlohmann::json::array({{
        {"uid", "account-7"}, {"mainUid", "main-1"}
    }})}};
    const nlohmann::json balance{{"code", "0"}, {"data", nlohmann::json::array({{
        {"uTime", "1700000000000"},
        {"details", nlohmann::json::array({{
             {"ccy", "USDT"}, {"eq", "5.000370592936"},
             {"availBal", "0.0003705929360018"},
             {"frozenBal", "4.999999999999998"}, {"ordFrozen", "4.999999999999998"}
         }})}
    }})}};

    const auto result = OkxProtocol::parse_balances(config, balance);
    REQUIRE(result.ok);
    CHECK(result.snapshot.account_id == "account-7");
    CHECK(result.snapshot.main_account_id == "main-1");
    REQUIRE(result.snapshot.balances.size() == 1);
    CHECK(result.snapshot.balances.front().available == "0.0003705929360018");
    CHECK(result.snapshot.balances.front().order_frozen == "4.999999999999998");
}

TEST_CASE("OKX instrument metadata normalizes route constraints", "[protocol][rules]") {
    const nlohmann::json response{{"code", "0"}, {"data", nlohmann::json::array({{
        {"instId", "ETH-USDT"}, {"state", "live"},
        {"minSz", "0.0001"}, {"lotSz", "0.000001"}, {"tickSz", "0.01"},
        {"maxLmtSz", "999999999999"}, {"maxMktSz", "1000000"},
        {"maxLmtAmt", "20000000"}, {"maxMktAmt", "1000000"}
    }})}};

    const auto result = OkxProtocol::parse_instrument_rules(response);
    REQUIRE(result.ok);
    CHECK(result.rules.symbol == "ETH-USDT");
    CHECK(result.rules.trading);
    CHECK(result.rules.minimum_quantity == Decimal::parse("0.0001"));
    CHECK(result.rules.quantity_step == Decimal::parse("0.000001"));
    CHECK(result.rules.price_tick == Decimal::parse("0.01"));
    CHECK_FALSE(result.rules.maximum_quantity); // effectively unbounded in ABEX's domain
    CHECK(result.rules.market_maximum_notional == Decimal::parse("1000000"));
}

TEST_CASE("Binance protocol translates symbols and execution reports", "[protocol]") {
    auto order = make_order(test::limit_order("binance-protocol", Venue::Binance));
    const auto request = BinanceProtocol::place_params(order);
    CHECK(request.at("symbol") == "BTCUSDT");
    CHECK(request.at("newClientOrderId") == "binance-protocol");

    const nlohmann::json message{
        {"subscriptionId", 0},
        {"event", {
             {"e", "executionReport"},
             {"E", 1700000000001LL},
             {"i", 991},
             {"I", 55},
             {"c", "binance-protocol"},
             {"X", "FILLED"},
             {"x", "TRADE"},
             {"z", "0.1"},
             {"Z", "5000"},
             {"L", "50000"},
             {"r", "NONE"},
         }},
    };
    const auto report = BinanceProtocol::parse_execution_report(message);
    CHECK(report.exchange_order_id == "991");
    CHECK(report.status == OrderStatus::Filled);
    CHECK(report.cumulative_quote == Decimal::parse("5000"));
    CHECK(report.event_id == "binance-execution-55");
}

TEST_CASE("Binance account status normalizes free and locked balances", "[protocol][balance]") {
    const nlohmann::json response{
        {"status", 200},
        {"result", {
             {"updateTime", 1700000000001LL},
             {"balances", nlohmann::json::array({{
                  {"asset", "USDT"}, {"free", "12.5"}, {"locked", "2.25"}
              }})},
         }},
    };
    const auto result = BinanceProtocol::parse_balances(response);
    REQUIRE(result.ok);
    REQUIRE(result.snapshot.balances.size() == 1);
    CHECK(result.snapshot.balances.front().total == "14.75");
    CHECK(result.snapshot.balances.front().available == "12.5");
    CHECK(result.snapshot.balances.front().frozen == "2.25");
}

TEST_CASE("Binance exchangeInfo normalizes spot filters", "[protocol][rules]") {
    const nlohmann::json response{
        {"status", 200},
        {"result", {{"symbols", nlohmann::json::array({{
            {"symbol", "ETHUSDT"}, {"baseAsset", "ETH"}, {"quoteAsset", "USDT"},
            {"status", "TRADING"},
            {"filters", nlohmann::json::array({
                {{"filterType", "PRICE_FILTER"}, {"minPrice", "0.01"},
                 {"maxPrice", "1000000"}, {"tickSize", "0.01"}},
                {{"filterType", "LOT_SIZE"}, {"minQty", "0.0001"},
                 {"maxQty", "9000"}, {"stepSize", "0.0001"}},
                {{"filterType", "MARKET_LOT_SIZE"}, {"minQty", "0"},
                 {"maxQty", "3539.96789708"}, {"stepSize", "0"}},
                {{"filterType", "NOTIONAL"}, {"minNotional", "5"},
                 {"applyMinToMarket", true}, {"maxNotional", "9000000"},
                 {"applyMaxToMarket", false}}
            })}
        }})}}},
    };

    const auto result = BinanceProtocol::parse_instrument_rules(response);
    REQUIRE(result.ok);
    CHECK(result.rules.symbol == "ETH-USDT");
    CHECK(result.rules.trading);
    CHECK(result.rules.minimum_quantity == Decimal::parse("0.0001"));
    CHECK(result.rules.quantity_step == Decimal::parse("0.0001"));
    CHECK(result.rules.market_maximum_quantity == Decimal::parse("3539.96789708"));
    CHECK(result.rules.minimum_notional == Decimal::parse("5"));
    CHECK(result.rules.market_minimum_notional == Decimal::parse("5"));
    CHECK_FALSE(result.rules.market_maximum_notional);
}

TEST_CASE("Binance cancel-replace acknowledgement selects the new order", "[protocol]") {
    const nlohmann::json response{
        {"status", 200},
        {"result", {
             {"cancelResult", "SUCCESS"},
             {"newOrderResult", "SUCCESS"},
             {"newOrderResponse", {
                  {"orderId", 222},
                  {"clientOrderId", "replacement-2"},
              }},
         }},
    };
    const auto result = BinanceProtocol::parse_ack(response);
    CHECK(result.accepted);
    CHECK(result.replacement);
    CHECK(result.original_order_canceled);
    CHECK(result.exchange_order_id == "222");
    CHECK(result.exchange_client_order_id == "replacement-2");
}

TEST_CASE("Binance cancel-replace exposes partial failure details", "[protocol]") {
    const nlohmann::json response{
        {"status", 409},
        {"result", {
             {"cancelResult", "SUCCESS"},
             {"newOrderResult", "FAILURE"},
             {"cancelResponse", {
                  {"orderId", 111}, {"clientOrderId", "original"},
                  {"status", "CANCELED"}, {"executedQty", "0.03"},
                  {"cummulativeQuoteQty", "1500"}, {"origQty", "0.1"},
                  {"price", "50000"}, {"transactTime", 1700000000002LL},
              }},
             {"newOrderResponse", {
                  {"code", -1013}, {"msg", "Filter failure: LOT_SIZE"},
              }},
         }},
    };
    const auto result = BinanceProtocol::parse_ack(response);
    CHECK_FALSE(result.accepted);
    CHECK(result.original_order_canceled);
    CHECK(result.code == "-1013");
    CHECK(result.message.find("original order canceled") != std::string::npos);
    REQUIRE(result.authoritative_reports.size() == 1);
    CHECK(result.authoritative_reports.front().status == OrderStatus::Canceled);
    CHECK(result.authoritative_reports.front().cumulative_filled == Decimal::parse("0.03"));
}

TEST_CASE("Binance numeric exchange ids stay JSON integers", "[protocol]") {
    auto order = make_order(test::limit_order("typed-id", Venue::Binance));
    order.exchange_order_id = "3844403397573132288";
    const auto cancel = BinanceProtocol::cancel_params(order);
    CHECK(cancel.at("orderId").is_number_unsigned());
    const auto replacement = BinanceProtocol::amend_params(
        order, Decimal::parse("49000"), Decimal::parse("0.07"), "replacement");
    CHECK(replacement.at("cancelOrderId").is_number_unsigned());
    CHECK(replacement.at("quantity") == "0.07");
    CHECK(replacement.at("newOrderRespType") == "FULL");
}

TEST_CASE("cryptographic helpers match a standard HMAC vector", "[crypto]") {
    CHECK(hmac_sha256_hex("key", "The quick brown fox jumps over the lazy dog") ==
          "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
    const nlohmann::json params{{"timestamp", 42}, {"symbol", "BTCUSDT"}, {"side", "BUY"}};
    CHECK(canonical_query(params) == "side=BUY&symbol=BTCUSDT&timestamp=42");
}
