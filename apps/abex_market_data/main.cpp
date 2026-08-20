#include "abex/bootstrap/config_loader.hpp"
#include "abex/infrastructure/market_data_ring.hpp"
#include "abex/infrastructure/public_market_data.hpp"
#include "abex/infrastructure/reconnecting_websocket.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

extern "C" void request_stop(int) { stop_requested = 1; }

struct Arguments {
    std::optional<std::filesystem::path> config;
    std::optional<std::filesystem::path> ring_path;
    std::optional<std::chrono::milliseconds> interval;
    std::optional<std::size_t> capacity;
    std::optional<int> ready_fd;
    bool once{false};
};

[[nodiscard]] Arguments parse_arguments(int argc, char** argv) {
    Arguments result;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto value = [&](std::string_view name) -> std::string {
            if (++index >= argc) throw std::invalid_argument("missing value for " + std::string(name));
            return argv[index];
        };
        if (argument == "--config") result.config = value("--config");
        else if (argument == "--ring-file") result.ring_path = value("--ring-file");
        else if (argument == "--interval-ms") {
            result.interval = std::chrono::milliseconds{std::stoll(value("--interval-ms"))};
        } else if (argument == "--capacity") {
            result.capacity = std::stoull(value("--capacity"));
        } else if (argument == "--ready-fd") {
            const auto parsed = std::stoul(value("--ready-fd"));
            if (parsed > static_cast<unsigned long>(std::numeric_limits<int>::max())) {
                throw std::invalid_argument("ready file descriptor is out of range");
            }
            result.ready_fd = static_cast<int>(parsed);
        } else if (argument == "--no-ui") {
            // Backward-compatible no-op: this executable is always publisher-only.
        } else if (argument == "--once") result.once = true;
        else if (argument == "--help" || argument == "-h") {
            std::cout << "Usage: abex_market_data --config FILE.json|FILE.yaml|FILE.cfg|FILE.config "
                         "[--ring-file FILE] "
                         "[--interval-ms N] [--capacity N] [--once]\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown argument: " + argument);
        }
    }
    return result;
}

void notify_parent_ready(std::optional<int>& descriptor) {
    if (!descriptor) return;
    constexpr char ready = 'R';
    while (::write(*descriptor, &ready, 1) < 0) {
        if (errno == EINTR) continue;
        const auto error = errno;
        ::close(*descriptor);
        descriptor.reset();
        throw std::runtime_error("failed to notify OMS that market data is ready: " +
                                 std::string(std::strerror(error)));
    }
    ::close(*descriptor);
    descriptor.reset();
}

// ── WebSocket quote parsers ───────────────────────────────────────────────

// OKX public WS: subscribe to tickers channel.
// Subscribe message: {"op":"subscribe","args":[{"channel":"tickers","instId":"BTC-USDT"},
//                                              {"channel":"tickers","instId":"ETH-USDT"}]}
// Update message: {"arg":{"channel":"tickers","instId":"BTC-USDT"},
//                  "data":[{"instId":"BTC-USDT","bidPx":"...","askPx":"...","ts":"..."}]}
[[nodiscard]] std::optional<abex::MarketQuote> parse_okx_ws_message(std::string_view text) {
    try {
        const auto msg = nlohmann::json::parse(text);
        if (!msg.contains("data") || !msg.at("data").is_array() || msg.at("data").empty())
            return std::nullopt;
        const auto& item = msg.at("data").front();
        const auto symbol = item.value("instId", std::string{});
        if (symbol != "BTC-USDT" && symbol != "ETH-USDT") return std::nullopt;
        const auto bid = item.value("bidPx", std::string{});
        const auto ask = item.value("askPx", std::string{});
        if (bid.empty() || ask.empty() || bid == "0" || ask == "0") return std::nullopt;
        const auto now = abex::unix_time_ms();
        const auto ts = item.contains("ts") && item.at("ts").is_string()
                            ? std::stoll(item.at("ts").get<std::string>()) : now;
        return abex::MarketQuote{
            .venue           = abex::Venue::Okx,
            .symbol          = symbol,
            .bid_price       = abex::Decimal::parse(bid),
            .ask_price       = abex::Decimal::parse(ask),
            .source_time_ms  = ts,
            .published_at_ms = now,
        };
    } catch (...) { return std::nullopt; }
}

// Binance public WS: combined stream bookTicker.
// URL: wss://stream.binance.com:9443/stream?streams=btcusdt@bookTicker/ethusdt@bookTicker
// Message: {"stream":"btcusdt@bookTicker","data":{"s":"BTCUSDT","b":"...","a":"..."}}
[[nodiscard]] std::optional<abex::MarketQuote> parse_binance_ws_message(std::string_view text) {
    try {
        const auto msg = nlohmann::json::parse(text);
        const nlohmann::json* item = nullptr;
        nlohmann::json data_holder;
        if (msg.contains("data")) {
            item = &msg.at("data");
        } else if (msg.contains("s")) {
            item = &msg;
        } else {
            return std::nullopt;
        }
        const auto raw_symbol = item->value("s", std::string{});
        std::string symbol;
        if (raw_symbol == "BTCUSDT") symbol = "BTC-USDT";
        else if (raw_symbol == "ETHUSDT") symbol = "ETH-USDT";
        else return std::nullopt;
        const auto bid = item->value("b", std::string{});
        const auto ask = item->value("a", std::string{});
        if (bid.empty() || ask.empty()) return std::nullopt;
        const auto now = abex::unix_time_ms();
        return abex::MarketQuote{
            .venue           = abex::Venue::Binance,
            .symbol          = std::move(symbol),
            .bid_price       = abex::Decimal::parse(bid),
            .ask_price       = abex::Decimal::parse(ask),
            .source_time_ms  = now,
            .published_at_ms = now,
        };
    } catch (...) { return std::nullopt; }
}

// ── WebSocket publisher ───────────────────────────────────────────────────

// Runs two ReconnectingWebSocket instances (one per venue) and writes every
// parsed quote directly to the ring. The ring writer is protected by a mutex
// because both WS callbacks fire on different threads.
int run_websocket_mode(const std::string& okx_ws_url,
                       const std::string& binance_ws_url,
                       abex::MarketDataRingWriter& writer,
                       std::optional<int>& ready_fd,
                       bool once) {
    std::mutex ring_mutex;
    std::atomic<std::size_t> published{0};

    auto publish_quote = [&](abex::MarketQuote quote) {
        std::scoped_lock lock(ring_mutex);
        writer.publish(std::span<const abex::MarketQuote>(&quote, 1));
        published.fetch_add(1, std::memory_order_relaxed);
        notify_parent_ready(ready_fd);
    };

    // OKX public WS — tickers channel
    abex::ReconnectingWebSocket okx_ws({
        .url = okx_ws_url,
        .application_heartbeat_request  = R"({"op":"ping"})",
        .application_heartbeat_response = "pong",
        .application_heartbeat_idle     = std::chrono::seconds{20},
        .application_heartbeat_timeout  = std::chrono::seconds{10},
    });
    okx_ws.start(
        [&okx_ws] {
            (void)okx_ws.send(R"({"op":"subscribe","args":[)"
                        R"({"channel":"tickers","instId":"BTC-USDT"},)"
                        R"({"channel":"tickers","instId":"ETH-USDT"}]})");
        },
        [&publish_quote](std::string_view text) {
            if (auto quote = parse_okx_ws_message(text)) publish_quote(std::move(*quote));
        },
        [](bool connected, std::string_view reason) {
            std::cout << "OKX WS " << (connected ? "connected" : "disconnected")
                      << (reason.empty() ? "" : ": ") << reason << '\n' << std::flush;
        });

    // Binance public WS — combined bookTicker stream
    abex::ReconnectingWebSocket binance_ws({.url = binance_ws_url});
    binance_ws.start(
        nullptr,
        [&publish_quote](std::string_view text) {
            if (auto quote = parse_binance_ws_message(text)) publish_quote(std::move(*quote));
        },
        [](bool connected, std::string_view reason) {
            std::cout << "Binance WS " << (connected ? "connected" : "disconnected")
                      << (reason.empty() ? "" : ": ") << reason << '\n' << std::flush;
        });

    std::cout << "ABEX market-data publisher (WebSocket mode): OKX=" << okx_ws_url
              << " Binance=" << binance_ws_url << '\n' << std::flush;

    if (once) {
        // Wait up to 10 s for at least one quote from each venue then exit.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (std::chrono::steady_clock::now() < deadline && stop_requested == 0) {
            if (published.load() >= 2) break;
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
        okx_ws.stop();
        binance_ws.stop();
        return published.load() == 0 ? 2 : 0;
    }

    while (stop_requested == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    okx_ws.stop();
    binance_ws.stop();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto arguments = parse_arguments(argc, argv);
        if (!arguments.config)
            throw std::invalid_argument(
                "--config FILE is required (.json/.cfg/.config or .yaml/.yml); "
                "see config/gateway.example.json or config/gateway.example.yaml");
        const auto config = abex::load_config(*arguments.config);
        const auto market = config.value("marketData", nlohmann::json::object());
        const auto ring_path = arguments.ring_path.value_or(
            market.value("ringPath", std::filesystem::path{"state/market-data.ring"}));
        const auto interval = arguments.interval.value_or(
            std::chrono::milliseconds{market.value("publishIntervalMs", 1000)});
        const auto capacity = arguments.capacity.value_or(market.value("ringCapacity", 1024U));
        if (interval <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("publish interval must be positive");
        }

        abex::MarketDataRingWriter writer(ring_path, capacity);
        std::signal(SIGINT, request_stop);
        std::signal(SIGTERM, request_stop);
        auto ready_fd = arguments.ready_fd;

        // WebSocket mode: both public WS URLs must be configured.
        const auto okx_ws_url     = market.value("okxPublicWebSocketUrl", std::string{});
        const auto binance_ws_url = market.value("binancePublicWebSocketUrl", std::string{});
        if (!okx_ws_url.empty() && !binance_ws_url.empty()) {
            return run_websocket_mode(okx_ws_url, binance_ws_url, writer, ready_fd,
                                      arguments.once);
        }

        // REST poll mode (default): concurrent per-venue fetches, one tick per interval.
        abex::PublicMarketDataClient client({
            .okx_rest_url = market.value("okxRestUrl", std::string{"https://www.okx.com"}),
            .binance_rest_url = market.value(
                "binanceRestUrl", std::string{"https://data-api.binance.vision"}),
        });
        std::cout << "ABEX market-data publisher ready: " << ring_path << " (1 writer, capacity "
                  << capacity << ", interval " << interval.count() << " ms)\n"
                  << "Publisher process only: quotes flow through mmap to abex_server.\n"
                  << std::flush;

        auto next_tick = std::chrono::steady_clock::now();
        std::uint64_t tick = 0;
        do {
            ++tick;
            auto okx = std::async(std::launch::async, [&client] { return client.fetch_okx(); });
            auto binance =
                std::async(std::launch::async, [&client] { return client.fetch_binance(); });
            std::size_t published = 0;
            try {
                const auto quotes = okx.get();
                writer.publish(quotes);
                published += quotes.size();
            } catch (const std::exception& error) {
                std::cerr << "OKX market-data tick failed: " << error.what() << '\n';
            }
            try {
                const auto quotes = binance.get();
                writer.publish(quotes);
                published += quotes.size();
            } catch (const std::exception& error) {
                std::cerr << "Binance market-data tick failed: " << error.what() << '\n';
            }
            std::cout << "tick=" << tick << " published=" << published
                      << " ring-sequence=" << writer.sequence() << '\n'
                      << std::flush;
            if (published != 0) notify_parent_ready(ready_fd);
            if (arguments.once) return published == 0 ? 2 : 0;
            next_tick += interval;
            std::this_thread::sleep_until(next_tick);
        } while (stop_requested == 0);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
}
