#include "abex/infrastructure/http_client.hpp"

#include <mutex>
#include <stdexcept>

#include <curl/curl.h>

namespace abex {
namespace {

void initialize_curl() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            throw std::runtime_error("failed to initialize libcurl");
        }
    });
}

std::size_t append_body(char* data, std::size_t size, std::size_t count, void* target) {
    const auto bytes = size * count;
    static_cast<std::string*>(target)->append(data, bytes);
    return bytes;
}

class CurlHeaders final {
public:
    ~CurlHeaders() { curl_slist_free_all(value_); }

    void append(const std::string& header) {
        auto* updated = curl_slist_append(value_, header.c_str());
        if (!updated) throw std::bad_alloc();
        value_ = updated;
    }
    [[nodiscard]] curl_slist* get() const noexcept { return value_; }

private:
    curl_slist* value_{nullptr};
};

} // namespace

HttpClient::HttpClient() { initialize_curl(); }

HttpResponse HttpClient::perform(const HttpRequest& request) const {
    if (request.url.empty()) throw std::invalid_argument("HTTP URL must not be empty");
    auto* handle = curl_easy_init();
    if (!handle) throw std::runtime_error("failed to allocate libcurl handle");

    HttpResponse response;
    CurlHeaders headers;
    for (const auto& [name, value] : request.headers) headers.append(name + ": " + value);

    curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, request.method.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, append_body);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout.count()));
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "abex-gateway/0.1");
    if (!request.body.empty()) {
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.data());
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(request.body.size()));
    }

    const auto result = curl_easy_perform(handle);
    if (result != CURLE_OK) {
        const std::string message = curl_easy_strerror(result);
        curl_easy_cleanup(handle);
        throw std::runtime_error("HTTP transport error: " + message);
    }
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response.status);
    curl_easy_cleanup(handle);
    return response;
}

} // namespace abex
