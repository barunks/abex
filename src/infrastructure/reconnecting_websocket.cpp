#include "abex/infrastructure/reconnecting_websocket.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <openssl/ssl.h>

namespace abex {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

namespace {

struct WebSocketUrl {
    std::string host;
    std::string port;
    std::string target;
};

[[nodiscard]] WebSocketUrl parse_url(std::string_view url) {
    constexpr std::string_view prefix = "wss://";
    if (!url.starts_with(prefix)) throw std::invalid_argument("only wss:// URLs are supported");
    url.remove_prefix(prefix.size());
    const auto slash = url.find('/');
    const auto authority = url.substr(0, slash);
    const auto target = slash == std::string_view::npos ? std::string_view{"/"} : url.substr(slash);
    if (authority.empty()) throw std::invalid_argument("WebSocket URL has no host");

    const auto colon = authority.rfind(':');
    WebSocketUrl parsed;
    if (colon != std::string_view::npos) {
        parsed.host = authority.substr(0, colon);
        parsed.port = authority.substr(colon + 1);
    } else {
        parsed.host = authority;
        parsed.port = "443";
    }
    parsed.target = target;
    if (parsed.host.empty() || parsed.port.empty()) {
        throw std::invalid_argument("invalid WebSocket URL authority");
    }
    return parsed;
}

} // namespace

class ReconnectingWebSocket::Impl final {
public:
    using Stream = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

    struct OutboundMessage {
        std::string payload;
        bool heartbeat{false};
    };

    explicit Impl(Config config)
        : config_(std::move(config)), url_(parse_url(config_.url)), ssl_context_(ssl::context::tls_client),
          work_(asio::make_work_guard(io_)), resolver_(io_), reconnect_timer_(io_),
          heartbeat_timer_(io_),
          reconnect_delay_(config_.initial_reconnect_delay) {
        const bool heartbeat_enabled = !config_.application_heartbeat_request.empty();
        if (heartbeat_enabled &&
            (config_.application_heartbeat_response.empty() ||
             config_.application_heartbeat_idle <= std::chrono::milliseconds::zero() ||
             config_.application_heartbeat_timeout <= std::chrono::milliseconds::zero())) {
            throw std::invalid_argument(
                "application heartbeat requires a response and positive idle/timeout intervals");
        }
        ssl_context_.set_default_verify_paths();
        ssl_context_.set_verify_mode(ssl::verify_peer);
    }

    ~Impl() { stop(); }

    void start(OpenCallback on_open,
               MessageCallback on_message,
               ConnectionCallback on_connection) {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) return;
        on_open_ = std::move(on_open);
        on_message_ = std::move(on_message);
        on_connection_ = std::move(on_connection);
        thread_ = std::thread([this] { io_.run(); });
        asio::post(io_, [this] { connect(); });
    }

    void stop() noexcept {
        if (!running_.exchange(false)) return;
        stopping_.store(true);
        asio::post(io_, [this] {
            boost::system::error_code ignored;
            resolver_.cancel();
            reconnect_timer_.cancel();
            heartbeat_timer_.cancel();
            if (socket_) {
                beast::get_lowest_layer(*socket_).socket().cancel(ignored);
                beast::get_lowest_layer(*socket_).socket().close(ignored);
                socket_.reset();
            }
            set_connected(false, "stopped");
            work_.reset();
            io_.stop();
        });
        if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
    }

    [[nodiscard]] bool send(std::string message) {
        if (!running_.load() || stopping_.load()) return false;
        asio::post(io_, [this, message = std::move(message)]() mutable {
            outbound_.push_back({.payload = std::move(message)});
            if (connected_.load() && !write_in_progress_) write_next();
        });
        return true;
    }

    [[nodiscard]] bool wait_connected(std::chrono::milliseconds timeout) {
        std::unique_lock lock(connection_mutex_);
        return connection_condition_.wait_for(lock, timeout, [this] {
            return connected_.load() || !running_.load();
        }) && connected_.load();
    }

    [[nodiscard]] bool connected() const noexcept { return connected_.load(); }

private:
    void connect() {
        if (stopping_.load()) return;
        reconnect_scheduled_ = false;
        resolver_.async_resolve(url_.host, url_.port,
                                [this](boost::system::error_code error,
                                       tcp::resolver::results_type endpoints) {
            if (error) return fail(nullptr, error, "resolve");
            auto socket = std::make_shared<Stream>(io_, ssl_context_);
            socket_ = socket;
            socket->next_layer().set_verify_mode(ssl::verify_peer);
            socket->next_layer().set_verify_callback(ssl::host_name_verification(url_.host));
            if (!SSL_set_tlsext_host_name(socket->next_layer().native_handle(), url_.host.c_str())) {
                return fail(socket,
                            boost::system::error_code(static_cast<int>(::ERR_get_error()),
                                                      asio::error::get_ssl_category()),
                            "SNI");
            }
            beast::get_lowest_layer(*socket).expires_after(std::chrono::seconds{10});
            beast::get_lowest_layer(*socket).async_connect(
                endpoints, [this, socket](boost::system::error_code connect_error,
                                          const tcp::endpoint&) {
                    if (connect_error) return fail(socket, connect_error, "connect");
                    beast::get_lowest_layer(*socket).expires_after(std::chrono::seconds{10});
                    socket->next_layer().async_handshake(
                        ssl::stream_base::client,
                        [this, socket](boost::system::error_code handshake_error) {
                            if (handshake_error) return fail(socket, handshake_error, "TLS handshake");
                            beast::get_lowest_layer(*socket).expires_never();
                            socket->set_option(websocket::stream_base::timeout::suggested(
                                beast::role_type::client));
                            socket->set_option(websocket::stream_base::decorator(
                                [](websocket::request_type& request) {
                                    request.set(boost::beast::http::field::user_agent,
                                                "abex-gateway/0.1");
                                }));
                            const auto host_header = url_.port == "443"
                                                         ? url_.host
                                                         : url_.host + ':' + url_.port;
                            socket->async_handshake(
                                host_header, url_.target,
                                [this, socket](boost::system::error_code websocket_error) {
                                    if (websocket_error) {
                                        return fail(socket, websocket_error, "WebSocket handshake");
                                    }
                                    if (socket != socket_) return;
                                    socket->text(true);
                                    reconnect_delay_ = config_.initial_reconnect_delay;
                                    set_connected(true, {});
                                    if (on_open_) on_open_();
                                    read_next(socket, std::make_shared<beast::flat_buffer>());
                                    if (!outbound_.empty() && !write_in_progress_) write_next();
                                    arm_heartbeat(socket, config_.application_heartbeat_idle);
                                });
                        });
                });
        });
    }

    void read_next(const std::shared_ptr<Stream>& socket,
                   const std::shared_ptr<beast::flat_buffer>& buffer) {
        socket->async_read(*buffer, [this, socket, buffer](boost::system::error_code error,
                                                           std::size_t) {
            if (error) return fail(socket, error, "read");
            if (socket != socket_) return;
            const auto message = beast::buffers_to_string(buffer->data());
            buffer->consume(buffer->size());
            const bool heartbeat_response = heartbeat_enabled() &&
                                            message == config_.application_heartbeat_response;
            if (heartbeat_response) {
                awaiting_heartbeat_response_ = false;
            }
            if (!awaiting_heartbeat_response_) {
                arm_heartbeat(socket, config_.application_heartbeat_idle);
            }
            if (!heartbeat_response && on_message_) on_message_(message);
            read_next(socket, buffer);
        });
    }

    void write_next() {
        if (!socket_ || outbound_.empty() || !connected_.load()) return;
        write_in_progress_ = true;
        auto socket = socket_;
        auto message = std::make_shared<OutboundMessage>(outbound_.front());
        socket->async_write(asio::buffer(message->payload),
                            [this, socket, message](boost::system::error_code error,
                                                    std::size_t) {
            if (error) {
                write_in_progress_ = false;
                return fail(socket, error, "write");
            }
            if (socket != socket_) return;
            outbound_.pop_front();
            write_in_progress_ = false;
            if (message->heartbeat) {
                awaiting_heartbeat_response_ = true;
                arm_heartbeat(socket, config_.application_heartbeat_timeout);
            }
            if (!outbound_.empty()) write_next();
        });
    }

    [[nodiscard]] bool heartbeat_enabled() const noexcept {
        return !config_.application_heartbeat_request.empty();
    }

    void arm_heartbeat(const std::shared_ptr<Stream>& socket,
                       std::chrono::milliseconds delay) {
        if (!heartbeat_enabled() || socket != socket_ || !connected_.load()) return;
        heartbeat_timer_.expires_after(delay);
        heartbeat_timer_.async_wait([this, socket](boost::system::error_code error) {
            if (error == asio::error::operation_aborted) return;
            if (error || socket != socket_ || !connected_.load() || stopping_.load()) return;
            if (awaiting_heartbeat_response_) {
                return fail(socket, asio::error::timed_out, "application heartbeat");
            }
            outbound_.push_back({.payload = config_.application_heartbeat_request,
                                 .heartbeat = true});
            if (!write_in_progress_) write_next();
        });
    }

    void fail(const std::shared_ptr<Stream>& socket,
              boost::system::error_code error,
              std::string_view operation) {
        if (stopping_.load()) return;
        if (socket && socket_ && socket != socket_) return;
        if (reconnect_scheduled_) return;
        reconnect_scheduled_ = true;
        write_in_progress_ = false;
        awaiting_heartbeat_response_ = false;
        boost::system::error_code ignored;
        heartbeat_timer_.cancel(ignored);
        // Requests whose write did not complete have already become unknown to callers.
        // Never replay them implicitly after reconnect; idempotency/reconciliation decides retry.
        outbound_.clear();
        const auto message = std::string(operation) + ": " + error.message();
        set_connected(false, message);
        if (socket_) {
            beast::get_lowest_layer(*socket_).socket().cancel(ignored);
            beast::get_lowest_layer(*socket_).socket().close(ignored);
            socket_.reset();
        }
        reconnect_timer_.expires_after(reconnect_delay_);
        reconnect_delay_ = std::min(config_.maximum_reconnect_delay, reconnect_delay_ * 2);
        reconnect_timer_.async_wait([this](boost::system::error_code timer_error) {
            if (!timer_error && !stopping_.load()) connect();
        });
    }

    void set_connected(bool value, std::string_view reason) {
        const bool changed = connected_.exchange(value) != value;
        connection_condition_.notify_all();
        if (changed && on_connection_) on_connection_(value, reason);
    }

    Config config_;
    WebSocketUrl url_;
    asio::io_context io_;
    ssl::context ssl_context_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    tcp::resolver resolver_;
    asio::steady_timer reconnect_timer_;
    asio::steady_timer heartbeat_timer_;
    std::shared_ptr<Stream> socket_;
    std::deque<OutboundMessage> outbound_;
    bool write_in_progress_{false};
    bool awaiting_heartbeat_response_{false};
    bool reconnect_scheduled_{false};
    std::chrono::milliseconds reconnect_delay_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> connected_{false};
    std::thread thread_;
    mutable std::mutex connection_mutex_;
    std::condition_variable connection_condition_;
    OpenCallback on_open_;
    MessageCallback on_message_;
    ConnectionCallback on_connection_;
};

ReconnectingWebSocket::ReconnectingWebSocket(Config config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

ReconnectingWebSocket::~ReconnectingWebSocket() = default;

void ReconnectingWebSocket::start(OpenCallback on_open,
                                  MessageCallback on_message,
                                  ConnectionCallback on_connection) {
    impl_->start(std::move(on_open), std::move(on_message), std::move(on_connection));
}

void ReconnectingWebSocket::stop() noexcept { impl_->stop(); }

bool ReconnectingWebSocket::send(std::string message) { return impl_->send(std::move(message)); }

bool ReconnectingWebSocket::wait_connected(std::chrono::milliseconds timeout) {
    return impl_->wait_connected(timeout);
}

bool ReconnectingWebSocket::connected() const noexcept { return impl_->connected(); }

} // namespace abex
