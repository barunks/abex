#include "abex/bootstrap/environment.hpp"
#include "abex/bootstrap/gateway_runtime.hpp"
#include "abex/infrastructure/binance_adapter.hpp"
#include "abex/infrastructure/okx_adapter.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

using namespace abex;

namespace {

void set_test_environment(const std::string& name, const std::optional<std::string>& value) {
    const auto result = value ? ::setenv(name.c_str(), value->c_str(), 1) : ::unsetenv(name.c_str());
    if (result != 0) throw std::runtime_error("failed to update test environment");
}

class EnvironmentGuard final {
public:
    explicit EnvironmentGuard(std::string name) : name_(std::move(name)) {
        if (const auto* value = std::getenv(name_.c_str())) original_ = value;
    }
    ~EnvironmentGuard() { set_test_environment(name_, original_); }

private:
    std::string name_;
    std::optional<std::string> original_;
};

class TemporaryEnvironmentFile final {
public:
    explicit TemporaryEnvironmentFile(std::string_view contents)
        : path_(std::filesystem::temp_directory_path() /
                ("abex-env-test-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 ".env")) {
        std::ofstream output(path_);
        REQUIRE(output.good());
        output << contents;
        REQUIRE(output.good());
    }
    ~TemporaryEnvironmentFile() { std::filesystem::remove(path_); }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("environment file feeds the exact live adapter credential names", "[environment]") {
    const std::string okx_api{"ABEX_OKX_API_KEY"};
    const std::string okx_secret{"ABEX_OKX_SECRET_KEY"};
    const std::string okx_passphrase{"ABEX_OKX_PASSPHRASE"};
    const std::string binance_api{"ABEX_BINANCE_API_KEY"};
    const std::string binance_secret{"ABEX_BINANCE_SECRET_KEY"};
    EnvironmentGuard guard1(okx_api);
    EnvironmentGuard guard2(okx_secret);
    EnvironmentGuard guard3(okx_passphrase);
    EnvironmentGuard guard4(binance_api);
    EnvironmentGuard guard5(binance_secret);

    TemporaryEnvironmentFile file{
        "# Exchange credentials\n"
        "export ABEX_OKX_API_KEY=okx-api\n"
        "ABEX_OKX_SECRET_KEY='okx secret'\n"
        "ABEX_OKX_PASSPHRASE=okx-passphrase # ignored comment\n"
        "ABEX_BINANCE_API_KEY=binance-api\n"
        "ABEX_BINANCE_SECRET_KEY=binance-secret\n"};
    const auto loaded = load_environment_file(file.path(), true);
    REQUIRE(loaded.file_found);
    CHECK(loaded.variables_loaded == 5);

    const nlohmann::json okx_json{{"restUrl", "https://okx.invalid"},
                                  {"privateWebSocketUrl", "wss://okx.invalid"}};
    const auto okx = OkxAdapter::Config::from_environment(okx_json, true);
    CHECK(okx.api_key == "okx-api");
    CHECK(okx.secret_key == "okx secret");
    CHECK(okx.passphrase == "okx-passphrase");

    const nlohmann::json binance_json{{"webSocketUrl", "wss://binance.invalid"}};
    const auto binance = BinanceAdapter::Config::from_environment(binance_json);
    CHECK(binance.api_key == "binance-api");
    CHECK(binance.secret_key == "binance-secret");
}

TEST_CASE("process environment takes precedence over environment file", "[environment]") {
    const std::string name{"ABEX_TEST_ENV_PRECEDENCE"};
    EnvironmentGuard guard(name);
    set_test_environment(name, "from-process");
    TemporaryEnvironmentFile file{"ABEX_TEST_ENV_PRECEDENCE=from-file\n"};

    const auto loaded = load_environment_file(file.path());
    REQUIRE(loaded.file_found);
    CHECK(loaded.variables_loaded == 0);
    REQUIRE(std::getenv(name.c_str()) != nullptr);
    CHECK(std::string{std::getenv(name.c_str())} == "from-process");
}

TEST_CASE("runtime mode is selected dynamically", "[runtime]") {
    CHECK(runtime_mode_from_string("live") == RuntimeMode::Live);
    CHECK(runtime_mode_from_string("LIVE") == RuntimeMode::Live);
    CHECK(runtime_mode_from_string("simulation") == RuntimeMode::Simulation);
    CHECK(runtime_mode_from_string("simulated") == RuntimeMode::Simulation);
    CHECK(to_string(RuntimeMode::Live) == "live");
    CHECK(to_string(RuntimeMode::Simulation) == "simulation");
    CHECK_THROWS_AS(runtime_mode_from_string("test"), std::invalid_argument);
}
