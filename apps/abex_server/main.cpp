#include "abex/bootstrap/config_loader.hpp"
#include "abex/bootstrap/gateway_runtime.hpp"
#include "abex/server/http_server.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

extern "C" void request_stop(int) { stop_requested = 1; }

struct Arguments {
    std::optional<std::filesystem::path> config;
    std::optional<std::filesystem::path> state;
    std::optional<std::filesystem::path> web_root;
    std::optional<std::string> address;
    std::optional<std::uint16_t> port;
    std::filesystem::path environment{".env"};
    abex::RuntimeMode mode{abex::RuntimeMode::Live};
    bool spawn_market_data{true};
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
        else if (argument == "--state") result.state = value("--state");
        else if (argument == "--env-file") result.environment = value("--env-file");
        else if (argument == "--web-root") result.web_root = value("--web-root");
        else if (argument == "--address") result.address = value("--address");
        else if (argument == "--port") {
            const auto parsed = std::stoul(value("--port"));
            if (parsed > 65535) throw std::invalid_argument("port must be between 0 and 65535");
            result.port = static_cast<std::uint16_t>(parsed);
        } else if (argument == "--mode") {
            result.mode = abex::runtime_mode_from_string(value("--mode"));
        } else if (argument == "--no-market-data") result.spawn_market_data = false;
        else if (argument == "--help" || argument == "-h") {
            std::cout << "Usage: abex_server --config FILE.json|FILE.yaml|FILE.cfg|FILE.config "
                         "[--state FILE] [--env-file FILE] "
                         "[--web-root DIR] "
                         "[--address IP] [--port PORT] [--mode live|simulation] "
                         "[--no-market-data]\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown argument: " + argument);
        }
    }
    return result;
}

[[nodiscard]] std::string child_status(int status) {
    if (WIFEXITED(status)) return "exit code " + std::to_string(WEXITSTATUS(status));
    if (WIFSIGNALED(status)) return "signal " + std::to_string(WTERMSIG(status));
    return "unknown status";
}

class MarketDataProcess final {
public:
    MarketDataProcess() = default;
    ~MarketDataProcess() { stop(); }

    MarketDataProcess(const MarketDataProcess&) = delete;
    MarketDataProcess& operator=(const MarketDataProcess&) = delete;

    void start(const std::filesystem::path& config) {
        if (pid_ > 0) throw std::logic_error("market-data process is already running");
        const auto executable =
            std::filesystem::canonical("/proc/self/exe").parent_path() / "abex_market_data";
        if (!std::filesystem::exists(executable)) {
            throw std::runtime_error(
                "cannot start complete setup because abex_market_data is missing beside " +
                executable.parent_path().string() +
                "; build it or use --no-market-data");
        }

        std::array<int, 2> ready_pipe{-1, -1};
        if (::pipe(ready_pipe.data()) != 0) {
            throw std::runtime_error(std::string("failed to create market-data readiness pipe: ") +
                                     std::strerror(errno));
        }

        pid_ = ::fork();
        if (pid_ < 0) {
            const auto error = errno;
            ::close(ready_pipe[0]);
            ::close(ready_pipe[1]);
            throw std::runtime_error(std::string("failed to spawn market-data process: ") +
                                     std::strerror(error));
        }
        if (pid_ == 0) {
            ::close(ready_pipe[0]);
            std::vector<std::string> values{
                executable.string(), "--config", config.string(),
                "--ready-fd", std::to_string(ready_pipe[1]),
            };
            std::vector<char*> child_arguments;
            child_arguments.reserve(values.size() + 1);
            for (auto& value : values) child_arguments.push_back(value.data());
            child_arguments.push_back(nullptr);
            ::execv(executable.c_str(), child_arguments.data());
            std::cerr << "fatal: failed to execute market-data publisher: "
                      << std::strerror(errno) << '\n';
            ::close(ready_pipe[1]);
            ::_exit(127);
        }

        ::close(ready_pipe[1]);
        try {
            wait_until_ready(ready_pipe[0]);
        } catch (...) {
            ::close(ready_pipe[0]);
            stop();
            throw;
        }
        ::close(ready_pipe[0]);
    }

    void ensure_running() {
        if (pid_ <= 0) return;
        int status = 0;
        const auto result = ::waitpid(pid_, &status, WNOHANG);
        if (result == 0 || (result < 0 && errno == EINTR)) return;
        if (result == pid_) {
            pid_ = -1;
            throw std::runtime_error("market-data publisher stopped unexpectedly with " +
                                     child_status(status));
        }
        if (result < 0) {
            throw std::runtime_error(std::string("failed to monitor market-data publisher: ") +
                                     std::strerror(errno));
        }
    }

    void stop() noexcept {
        if (pid_ <= 0) return;
        (void)::kill(pid_, SIGTERM);
        for (int attempt = 0; attempt < 100; ++attempt) {
            int status = 0;
            const auto result = ::waitpid(pid_, &status, WNOHANG);
            if (result == pid_ || (result < 0 && errno == ECHILD)) {
                pid_ = -1;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
        (void)::kill(pid_, SIGKILL);
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
        pid_ = -1;
    }

private:
    void wait_until_ready(int descriptor) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
        while (std::chrono::steady_clock::now() < deadline) {
            if (stop_requested != 0) throw std::runtime_error("market-data startup interrupted");
            pollfd readiness{.fd = descriptor, .events = POLLIN, .revents = 0};
            const auto result = ::poll(&readiness, 1, 100);
            if (result < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error(std::string("failed while waiting for market data: ") +
                                         std::strerror(errno));
            }
            if ((readiness.revents & POLLIN) != 0) {
                char marker = 0;
                if (::read(descriptor, &marker, 1) == 1 && marker == 'R') return;
            }

            int status = 0;
            const auto child = ::waitpid(pid_, &status, WNOHANG);
            if (child == pid_) {
                pid_ = -1;
                throw std::runtime_error("market-data publisher failed to start with " +
                                         child_status(status));
            }
            if ((readiness.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                throw std::runtime_error(
                    "market-data publisher closed its readiness channel before the first quote");
            }
        }
        throw std::runtime_error(
            "timed out after 30 seconds waiting for the first market-data quote");
    }

    pid_t pid_{-1};
};

} // namespace

int main(int argc, char** argv) {
    try {
        const auto arguments = parse_arguments(argc, argv);
        if (!arguments.config)
            throw std::invalid_argument(
                "--config FILE is required (.json/.cfg/.config or .yaml/.yml); "
                "see config/gateway.example.json or config/gateway.example.yaml");
        const auto config = abex::load_config(*arguments.config);
        const auto server_config = config.value("server", nlohmann::json::object());
        const auto address =
            arguments.address.value_or(server_config.value("address", std::string{"127.0.0.1"}));
        std::signal(SIGINT, request_stop);
        std::signal(SIGTERM, request_stop);

        abex::GatewayRuntime runtime(config, arguments.state, arguments.mode,
                                     arguments.environment);
        abex::HttpServer server(runtime.gateway(), runtime.market_data(), {
            .address = address,
            .port = arguments.port.value_or(server_config.value("port", std::uint16_t{8080})),
            .io_threads = server_config.value("ioThreads", std::size_t{2}),
            .web_root = arguments.web_root.value_or(
                server_config.value("webRoot", std::filesystem::path{"web"})),
            .runtime_mode = std::string(abex::to_string(runtime.mode())),
            .request_timeout = std::chrono::seconds{server_config.value("requestTimeoutS", 30)},
        });
        server.start();

        MarketDataProcess market_data;
        if (arguments.spawn_market_data) {
            std::cout << "Starting separate market-data publisher and waiting for its first quote...\n"
                      << std::flush;
            market_data.start(*arguments.config);
            std::cout << "Market-data stream readiness confirmed.\n" << std::flush;
        }
        if (address != "127.0.0.1" && address != "::1") {
            std::cerr << "warning: this exercise server has no client authentication or TLS; "
                         "do not expose it to an untrusted network\n";
        }
        const auto base_url = "http://" + address + ':' + std::to_string(server.port());
        std::cout << "ABEX complete setup ready (" << abex::to_string(runtime.mode()) << ")\n"
                  << "UI: " << base_url << "\nREST: " << base_url
                  << "/api/v1/orders\nSystem: " << base_url
                  << "/api/v1/system\nWebSocket: /ws/v1/orders\n"
                  << "Market data: "
                  << (arguments.spawn_market_data
                          ? "separate supervised mmap publisher\n"
                          : "external publisher required (--no-market-data)\n")
                  << std::flush;
        while (stop_requested == 0) {
            market_data.ensure_running();
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
        }
        server.stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
}
