#include "abex/server/gateway_api.hpp"

#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace abex;

namespace {

[[nodiscard]] nlohmann::json order_body(std::string id, std::string venue) {
    return {
        {"clientOrderId", std::move(id)},
        {"venue", std::move(venue)},
        {"symbol", "BTC-USDT"},
        {"side", "BUY"},
        {"type", "LIMIT"},
        {"price", "50000"},
        {"quantity", "0.1"},
        {"timeInForce", "GTC"},
    };
}

[[nodiscard]] ApiResponse call(GatewayApi& api,
                               std::string method,
                               std::string target,
                               nlohmann::json body = nlohmann::json{},
                               StringMap<std::string> headers = {}) {
    return api.handle({
        .method = std::move(method),
        .target = std::move(target),
        .body = body.is_null() ? std::string{} : body.dump(),
        .headers = std::move(headers),
    });
}

} // namespace

TEST_CASE("POST /api/v1/orders with Prefer: respond-async returns 202", "[api][async]") {
    test::GatewayFixture fixture;
    GatewayApi api(*fixture.gateway);

    // Without header: 201
    const auto sync = call(api, "POST", "/api/v1/orders", order_body("async-order-1", "OKX"));
    CHECK(sync.status == 201);

    // With Prefer: respond-async: 202
    const auto async = call(api, "POST", "/api/v1/orders",
                            order_body("async-order-2", "OKX"),
                            {{"prefer", "respond-async"}});
    CHECK(async.status == 202);
    const auto body = nlohmann::json::parse(async.body);
    CHECK(body.at("ok") == true);
    CHECK(body.at("order").at("clientOrderId") == "async-order-2");

    // Idempotent replay is always 200 regardless of Prefer
    const auto replay = call(api, "POST", "/api/v1/orders",
                             order_body("async-order-2", "OKX"),
                             {{"prefer", "respond-async"}});
    CHECK(replay.status == 200);
    CHECK(nlohmann::json::parse(replay.body).at("idempotentReplay") == true);

    // OpenAPI document lists the Prefer parameter
    const auto schema = nlohmann::json::parse(
        call(api, "GET", "/api/v1/openapi.json").body);
    const auto& post_params = schema.at("paths").at("/api/v1/orders").at("post").at("parameters");
    CHECK(std::ranges::any_of(post_params, [](const auto& p) {
        return p.at("name") == "Prefer";
    }));
}

TEST_CASE("REST API accepts one common schema for both venues", "[api][integration]") {
    test::GatewayFixture fixture;
    GatewayApi api(*fixture.gateway);

    const auto okx = call(api, "POST", "/api/v1/orders", order_body("rest-okx", "OKX"));
    REQUIRE(okx.status == 201);
    const auto okx_json = nlohmann::json::parse(okx.body);
    CHECK(okx_json.at("ok") == true);
    CHECK(okx_json.at("order").at("venue") == "OKX");
    CHECK(okx_json.at("order").at("exchangeOrderId").get<std::string>().starts_with("OKX-SIM-"));

    const auto binance =
        call(api, "POST", "/api/v1/orders", order_body("rest-binance", "BINANCE"));
    REQUIRE(binance.status == 201);
    const auto binance_json = nlohmann::json::parse(binance.body);
    CHECK(binance_json.at("order").at("venue") == "BINANCE");
    CHECK(binance_json.at("order").at("exchangeOrderId")
              .get<std::string>()
              .starts_with("BINANCE-SIM-"));

    const auto listed = call(api, "GET", "/api/v1/orders?venue=OKX");
    REQUIRE(listed.status == 200);
    CHECK(nlohmann::json::parse(listed.body).at("orders").size() == 1);

    const auto replay = call(api, "POST", "/api/v1/orders", order_body("rest-okx", "OKX"));
    CHECK(replay.status == 200);
    CHECK(nlohmann::json::parse(replay.body).at("idempotentReplay") == true);
}

TEST_CASE("REST API requires a supported order currency for venue balances",
          "[api][balance]") {
    test::GatewayFixture fixture;
    GatewayApi api(*fixture.gateway);

    const auto response = call(api, "GET", "/api/v1/balances?venue=OKX&currency=USDT");
    REQUIRE(response.status == 200);
    const auto body = nlohmann::json::parse(response.body);
    CHECK(body.at("venue") == "OKX");
    CHECK(body.at("accountId") == "SIM-OKX");
    REQUIRE(body.at("balances").size() == 1);
    CHECK(body.at("balances").front().at("currency") == "USDT");
    CHECK(body.at("balances").front().at("available") == "1000000");

    const auto missing_venue = call(api, "GET", "/api/v1/balances");
    CHECK(missing_venue.status == 400);
    CHECK(nlohmann::json::parse(missing_venue.body).at("code") == "VENUE_REQUIRED");

    const auto missing_currency = call(api, "GET", "/api/v1/balances?venue=OKX");
    CHECK(missing_currency.status == 400);
    CHECK(nlohmann::json::parse(missing_currency.body).at("code") == "CURRENCY_REQUIRED");

    const auto unsupported = call(api, "GET", "/api/v1/balances?venue=OKX&currency=XRP");
    CHECK(unsupported.status == 400);
    CHECK(nlohmann::json::parse(unsupported.body).at("code") == "UNSUPPORTED_CURRENCY");
}

TEST_CASE("REST API exposes authoritative selected-instrument route rules",
          "[api][rules]") {
    test::GatewayFixture fixture;
    GatewayApi api(*fixture.gateway);

    const auto response = call(
        api, "GET", "/api/v1/instruments?venue=OKX&symbol=ETH-USDT");
    REQUIRE(response.status == 200);
    const auto body = nlohmann::json::parse(response.body);
    CHECK(body.at("ok") == true);
    CHECK(body.at("venue") == "OKX");
    CHECK(body.at("symbol") == "ETH-USDT");
    CHECK(body.at("trading") == true);
    CHECK(body.at("minimumQuantity") == "0.0001");
    CHECK(body.at("quantityStep") == "0.000001");
    CHECK(body.at("priceTick") == "0.01");

    const auto unsupported = call(
        api, "GET", "/api/v1/instruments?venue=OKX&symbol=XRP-USDT");
    CHECK(unsupported.status == 400);
    CHECK(nlohmann::json::parse(unsupported.body).at("code") ==
          "UNSUPPORTED_INSTRUMENT");
}

TEST_CASE("REST API rejects exchange-specific fields", "[api][schema]") {
    test::GatewayFixture fixture;
    GatewayApi api(*fixture.gateway);
    auto body = order_body("leaky-schema", "OKX");
    body["tdMode"] = "cash";

    const auto response = call(api, "POST", "/api/v1/orders", std::move(body));
    CHECK(response.status == 400);
    const auto json = nlohmann::json::parse(response.body);
    CHECK(json.at("code") == "INVALID_REQUEST");
    CHECK(json.at("message").get<std::string>().find("common schema") != std::string::npos);
}

TEST_CASE("REST amend and cancel operations preserve request idempotency", "[api][idempotency]") {
    test::GatewayFixture fixture;
    GatewayApi api(*fixture.gateway);
    REQUIRE(call(api, "POST", "/api/v1/orders", order_body("rest-lifecycle", "OKX")).status ==
            201);

    const nlohmann::json amend{{"requestId", "amend-rest-1"}, {"newPrice", "49000"}};
    const auto amended = call(api, "PATCH", "/api/v1/orders/rest-lifecycle", amend);
    REQUIRE(amended.status == 200);
    CHECK(nlohmann::json::parse(amended.body).at("order").at("price") == "49000");

    const auto amend_replay = call(api, "PATCH", "/api/v1/orders/rest-lifecycle", amend);
    CHECK(amend_replay.status == 200);
    CHECK(nlohmann::json::parse(amend_replay.body).at("idempotentReplay") == true);

    const nlohmann::json cancel{{"requestId", "cancel-rest-1"}};
    const auto canceled = call(api, "DELETE", "/api/v1/orders/rest-lifecycle", cancel);
    REQUIRE(canceled.status == 200);
    CHECK(nlohmann::json::parse(canceled.body).at("order").at("status") == "CANCELED");

    const auto cancel_replay = call(api, "DELETE", "/api/v1/orders/rest-lifecycle", cancel);
    CHECK(cancel_replay.status == 200);
    CHECK(nlohmann::json::parse(cancel_replay.body).at("idempotentReplay") == true);
}

TEST_CASE("REST amend and cancel accept Idempotency-Key without a body requestId",
          "[api][idempotency][headers]") {
    test::GatewayFixture fixture;
    GatewayApi api(*fixture.gateway);
    REQUIRE(call(api, "POST", "/api/v1/orders",
                 order_body("rest-header-idempotency", "OKX")).status == 201);

    const auto amended = call(
        api, "PATCH", "/api/v1/orders/rest-header-idempotency",
        nlohmann::json{{"newPrice", "49000"}},
        {{"idempotency-key", "amend-from-header"}});
    REQUIRE(amended.status == 200);
    const auto amend_replay = call(
        api, "PATCH", "/api/v1/orders/rest-header-idempotency",
        nlohmann::json{{"newPrice", "49000"}},
        {{"idempotency-key", "amend-from-header"}});
    CHECK(nlohmann::json::parse(amend_replay.body).at("idempotentReplay") == true);

    const auto canceled = call(
        api, "DELETE", "/api/v1/orders/rest-header-idempotency",
        nlohmann::json::object(), {{"idempotency-key", "cancel-from-header"}});
    REQUIRE(canceled.status == 200);
    const auto cancel_replay = call(
        api, "DELETE", "/api/v1/orders/rest-header-idempotency",
        nlohmann::json::object(), {{"idempotency-key", "cancel-from-header"}});
    CHECK(nlohmann::json::parse(cancel_replay.body).at("idempotentReplay") == true);
}

TEST_CASE("REST API maps risk failures and publishes its schema", "[api][risk]") {
    test::GatewayFixture fixture;
    GatewayApi api(*fixture.gateway);
    auto body = order_body("rest-risk", "BINANCE");
    body["quantity"] = "3";

    const auto rejected = call(api, "POST", "/api/v1/orders", body);
    CHECK(rejected.status == 422);
    const auto rejected_json = nlohmann::json::parse(rejected.body);
    CHECK(rejected_json.at("code") == "MAX_ORDER_SIZE");
    CHECK(rejected_json.at("message") == "quantity 3 exceeds max order size 2");
    CHECK(rejected_json.at("order").at("rejectionReason") ==
          "MAX_ORDER_SIZE: quantity 3 exceeds max order size 2");
    const auto rejected_pipeline = nlohmann::json::parse(
        call(api, "GET", "/api/v1/orders/rest-risk/pipeline").body);
    REQUIRE(rejected_pipeline.at("events").size() == 1);
    CHECK(rejected_pipeline.at("events").front().at("code") == "ORDER_REJECTED");
    CHECK(rejected_pipeline.at("events").front().at("order").at("rejectionReason") ==
          "MAX_ORDER_SIZE: quantity 3 exceeds max order size 2");

    const auto schema = call(api, "GET", "/api/v1/openapi.json");
    REQUIRE(schema.status == 200);
    const auto document = nlohmann::json::parse(schema.body);
    CHECK(document.at("openapi") == "3.1.0");
    CHECK(document.at("components").at("schemas").at("OrderRequest").at("additionalProperties") ==
          false);
}

TEST_CASE("REST API exposes a durable sequenced order pipeline", "[api][pipeline]") {
    test::GatewayFixture fixture;
    GatewayApi api(*fixture.gateway);

    REQUIRE(call(api, "POST", "/api/v1/orders", order_body("pipeline-order", "OKX")).status ==
            201);
    REQUIRE(fixture.okx->emit("pipeline-order", OrderStatus::PartiallyFilled,
                              Decimal::parse("0.04"), Decimal::parse("50000"),
                              "pipeline-partial", 41));
    REQUIRE(fixture.okx->emit("pipeline-order", OrderStatus::Filled,
                              Decimal::parse("0.1"), Decimal::parse("50000"),
                              "pipeline-fill", 42));
    fixture.gateway->flush_events();

    const auto response = call(api, "GET", "/api/v1/orders/pipeline-order/pipeline");
    REQUIRE(response.status == 200);
    const auto body = nlohmann::json::parse(response.body);
    const auto& events = body.at("events");
    REQUIRE(events.size() >= 5);
    std::uint64_t previous_sequence = 0;
    for (const auto& event : events) {
        CHECK(event.at("sequence").get<std::uint64_t>() > previous_sequence);
        previous_sequence = event.at("sequence").get<std::uint64_t>();
        CHECK(event.at("clientOrderId") == "pipeline-order");
    }
    const auto event_with_code = [&events](std::string_view code) -> const nlohmann::json& {
        const auto found = std::ranges::find_if(events, [code](const auto& event) {
            return event.at("code") == code;
        });
        REQUIRE(found != events.end());
        return *found;
    };
    CHECK(event_with_code("ORDER_INTENT_PERSISTED").at("order").at("pendingAction") == "NEW");
    CHECK(event_with_code("ORDER_SENT_TO_EXCHANGE").at("order").at("symbol") == "BTC-USDT");
    CHECK(event_with_code("ORDER_ACKNOWLEDGED").at("order").at("exchangeOrderId")
              .get<std::string>()
              .starts_with("OKX-SIM-"));
    CHECK(event_with_code("ORDER_PARTIALLY_FILLED").at("order").at("filledQuantity") == "0.04");
    CHECK(event_with_code("ORDER_PARTIALLY_FILLED").at("order").at("venueSequence") == 41);
    CHECK(event_with_code("ORDER_FILLED").at("order").at("status") == "FILLED");
    CHECK(body.at("order").at("exchangeOrderId").get<std::string>().starts_with("OKX-SIM-"));

    REQUIRE(call(api, "POST", "/api/v1/orders", order_body("pipeline-cancel", "BINANCE")).status ==
            201);
    REQUIRE(call(api, "DELETE", "/api/v1/orders/pipeline-cancel",
                 {{"requestId", "pipeline-cancel-request"}}).status == 200);
    const auto cancel_pipeline = nlohmann::json::parse(
        call(api, "GET", "/api/v1/orders/pipeline-cancel/pipeline").body);
    const auto& cancel_events = cancel_pipeline.at("events");
    for (const auto code : {"CANCEL_INTENT_PERSISTED", "CANCEL_SENT_TO_EXCHANGE",
                            "CANCEL_ACKNOWLEDGED", "ORDER_CANCELED"}) {
        CHECK(std::ranges::any_of(cancel_events, [code](const auto& event) {
            return event.at("code") == code;
        }));
    }
    CHECK(cancel_pipeline.at("order").at("status") == "CANCELED");
}

TEST_CASE("REST API exposes fresh mapped quotes and executable best prices", "[api][market-data]") {
    test::GatewayFixture fixture;
    MarketDataBook book;
    const auto now = unix_time_ms();
    book.publish({
        .venue = Venue::Okx,
        .symbol = "BTC-USDT",
        .bid_price = Decimal::parse("59999"),
        .ask_price = Decimal::parse("60002"),
        .source_time_ms = now,
        .published_at_ms = now,
        .sequence = 1,
    });
    book.publish({
        .venue = Venue::Binance,
        .symbol = "BTC-USDT",
        .bid_price = Decimal::parse("60000"),
        .ask_price = Decimal::parse("60001"),
        .source_time_ms = now,
        .published_at_ms = now,
        .sequence = 2,
    });
    book.set_ring_status(true, 42, 2);
    GatewayApi api(*fixture.gateway, &book);

    const auto response = call(api, "GET", "/api/v1/market-data");
    REQUIRE(response.status == 200);
    const auto body = nlohmann::json::parse(response.body);
    CHECK(body.at("ring").at("connected") == true);
    CHECK(body.at("ring").at("mapped") == true);
    CHECK(body.at("ring").at("status") == "LIVE");
    CHECK(body.at("ring").at("lastSequence") == 2);
    CHECK(body.at("quotes").size() == 2);
    CHECK(body.at("quotes").front().contains("ageMs"));
    CHECK(body.at("maximumAgeMs") == 5000);
    CHECK(body.at("sources").at("OKX").at("transport") == "PUBLIC_REST");
    CHECK(body.at("sources").at("OKX").at("freshSymbols") == 1);
    CHECK(body.at("best").at("BTC-USDT").at("buy").at("venue") == "BINANCE");
    CHECK(body.at("best").at("BTC-USDT").at("buy").at("price") == "60001");
    CHECK(body.at("best").at("BTC-USDT").at("sell").at("venue") == "BINANCE");
    CHECK(body.at("best").at("BTC-USDT").at("sell").at("price") == "60000");

    // WebSocket transport is reflected when set explicitly
    book.set_ring_status(true, 42, 2, {}, "PUBLIC_WEBSOCKET");
    const auto ws_body = nlohmann::json::parse(
        call(api, "GET", "/api/v1/market-data").body);
    CHECK(ws_body.at("sources").at("OKX").at("transport") == "PUBLIC_WEBSOCKET");
    CHECK(ws_body.at("sources").at("BINANCE").at("transport") == "PUBLIC_WEBSOCKET");
}

TEST_CASE("REST API explains OMS transport persistence and stability", "[api][system]") {
    test::GatewayFixture fixture;
    GatewayApi api(*fixture.gateway);
    const auto response = call(api, "GET", "/api/v1/system");
    REQUIRE(response.status == 200);
    const auto body = nlohmann::json::parse(response.body);
    CHECK(body.at("transport").at("commands") == "REST API");
    CHECK(body.at("transport").at("updates") == "WebSocket");
    CHECK(body.at("exchangeConnectivity").at("venues").at("OKX").at("commands") ==
          "Authenticated REST API");
    CHECK(body.at("exchangeConnectivity").at("venues").at("BINANCE").at("commands") ==
          "Signed WebSocket API");
    CHECK(body.at("exchangeConnectivity").at("orderGatewayIsolation") == false);
    CHECK(body.at("exchangeConnectivity").at("restartDomain")
              .get<std::string>()
              .find("abex_server") != std::string::npos);
    CHECK(body.at("recoveryPolicy").at("transportFailure")
              .get<std::string>()
              .find("not blindly resent") != std::string::npos);
    CHECK(body.at("persistence").at("model") == "Checksummed append-only JSONL");
    CHECK(body.at("persistence").at("writeOrdering")
              .get<std::string>()
              .find("before exchange I/O") != std::string::npos);
    CHECK(body.at("stability").at("instanceId").get<std::string>().starts_with("gateway-"));
    CHECK_FALSE(body.at("events").empty());
}
