#include "abex/bootstrap/gateway_runtime.hpp"
#include "abex/cli/command_processor.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace {

struct Arguments {
    std::filesystem::path config{"config/gateway.example.json"};
    std::filesystem::path environment{".env"};
    std::optional<std::filesystem::path> state;
    std::optional<std::string> command;
    abex::RuntimeMode mode{abex::RuntimeMode::Live};
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
        else if (argument == "--env-file") result.environment = value("--env-file");
        else if (argument == "--state") result.state = value("--state");
        else if (argument == "--command") result.command = value("--command");
        else if (argument == "--mode") result.mode = abex::runtime_mode_from_string(value("--mode"));
        else if (argument == "--help" || argument == "-h") {
            std::cout << "Usage: abex_cli [--config FILE] [--state FILE] [--env-file FILE] "
                         "[--mode live|simulation] "
                         "[--command 'COMMAND']\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown argument: " + argument);
        }
    }
    return result;
}

[[nodiscard]] nlohmann::json read_config(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open configuration: " + path.string());
    return nlohmann::json::parse(input);
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto arguments = parse_arguments(argc, argv);
        const auto config = read_config(arguments.config);
        abex::GatewayRuntime runtime(config, arguments.state, arguments.mode,
                                     arguments.environment);
        abex::CommandProcessor processor(runtime.gateway(), runtime.simulated_adapters());

        if (arguments.command) {
            const auto response = processor.execute(*arguments.command);
            if (!response.output.empty()) std::cout << response.output << '\n';
            return response.output.find("\"ok\": false") == std::string::npos ? 0 : 2;
        }

        std::cout << "ABEX gateway ready (" << abex::to_string(runtime.mode())
                  << "). Type 'help' for commands.\n";
        for (std::string line; std::cout << "abex> " && std::getline(std::cin, line);) {
            const auto response = processor.execute(line);
            if (!response.output.empty()) std::cout << response.output << '\n';
            if (response.exit_requested) break;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
}
