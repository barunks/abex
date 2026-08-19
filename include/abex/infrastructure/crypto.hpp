#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace abex {

[[nodiscard]] std::string hmac_sha256_hex(std::string_view secret,
                                          std::string_view message);
[[nodiscard]] std::string hmac_sha256_base64(std::string_view secret,
                                             std::string_view message);
[[nodiscard]] std::string canonical_query(const nlohmann::json& parameters);
[[nodiscard]] std::string iso8601_utc_now();
[[nodiscard]] std::int64_t unix_time_milliseconds();

} // namespace abex
