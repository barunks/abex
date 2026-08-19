#include "abex/infrastructure/crypto.hpp"

#include <array>
#include <chrono>
#include <ctime>
#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace abex {
namespace {

[[nodiscard]] std::array<unsigned char, EVP_MAX_MD_SIZE>
hmac_sha256(std::string_view secret, std::string_view message, unsigned int& length) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    if (!HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
              reinterpret_cast<const unsigned char*>(message.data()), message.size(),
              digest.data(), &length)) {
        throw std::runtime_error("OpenSSL HMAC-SHA256 failed");
    }
    return digest;
}

void append_json_scalar(std::string& target, const nlohmann::json& value) {
    if (value.is_string()) {
        target += value.get_ref<const std::string&>();
        return;
    }
    if (value.is_boolean()) {
        target += value.get<bool>() ? "true" : "false";
        return;
    }
    if (value.is_number()) {
        target += value.dump();
        return;
    }
    throw std::invalid_argument("signed request parameters must be scalar values");
}

} // namespace

std::string hmac_sha256_hex(std::string_view secret, std::string_view message) {
    unsigned int length = 0;
    const auto digest = hmac_sha256(secret, message, length);
    constexpr char hex[] = "0123456789abcdef";
    std::string result(static_cast<std::size_t>(length) * 2, '0');
    for (unsigned int index = 0; index < length; ++index) {
        result[index * 2] = hex[digest[index] >> 4U];
        result[index * 2 + 1] = hex[digest[index] & 0x0fU];
    }
    return result;
}

std::string hmac_sha256_base64(std::string_view secret, std::string_view message) {
    unsigned int length = 0;
    const auto digest = hmac_sha256(secret, message, length);
    std::string result(4 * ((length + 2) / 3), '\0');
    const auto encoded = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(result.data()), digest.data(),
        static_cast<int>(length));
    if (encoded < 0) throw std::runtime_error("OpenSSL base64 encoding failed");
    result.resize(static_cast<std::size_t>(encoded));
    return result;
}

std::string canonical_query(const nlohmann::json& parameters) {
    std::string result;
    result.reserve(parameters.size() * 32);
    // nlohmann::json's default object_t is an ordered std::map, so iteration is
    // already the canonical lexicographic order required by Binance signing.
    for (auto it = parameters.begin(); it != parameters.end(); ++it) {
        if (it.key() == "signature") continue;
        if (!result.empty()) result.push_back('&');
        result += it.key();
        result.push_back('=');
        append_json_scalar(result, it.value());
    }
    return result;
}

std::string iso8601_utc_now() {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  now.time_since_epoch()) %
                              std::chrono::seconds{1};
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    if (!gmtime_r(&time, &utc)) throw std::runtime_error("failed to convert UTC timestamp");
    std::array<char, 25> output{};
    if (std::strftime(output.data(), output.size(), "%Y-%m-%dT%H:%M:%S", &utc) != 19) {
        throw std::runtime_error("failed to format UTC timestamp");
    }
    const auto value = milliseconds.count();
    output[19] = '.';
    output[20] = static_cast<char>('0' + (value / 100) % 10);
    output[21] = static_cast<char>('0' + (value / 10) % 10);
    output[22] = static_cast<char>('0' + value % 10);
    output[23] = 'Z';
    return {output.data(), 24};
}

std::int64_t unix_time_milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace abex
