#include "abex/bootstrap/config_loader.hpp"
#include "abex/infrastructure/market_data_ring.hpp"
#include "abex/infrastructure/public_market_data.hpp"

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
        }
        else if (argument == "--once") result.once = true;
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
        abex::PublicMarketDataClient client({
            .okx_rest_url = market.value("okxRestUrl", std::string{"https://www.okx.com"}),
            .binance_rest_url = market.value(
                "binanceRestUrl", std::string{"https://data-api.binance.vision"}),
        });
        abex::MarketDataRingWriter writer(ring_path, capacity);
        std::signal(SIGINT, request_stop);
        std::signal(SIGTERM, request_stop);
        auto ready_fd = arguments.ready_fd;
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
