#pragma once

#include <string_view>

namespace abex {

enum class HeartbeatTimerAction { Send, Timeout };

// Allocation-free protocol state shared by the WebSocket timer and read path.
// Keeping this state separate makes the missing-pong transition deterministic
// and independently testable without opening a TLS socket.
class ApplicationHeartbeat final {
public:
    [[nodiscard]] HeartbeatTimerAction timer_elapsed() const noexcept {
        return awaiting_response_ ? HeartbeatTimerAction::Timeout
                                  : HeartbeatTimerAction::Send;
    }

    void request_sent() noexcept { awaiting_response_ = true; }

    [[nodiscard]] bool observe(std::string_view message,
                               std::string_view expected_response) noexcept {
        if (message != expected_response) return false;
        awaiting_response_ = false;
        return true;
    }

    void reset() noexcept { awaiting_response_ = false; }
    [[nodiscard]] bool awaiting_response() const noexcept { return awaiting_response_; }

private:
    bool awaiting_response_{false};
};

} // namespace abex
