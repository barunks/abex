#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace abex {

struct HttpRequest {
    std::string method{"GET"};
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::chrono::milliseconds timeout{5000};
};

struct HttpResponse {
    long status{0};
    std::string body;
};

class HttpClient final {
public:
    HttpClient();
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&) = delete;
    HttpClient& operator=(HttpClient&&) = delete;

    [[nodiscard]] HttpResponse perform(const HttpRequest& request) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace abex
