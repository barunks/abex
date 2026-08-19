#pragma once

#include <cstddef>
#include <filesystem>

namespace abex {

struct EnvironmentLoadResult {
    bool file_found{false};
    std::size_t variables_loaded{0};
};

// Loads KEY=VALUE entries without logging their values. Existing process environment
// variables take precedence unless override_existing is explicitly requested.
[[nodiscard]] EnvironmentLoadResult load_environment_file(
    const std::filesystem::path& path,
    bool override_existing = false);

} // namespace abex
