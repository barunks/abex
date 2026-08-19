#pragma once

#include <chrono>
#include <string>
#include <unordered_map>

namespace abex {

struct HttpRequest {
    std::string method{"GET"};
    std::string url;
    std::unordered_map<std::string, std::string> headers;
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
    [[nodiscard]] HttpResponse perform(const HttpRequest& request) const;
};

} // namespace abex
