#include "abex/server/http_server.hpp"

#include "abex/presentation/json_views.hpp"
#include "abex/server/gateway_api.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

namespace abex {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

namespace {

class WebSocketSession;

class ServerState final {
public:
    ServerState(OrderGateway& gateway_ref,
                MarketDataBook* market_data_ref,
                std::filesystem::path selected_web_root,
                std::string runtime_mode)
        : gateway(gateway_ref), market_data(market_data_ref),
          api(gateway_ref, market_data_ref, std::move(runtime_mode)),
          web_root(std::move(selected_web_root)) {}

    void join(const std::shared_ptr<WebSocketSession>& session);
    void broadcast(std::string message);
    void publish_order(const Order& order);
    void publish_quote(const MarketQuote& quote);
    void publish_operational(const OperationalEvent& event);

    OrderGateway& gateway;
    MarketDataBook* market_data;
    GatewayApi api;
    std::filesystem::path web_root;

private:
    std::mutex sessions_mutex_;
    std::vector<std::weak_ptr<WebSocketSession>> sessions_;
};

[[nodiscard]] std::string content_type(std::string_view path) {
    if (path.ends_with(".html")) return "text/html; charset=utf-8";
    if (path.ends_with(".css")) return "text/css; charset=utf-8";
    if (path.ends_with(".js")) return "text/javascript; charset=utf-8";
    if (path.ends_with(".json")) return "application/json";
    if (path.ends_with(".svg")) return "image/svg+xml";
    return "application/octet-stream";
}

[[nodiscard]] std::optional<std::string> read_static_file(const std::filesystem::path& root,
                                                          std::string_view target) {
    static const std::unordered_map<std::string_view, std::string_view> routes{
        {"/", "index.html"},
        {"/index.html", "index.html"},
        {"/styles.css", "styles.css"},
        {"/app.js", "app.js"},
    };
    const auto route = routes.find(target);
    if (route == routes.end()) return std::nullopt;
    std::ifstream input(root / route->second, std::ios::binary);
    if (!input) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

template <typename Body, typename Allocator>
[[nodiscard]] StringMap<std::string>
request_headers(const http::request<Body, http::basic_fields<Allocator>>& request) {
    StringMap<std::string> headers;
    headers.reserve(16);
    for (const auto& field : request) {
        std::string name(field.name_string());
        std::ranges::transform(name, name.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        headers.emplace(std::move(name), field.value());
    }
    return headers;
}

class WebSocketSession final : public std::enable_shared_from_this<WebSocketSession> {
public:
    WebSocketSession(tcp::socket socket, std::shared_ptr<ServerState> state)
        : websocket_(std::move(socket)), state_(std::move(state)) {}

    void run(http::request<http::string_body> request) {
        websocket_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        websocket_.set_option(websocket::stream_base::decorator(
            [](websocket::response_type& response) {
                response.set(http::field::server, "abex-gateway/1.0");
            }));
        websocket_.async_accept(
            request, beast::bind_front_handler(&WebSocketSession::accepted, shared_from_this()));
    }

    void send(std::string message) {
        asio::post(websocket_.get_executor(),
                   [self = shared_from_this(), message = std::move(message)]() mutable {
            if (self->outbound_.size() >= maximum_pending_messages) {
                // The first entry may be the buffer of an active async_write and cannot be
                // invalidated. Drop updates until that write completes, then replace the
                // backlog with one explicit resynchronization instruction.
                self->resync_pending_ = true;
                return;
            }
            self->outbound_.push_back(std::move(message));
            if (!self->writing_) self->write_next();
        });
    }

private:
    static constexpr std::size_t maximum_pending_messages = 256;

    void accepted(beast::error_code error) {
        if (error) return;
        state_->join(shared_from_this());
        auto orders = nlohmann::json::array();
        for (const auto& order : state_->gateway.list()) orders.push_back(order_view(order));
        send(nlohmann::json{
            {"type", "orders.snapshot"},
            {"orders", std::move(orders)},
            {"serverTime", unix_time_ms()},
        }.dump());
        if (state_->market_data) {
            auto snapshot = market_data_view(*state_->market_data);
            snapshot["type"] = "market.snapshot";
            send(snapshot.dump());
        }
        auto system = system_view(state_->gateway);
        system["type"] = "system.snapshot";
        send(system.dump());
        read_next();
    }

    void read_next() {
        websocket_.async_read(buffer_,
                              beast::bind_front_handler(&WebSocketSession::read, shared_from_this()));
    }

    void read(beast::error_code error, std::size_t) {
        if (error) return;
        try {
            const auto message = nlohmann::json::parse(beast::buffers_to_string(buffer_.data()));
            if (message.value("type", std::string{}) == "ping") {
                send(nlohmann::json{
                    {"type", "pong"},
                    {"serverTime", unix_time_ms()},
                }.dump());
            }
        } catch (const nlohmann::json::exception&) {
            send(nlohmann::json{
                {"type", "error"},
                {"code", "INVALID_MESSAGE"},
                {"message", "WebSocket messages must be JSON"},
            }.dump());
        }
        buffer_.consume(buffer_.size());
        read_next();
    }

    void write_next() {
        if (outbound_.empty()) {
            writing_ = false;
            return;
        }
        writing_ = true;
        websocket_.text(true);
        websocket_.async_write(asio::buffer(outbound_.front()),
                               beast::bind_front_handler(&WebSocketSession::written,
                                                         shared_from_this()));
    }

    void written(beast::error_code error, std::size_t) {
        if (error) return;
        outbound_.pop_front();
        if (resync_pending_) {
            outbound_.clear();
            outbound_.push_back(nlohmann::json{
                {"type", "resync.required"},
                {"serverTime", unix_time_ms()},
            }.dump());
            resync_pending_ = false;
        }
        write_next();
    }

    websocket::stream<tcp::socket> websocket_;
    std::shared_ptr<ServerState> state_;
    beast::flat_buffer buffer_;
    std::deque<std::string> outbound_;
    bool writing_{false};
    bool resync_pending_{false};
};

void ServerState::join(const std::shared_ptr<WebSocketSession>& session) {
    std::scoped_lock lock(sessions_mutex_);
    sessions_.push_back(session);
}

void ServerState::broadcast(std::string message) {
    std::vector<std::shared_ptr<WebSocketSession>> active;
    {
        std::scoped_lock lock(sessions_mutex_);
        std::erase_if(sessions_, [](const auto& session) { return session.expired(); });
        active.reserve(sessions_.size());
        for (auto& weak : sessions_) {
            if (auto session = weak.lock()) active.push_back(std::move(session));
        }
    }
    for (const auto& session : active) session->send(message);
}

void ServerState::publish_order(const Order& order) {
    broadcast(nlohmann::json{
        {"type", "order.updated"},
        {"order", order_view(order)},
        {"serverTime", unix_time_ms()},
    }.dump());
}

void ServerState::publish_quote(const MarketQuote& quote) {
    if (!market_data) return;
    broadcast(nlohmann::json{
        {"type", "market.updated"},
        {"quote", market_quote_view(*market_data, quote)},
        {"serverTime", unix_time_ms()},
    }.dump());
}

void ServerState::publish_operational(const OperationalEvent& event) {
    broadcast(nlohmann::json{
        {"type", "system.event"},
        {"event", operational_event_view(event)},
        {"serverTime", unix_time_ms()},
    }.dump());
}

class HttpSession final : public std::enable_shared_from_this<HttpSession> {
public:
    HttpSession(tcp::socket socket, std::shared_ptr<ServerState> state)
        : stream_(std::move(socket)), state_(std::move(state)) {}

    void run() { read_next(); }

private:
    void read_next() {
        parser_.emplace();
        parser_->body_limit(1024 * 1024);
        stream_.expires_after(std::chrono::seconds{30});
        http::async_read(stream_, buffer_, *parser_,
                         beast::bind_front_handler(&HttpSession::read, shared_from_this()));
    }

    void read(beast::error_code error, std::size_t) {
        if (error == http::error::end_of_stream) return close();
        if (error) return;
        auto request = parser_->release();
        parser_.reset();
        const auto target = std::string(request.target());
        if (websocket::is_upgrade(request) && target == "/ws/v1/orders") {
            std::make_shared<WebSocketSession>(stream_.release_socket(), state_)
                ->run(std::move(request));
            return;
        }
        respond(std::move(request));
    }

    void respond(http::request<http::string_body> request) {
        auto response = std::make_shared<http::response<http::string_body>>();
        response->version(request.version());
        response->keep_alive(request.keep_alive());
        response->set(http::field::server, "abex-gateway/1.0");

        const auto target = std::string(request.target());
        const bool api_request = target.starts_with("/api/");
        if (api_request) {
            const auto api_response = state_->api.handle({
                .method = std::string(request.method_string()),
                .target = target,
                .body = request.body(),
                .headers = request_headers(request),
            });
            response->result(static_cast<http::status>(api_response.status));
            response->set(http::field::content_type, api_response.content_type);
            for (const auto& [name, value] : api_response.headers) response->set(name, value);
            response->body() = api_response.body;
        } else if (request.method() != http::verb::get && request.method() != http::verb::head) {
            response->result(http::status::method_not_allowed);
            response->set(http::field::content_type, "application/json");
            response->body() = nlohmann::json{
                {"ok", false},
                {"code", "METHOD_NOT_ALLOWED"},
                {"message", "static resources support GET and HEAD only"},
            }.dump(2);
        } else {
            const auto file_target = target.substr(0, target.find('?'));
            const auto body = read_static_file(state_->web_root, file_target);
            if (!body) {
                response->result(http::status::not_found);
                response->set(http::field::content_type, "application/json");
                response->body() = nlohmann::json{
                    {"ok", false},
                    {"code", "RESOURCE_NOT_FOUND"},
                    {"message", "resource does not exist"},
                }.dump(2);
            } else {
                response->result(http::status::ok);
                response->set(http::field::content_type,
                              content_type(file_target == "/" ? "index.html" : file_target));
                response->set(http::field::cache_control, "no-cache");
                response->set("Content-Security-Policy",
                              "default-src 'self'; connect-src 'self' ws: wss:; "
                              "script-src 'self'; style-src 'self'; img-src 'self' data:");
                response->set("X-Content-Type-Options", "nosniff");
                response->set("X-Frame-Options", "DENY");
                if (request.method() != http::verb::head) response->body() = *body;
            }
        }
        response->prepare_payload();
        const bool keep_alive = response->keep_alive();
        http::async_write(stream_, *response,
                          [self = shared_from_this(), response, keep_alive](beast::error_code error,
                                                                           std::size_t) {
            if (error) return;
            if (!keep_alive) return self->close();
            self->read_next();
        });
    }

    void close() {
        beast::error_code ignored;
        stream_.socket().shutdown(tcp::socket::shutdown_send, ignored);
    }

    beast::tcp_stream stream_;
    std::shared_ptr<ServerState> state_;
    beast::flat_buffer buffer_;
    std::optional<http::request_parser<http::string_body>> parser_;
};

class Listener final : public std::enable_shared_from_this<Listener> {
public:
    Listener(asio::io_context& context,
             const tcp::endpoint& endpoint,
             std::shared_ptr<ServerState> state)
        : context_(context), acceptor_(asio::make_strand(context)), state_(std::move(state)) {
        beast::error_code error;
        acceptor_.open(endpoint.protocol(), error);
        if (error) throw std::runtime_error("server open failed: " + error.message());
        acceptor_.set_option(asio::socket_base::reuse_address(true), error);
        if (error) throw std::runtime_error("server reuse-address failed: " + error.message());
        acceptor_.bind(endpoint, error);
        if (error) {
            throw std::runtime_error(
                "server bind failed on " + endpoint.address().to_string() + ':' +
                std::to_string(endpoint.port()) + ": " + error.message() +
                "; stop the process using that port or start with --port PORT");
        }
        acceptor_.listen(asio::socket_base::max_listen_connections, error);
        if (error) throw std::runtime_error("server listen failed: " + error.message());
    }

    void run() { accept_next(); }

    void stop() noexcept {
        beast::error_code ignored;
        acceptor_.cancel(ignored);
        acceptor_.close(ignored);
    }

    [[nodiscard]] std::uint16_t port() const {
        beast::error_code error;
        const auto endpoint = acceptor_.local_endpoint(error);
        return error ? 0 : endpoint.port();
    }

private:
    void accept_next() {
        acceptor_.async_accept(asio::make_strand(context_),
                               beast::bind_front_handler(&Listener::accepted,
                                                         shared_from_this()));
    }

    void accepted(beast::error_code error, tcp::socket socket) {
        if (!error) std::make_shared<HttpSession>(std::move(socket), state_)->run();
        if (acceptor_.is_open()) accept_next();
    }

    asio::io_context& context_;
    tcp::acceptor acceptor_;
    std::shared_ptr<ServerState> state_;
};

} // namespace

class HttpServer::Impl final {
public:
    Impl(OrderGateway& gateway, MarketDataBook* market_data, Config config)
        : config_(std::move(config)),
          context_(static_cast<int>(std::max<std::size_t>(config_.io_threads, 1))),
          state_(std::make_shared<ServerState>(gateway, market_data, config_.web_root,
                                               config_.runtime_mode)),
          listener_(std::make_shared<Listener>(
              context_, tcp::endpoint(asio::ip::make_address(config_.address), config_.port), state_)) {}

    ~Impl() { stop(); }

    void start() {
        if (running_.exchange(true)) return;
        listener_->run();
        const auto thread_count = std::max<std::size_t>(config_.io_threads, 1);
        threads_.reserve(thread_count);
        for (std::size_t index = 0; index < thread_count; ++index) {
            threads_.emplace_back([this] { context_.run(); });
        }
    }

    void stop() noexcept {
        if (!running_.exchange(false)) return;
        listener_->stop();
        context_.stop();
        for (auto& thread : threads_) {
            if (thread.joinable()) thread.join();
        }
        threads_.clear();
    }

    void publish_order(const Order& order) { state_->publish_order(order); }
    void publish_quote(const MarketQuote& quote) { state_->publish_quote(quote); }
    void publish_operational(const OperationalEvent& event) {
        state_->publish_operational(event);
    }
    [[nodiscard]] std::uint16_t port() const noexcept { return listener_->port(); }

private:
    Config config_;
    asio::io_context context_;
    std::shared_ptr<ServerState> state_;
    std::shared_ptr<Listener> listener_;
    std::vector<std::thread> threads_;
    std::atomic<bool> running_{false};
};

HttpServer::HttpServer(OrderGateway& gateway) : HttpServer(gateway, Config{}) {}

HttpServer::HttpServer(OrderGateway& gateway, Config config)
    : gateway_(gateway), impl_(std::make_unique<Impl>(gateway, nullptr, std::move(config))) {
    observer_token_ = gateway_.add_order_observer(
        [this](const Order& order) { impl_->publish_order(order); });
    operational_observer_token_ = gateway_.add_operational_observer(
        [this](const OperationalEvent& event) { impl_->publish_operational(event); });
}

HttpServer::HttpServer(OrderGateway& gateway, MarketDataBook& market_data, Config config)
    : gateway_(gateway), market_data_(&market_data),
      impl_(std::make_unique<Impl>(gateway, market_data_, std::move(config))) {
    observer_token_ = gateway_.add_order_observer(
        [this](const Order& order) { impl_->publish_order(order); });
    operational_observer_token_ = gateway_.add_operational_observer(
        [this](const OperationalEvent& event) { impl_->publish_operational(event); });
    market_observer_token_ = market_data_->add_observer(
        [this](const MarketQuote& quote) { impl_->publish_quote(quote); });
}

HttpServer::~HttpServer() {
    gateway_.remove_order_observer(observer_token_);
    gateway_.remove_operational_observer(operational_observer_token_);
    if (market_data_ && market_observer_token_ != 0) {
        market_data_->remove_observer(market_observer_token_);
    }
    stop();
}

void HttpServer::start() { impl_->start(); }

void HttpServer::stop() noexcept { impl_->stop(); }

std::uint16_t HttpServer::port() const noexcept { return impl_->port(); }

} // namespace abex
