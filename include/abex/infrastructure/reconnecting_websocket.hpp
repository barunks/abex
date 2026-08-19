#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace abex {

class ReconnectingWebSocket final {
public:
    struct Config {
        std::string url;
        std::chrono::milliseconds initial_reconnect_delay{250};
        std::chrono::milliseconds maximum_reconnect_delay{10'000};
        std::string application_heartbeat_request;
        std::string application_heartbeat_response;
        std::chrono::milliseconds application_heartbeat_idle{0};
        std::chrono::milliseconds application_heartbeat_timeout{10'000};
        std::size_t maximum_outbound_messages{1024};
    };

    using OpenCallback = std::function<void()>;
    using MessageCallback = std::function<void(std::string_view)>;
    using ConnectionCallback = std::function<void(bool, std::string_view)>;

    explicit ReconnectingWebSocket(Config config);
    ~ReconnectingWebSocket();

    ReconnectingWebSocket(const ReconnectingWebSocket&) = delete;
    ReconnectingWebSocket& operator=(const ReconnectingWebSocket&) = delete;

    void start(OpenCallback on_open,
               MessageCallback on_message,
               ConnectionCallback on_connection);
    void stop() noexcept;
    [[nodiscard]] bool send(std::string message);
    [[nodiscard]] bool wait_connected(std::chrono::milliseconds timeout);
    [[nodiscard]] bool connected() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace abex
