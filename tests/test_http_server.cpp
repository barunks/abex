#include "abex/server/http_server.hpp"

#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace abex;

namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

[[nodiscard]] http::response<http::string_body> http_call(std::uint16_t port,
                                                           http::verb method,
                                                           std::string target,
                                                           std::string body = {}) {
    asio::io_context context;
    tcp::resolver resolver(context);
    beast::tcp_stream stream(context);
    stream.connect(resolver.resolve("127.0.0.1", std::to_string(port)));

    http::request<http::string_body> request(method, std::move(target), 11);
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::content_type, "application/json");
    request.body() = std::move(body);
    request.prepare_payload();
    http::write(stream, request);

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(stream, buffer, response);
    beast::error_code ignored;
    stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
    return response;
}

} // namespace

TEST_CASE("HTTP server hosts the UI and exchange-neutral REST API", "[server][integration]") {
    test::GatewayFixture fixture;
    MarketDataBook market_data;
    const auto now = unix_time_ms();
    market_data.publish({
        .venue = Venue::Okx,
        .symbol = "ETH-USDT",
        .bid_price = Decimal::parse("2999"),
        .ask_price = Decimal::parse("3001"),
        .source_time_ms = now,
        .published_at_ms = now,
        .sequence = 1,
    });
    market_data.set_ring_status(true, 7, 1);
    HttpServer server(*fixture.gateway, market_data, {
                                            .address = "127.0.0.1",
                                            .port = 0,
                                            .io_threads = 2,
                                            .web_root = std::filesystem::path(ABEX_SOURCE_DIR) / "web",
                                            .runtime_mode = "simulation",
                                        });
    server.start();

    const auto ui = http_call(server.port(), http::verb::get, "/");
    REQUIRE(ui.result() == http::status::ok);
    CHECK(ui.at(http::field::content_type).starts_with("text/html"));
    CHECK(ui.body().find("ABEX") != std::string::npos);
    CHECK(ui.body().find("How orders reach each exchange") != std::string::npos);
    CHECK(ui.body().find("ROUTEABLE QTY RANGE") != std::string::npos);
    CHECK(ui.body().find("ALL CURRENCIES") == std::string::npos);
    CHECK(ui.body().find("venueConnectivityState") != std::string::npos);

    const auto health = http_call(server.port(), http::verb::get, "/api/v1/health");
    REQUIRE(health.result() == http::status::ok);
    const auto health_json = nlohmann::json::parse(health.body());
    CHECK(health_json.at("mode") == "simulation");
    CHECK(health_json.at("venues").contains("OKX"));

    const auto quotes = http_call(server.port(), http::verb::get, "/api/v1/market-data");
    REQUIRE(quotes.result() == http::status::ok);
    const auto quote_json = nlohmann::json::parse(quotes.body());
    CHECK(quote_json.at("ring").at("connected") == true);
    CHECK(quote_json.at("quotes").front().at("symbol") == "ETH-USDT");

    const auto system = http_call(server.port(), http::verb::get, "/api/v1/system");
    REQUIRE(system.result() == http::status::ok);
    const auto system_json = nlohmann::json::parse(system.body());
    CHECK(system_json.at("transport").at("updates") == "WebSocket");
    CHECK(system_json.at("exchangeConnectivity").at("venues").at("BINANCE").at("commands") ==
          "Signed WebSocket API");

    const nlohmann::json order{
        {"clientOrderId", "http-binance"},
        {"venue", "BINANCE"},
        {"symbol", "BTC-USDT"},
        {"side", "BUY"},
        {"type", "LIMIT"},
        {"price", "50000"},
        {"quantity", "0.1"},
        {"timeInForce", "GTC"},
    };
    const auto placed =
        http_call(server.port(), http::verb::post, "/api/v1/orders", order.dump());
    CHECK(placed.result() == http::status::created);
    CHECK(nlohmann::json::parse(placed.body()).at("order").at("venue") == "BINANCE");
}

TEST_CASE("WebSocket endpoint streams snapshots and normalized updates", "[server][websocket]") {
    test::GatewayFixture fixture;
    MarketDataBook market_data;
    HttpServer server(*fixture.gateway, market_data, {
                                            .address = "127.0.0.1",
                                            .port = 0,
                                            .io_threads = 2,
                                            .web_root = std::filesystem::path(ABEX_SOURCE_DIR) / "web",
                                            .runtime_mode = "simulation",
                                        });
    server.start();

    asio::io_context context;
    tcp::resolver resolver(context);
    websocket::stream<tcp::socket> socket(context);
    asio::connect(socket.next_layer(), resolver.resolve("127.0.0.1", std::to_string(server.port())));
    socket.handshake("127.0.0.1", "/ws/v1/orders");

    beast::flat_buffer buffer;
    socket.read(buffer);
    const auto snapshot = nlohmann::json::parse(beast::buffers_to_string(buffer.data()));
    CHECK(snapshot.at("type") == "orders.snapshot");
    buffer.consume(buffer.size());

    socket.read(buffer);
    const auto market_snapshot = nlohmann::json::parse(beast::buffers_to_string(buffer.data()));
    CHECK(market_snapshot.at("type") == "market.snapshot");
    buffer.consume(buffer.size());

    socket.read(buffer);
    const auto system_snapshot = nlohmann::json::parse(beast::buffers_to_string(buffer.data()));
    CHECK(system_snapshot.at("type") == "system.snapshot");
    CHECK(system_snapshot.at("persistence").contains("recordSequence"));
    buffer.consume(buffer.size());

    REQUIRE(fixture.gateway->place(test::limit_order("ws-update", Venue::Okx)).ok);
    nlohmann::json update;
    for (int attempt = 0; attempt < 16; ++attempt) {
        socket.read(buffer);
        update = nlohmann::json::parse(beast::buffers_to_string(buffer.data()));
        buffer.consume(buffer.size());
        if (update.at("type") == "order.updated" &&
            update.at("order").at("clientOrderId") == "ws-update") {
            break;
        }
    }
    CHECK(update.at("type") == "order.updated");
    CHECK(update.at("order").at("clientOrderId") == "ws-update");

    const auto now = unix_time_ms();
    market_data.publish({
        .venue = Venue::Binance,
        .symbol = "BTC-USDT",
        .bid_price = Decimal::parse("60000"),
        .ask_price = Decimal::parse("60001"),
        .source_time_ms = now,
        .published_at_ms = now,
        .sequence = 9,
    });
    nlohmann::json market_update;
    for (int attempt = 0; attempt < 16; ++attempt) {
        socket.read(buffer);
        market_update = nlohmann::json::parse(beast::buffers_to_string(buffer.data()));
        buffer.consume(buffer.size());
        if (market_update.at("type") == "market.updated") break;
    }
    CHECK(market_update.at("type") == "market.updated");
    CHECK(market_update.at("quote").at("venue") == "BINANCE");
    CHECK(market_update.at("quote").at("ask") == "60001");

    beast::error_code ignored;
    socket.close(websocket::close_code::normal, ignored);
}
