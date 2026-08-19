#include "abex/bootstrap/environment.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace abex {
namespace {

[[nodiscard]] std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] bool valid_name(std::string_view name) {
    if (name.empty() || (!std::isalpha(static_cast<unsigned char>(name.front())) &&
                         name.front() != '_')) {
        return false;
    }
    for (const auto character : name.substr(1)) {
        if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_') {
            return false;
        }
    }
    return true;
}

[[noreturn]] void parse_error(const std::filesystem::path& path,
                              std::size_t line,
                              std::string_view reason) {
    throw std::runtime_error("invalid environment file " + path.string() + " at line " +
                             std::to_string(line) + ": " + std::string(reason));
}

[[nodiscard]] std::string parse_quoted_value(std::string_view input,
                                             char quote,
                                             const std::filesystem::path& path,
                                             std::size_t line) {
    std::string result;
    bool escaped = false;
    std::size_t index = 1;
    for (; index < input.size(); ++index) {
        const auto character = input[index];
        if (quote == '"' && escaped) {
            switch (character) {
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case '\\': result.push_back('\\'); break;
            case '"': result.push_back('"'); break;
            default:
                result.push_back('\\');
                result.push_back(character);
                break;
            }
            escaped = false;
            continue;
        }
        if (quote == '"' && character == '\\') {
            escaped = true;
            continue;
        }
        if (character == quote) break;
        result.push_back(character);
    }
    if (escaped || index == input.size()) parse_error(path, line, "unterminated quoted value");

    const auto remainder = trim(input.substr(index + 1));
    if (!remainder.empty() && remainder.front() != '#') {
        parse_error(path, line, "unexpected characters after quoted value");
    }
    return result;
}

[[nodiscard]] std::string parse_value(std::string_view input,
                                      const std::filesystem::path& path,
                                      std::size_t line) {
    input = trim(input);
    if (input.empty()) return {};
    if (input.front() == '\'' || input.front() == '"') {
        return parse_quoted_value(input, input.front(), path, line);
    }

    for (std::size_t index = 0; index < input.size(); ++index) {
        if (input[index] == '#' && index > 0 &&
            std::isspace(static_cast<unsigned char>(input[index - 1]))) {
            input = input.substr(0, index);
            break;
        }
    }
    return std::string(trim(input));
}

void set_environment(std::string_view name, std::string_view value) {
    const std::string owned_name{name};
    const std::string owned_value{value};
    if (::setenv(owned_name.c_str(), owned_value.c_str(), 1) != 0) {
        throw std::runtime_error("failed to set environment variable " + owned_name);
    }
}

} // namespace

EnvironmentLoadResult load_environment_file(const std::filesystem::path& path,
                                            bool override_existing) {
    std::error_code status_error;
    const auto status = std::filesystem::status(path, status_error);
    if (status_error) {
        if (status_error == std::errc::no_such_file_or_directory) return {};
        throw std::runtime_error("cannot inspect environment file " + path.string() + ": " +
                                 status_error.message());
    }
    if (!std::filesystem::is_regular_file(status)) {
        throw std::runtime_error("environment file is not a regular file: " + path.string());
    }

    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open environment file: " + path.string());

    EnvironmentLoadResult result{.file_found = true};
    std::size_t line_number = 0;
    for (std::string line; std::getline(input, line);) {
        ++line_number;
        static constexpr std::string_view utf8_bom{"\xEF\xBB\xBF"};
        std::string_view entry = line;
        if (line_number == 1 && entry.starts_with(utf8_bom)) {
            entry.remove_prefix(utf8_bom.size());
        }
        entry = trim(entry);
        if (entry.empty() || entry.front() == '#') continue;
        if (entry.starts_with("export") && entry.size() > 6 &&
            std::isspace(static_cast<unsigned char>(entry[6]))) {
            entry = trim(entry.substr(7));
        }

        const auto separator = entry.find('=');
        if (separator == std::string_view::npos) {
            parse_error(path, line_number, "expected NAME=VALUE");
        }
        const auto name = trim(entry.substr(0, separator));
        if (!valid_name(name)) parse_error(path, line_number, "invalid variable name");
        const auto value = parse_value(entry.substr(separator + 1), path, line_number);

        const std::string owned_name{name};
        if (!override_existing && std::getenv(owned_name.c_str()) != nullptr) continue;
        set_environment(name, value);
        ++result.variables_loaded;
    }
    if (input.bad()) {
        throw std::runtime_error("failed while reading environment file: " + path.string());
    }
    return result;
}

} // namespace abex
