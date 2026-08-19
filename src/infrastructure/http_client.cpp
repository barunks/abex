#include "abex/infrastructure/http_client.hpp"

#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <vector>

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
    try {
        static_cast<std::string*>(target)->append(data, bytes);
        return bytes;
    } catch (...) {
        return 0;
    }
}

class CurlHeaders final {
public:
    ~CurlHeaders() { curl_slist_free_all(value_); }

    void append(std::string_view name, std::string_view value) {
        std::string header;
        header.reserve(name.size() + value.size() + 2);
        header.append(name);
        header += ": ";
        header.append(value);
        auto* updated = curl_slist_append(value_, header.c_str());
        if (!updated) throw std::bad_alloc();
        value_ = updated;
    }
    [[nodiscard]] curl_slist* get() const noexcept { return value_; }

private:
    curl_slist* value_{nullptr};
};

} // namespace

class HttpClient::Impl final {
public:
    Impl() {
        initialize_curl();
        handles_.reserve(pool_size);
        available_.reserve(pool_size);
        try {
            for (std::size_t index = 0; index < pool_size; ++index) {
                auto* handle = curl_easy_init();
                if (!handle) throw std::runtime_error("failed to allocate libcurl handle");
                handles_.push_back(handle);
                available_.push_back(handle);
            }
        } catch (...) {
            for (auto* handle : handles_) curl_easy_cleanup(handle);
            throw;
        }
    }

    ~Impl() {
        for (auto* handle : handles_) curl_easy_cleanup(handle);
    }

    [[nodiscard]] CURL* acquire() {
        std::unique_lock lock(mutex_);
        available_condition_.wait(lock, [this] { return !available_.empty(); });
        auto* handle = available_.back();
        available_.pop_back();
        return handle;
    }

    void release(CURL* handle) noexcept {
        {
            std::scoped_lock lock(mutex_);
            available_.push_back(handle);
        }
        available_condition_.notify_one();
    }

private:
    static constexpr std::size_t pool_size = 4;
    std::mutex mutex_;
    std::condition_variable available_condition_;
    std::vector<CURL*> handles_;
    std::vector<CURL*> available_;
};

HttpClient::HttpClient() : impl_(std::make_unique<Impl>()) {}

HttpClient::~HttpClient() = default;

HttpResponse HttpClient::perform(const HttpRequest& request) const {
    if (request.url.empty()) throw std::invalid_argument("HTTP URL must not be empty");
    HttpResponse response;
    response.body.reserve(4096);
    CurlHeaders headers;
    for (const auto& [name, value] : request.headers) headers.append(name, value);

    auto* handle = impl_->acquire();
    curl_easy_reset(handle); // Keeps this handle's connection and DNS caches alive.
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
        const auto* message = curl_easy_strerror(result);
        impl_->release(handle);
        throw std::runtime_error(std::string("HTTP transport error: ") + message);
    }
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response.status);
    impl_->release(handle);
    return response;
}

} // namespace abex
