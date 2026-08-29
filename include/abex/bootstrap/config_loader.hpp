#pragma once

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

namespace abex {

[[nodiscard]] inline nlohmann::json load_config(const std::filesystem::path& path) {
    const auto ext = path.extension().string();
    if (ext == ".yaml" || ext == ".yml") {
        const auto root = YAML::LoadFile(path.string());
        // Convert YAML to JSON string then parse
        std::function<nlohmann::json(const YAML::Node&)> convert = [&](const YAML::Node& node) -> nlohmann::json {
            switch (node.Type()) {
                case YAML::NodeType::Map: {
                    nlohmann::json obj = nlohmann::json::object();
                    for (const auto& kv : node)
                        obj[kv.first.as<std::string>()] = convert(kv.second);
                    return obj;
                }
                case YAML::NodeType::Sequence: {
                    nlohmann::json arr = nlohmann::json::array();
                    for (const auto& item : node)
                        arr.push_back(convert(item));
                    return arr;
                }
                case YAML::NodeType::Scalar: {
                    const auto& val = node.Scalar();
                    if (val == "true" || val == "True" || val == "TRUE") return true;
                    if (val == "false" || val == "False" || val == "FALSE") return false;
                    if (val == "null" || val == "~") return nullptr;
                    try { return std::stoll(val); } catch (...) {}
                    try { return std::stod(val); } catch (...) {}
                    return val;
                }
                default:
                    return nullptr;
            }
        };
        return convert(root);
    }
    // JSON and .cfg/.config/.properties files
    std::ifstream file(path);
    if (!file) throw std::runtime_error("cannot open config file: " + path.string());
    return nlohmann::json::parse(file);
}

} // namespace abex
