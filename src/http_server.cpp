#include "cppmcp/transport/http_server.hpp"

#include "cppmcp/server.hpp"
#include "cppmcp/transport/http.hpp"

#include <asio.hpp>
#include <llhttp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cctype>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cppmcp/core/log.hpp"

namespace cppmcp::transport {
namespace {

using Tcp = asio::ip::tcp;

struct ParseState {
    HttpRequest request;
    std::string field;
    std::string value;
    bool reading_value = false;
    bool complete = false;
    bool headers_complete = false;
    std::size_t header_bytes = 0;
    std::size_t max_header_bytes = 0;
    std::size_t max_body_bytes = 0;
    int rejection_status = 0;
};

ParseState& parse_state(llhttp_t* parser) {
    return *static_cast<ParseState*>(parser->data);
}

void finish_header(ParseState& state) {
    if (!state.field.empty()) {
        state.request.headers.emplace_back(std::move(state.field), std::move(state.value));
    }
    state.field.clear();
    state.value.clear();
    state.reading_value = false;
}

int on_url(llhttp_t* parser, const char* data, std::size_t length) {
    auto& state = parse_state(parser);
    state.header_bytes += length;
    if (state.header_bytes > state.max_header_bytes) {
        state.rejection_status = 431;
        return HPE_USER;
    }
    state.request.target.append(data, length);
    return 0;
}

int on_header_field(llhttp_t* parser, const char* data, std::size_t length) {
    auto& state = parse_state(parser);
    if (state.reading_value) {
        finish_header(state);
    }
    state.header_bytes += length;
    if (state.header_bytes > state.max_header_bytes) {
        state.rejection_status = 431;
        return HPE_USER;
    }
    state.field.append(data, length);
    return 0;
}

int on_header_value(llhttp_t* parser, const char* data, std::size_t length) {
    auto& state = parse_state(parser);
    state.header_bytes += length;
    if (state.header_bytes > state.max_header_bytes) {
        state.rejection_status = 431;
        return HPE_USER;
    }
    state.reading_value = true;
    state.value.append(data, length);
    return 0;
}

int on_headers_complete(llhttp_t* parser) {
    auto& state = parse_state(parser);
    finish_header(state);
    state.request.method = llhttp_method_name(static_cast<llhttp_method_t>(parser->method));
    state.headers_complete = true;
    if ((parser->flags & F_CONTENT_LENGTH) != 0 &&
        parser->content_length > state.max_body_bytes) {
        state.rejection_status = 413;
        return HPE_USER;
    }
    return 0;
}

int on_body(llhttp_t* parser, const char* data, std::size_t length) {
    auto& state = parse_state(parser);
    if (length > state.max_body_bytes -
                     std::min(state.max_body_bytes, state.request.body.size())) {
        state.rejection_status = 413;
        return HPE_USER;
    }
    state.request.body.append(data, length);
    return 0;
}

int on_message_complete(llhttp_t* parser) {
    parse_state(parser).complete = true;
    llhttp_pause(parser);
    return 0;
}

llhttp_settings_t parser_settings() {
    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_url = on_url;
    settings.on_header_field = on_header_field;
    settings.on_header_value = on_header_value;
    settings.on_headers_complete = on_headers_complete;
    settings.on_body = on_body;
    settings.on_message_complete = on_message_complete;
    return settings;
}

struct DisconnectWatch {
    std::atomic<bool> stopped{false};
    std::atomic<bool> notified{false};
    std::function<void()> callback;
};

void notify_disconnect(const std::shared_ptr<DisconnectWatch>& watch) {
    if (!watch->stopped.load(std::memory_order_acquire) &&
        !watch->notified.exchange(true, std::memory_order_acq_rel) && watch->callback) {
        watch->callback();
    }
}

void watch_for_disconnect(const std::shared_ptr<Tcp::socket>& socket,
                          const std::shared_ptr<DisconnectWatch>& watch) {
    socket->async_wait(Tcp::socket::wait_read,
                       [socket, watch](const asio::error_code& error) {
        if (watch->stopped.load(std::memory_order_acquire)) {
            return;
        }
        if (error) {
            if (error != asio::error::operation_aborted) {
                notify_disconnect(watch);
            }
            return;
        }
        char byte = 0;
        asio::error_code read_error;
        const auto count = socket->receive(asio::buffer(&byte, 1),
                                           Tcp::socket::message_peek, read_error);
        if (read_error == asio::error::would_block ||
            read_error == asio::error::try_again) {
            watch_for_disconnect(socket, watch);
            return;
        }
        (void)count;
        notify_disconnect(watch);
    });
}

std::string reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 500: return "Internal Server Error";
        case 408: return "Request Timeout";
        case 431: return "Request Header Fields Too Large";
        default: return "Error";
    }
}

class SocketSseStream final : public SseStream {
public:
    SocketSseStream(std::shared_ptr<Tcp::socket> socket,
                    std::shared_ptr<DisconnectWatch> disconnect_watch,
                    std::size_t max_queue_bytes,
                    std::chrono::milliseconds keep_alive_interval)
        : socket_(std::move(socket)), disconnect_watch_(std::move(disconnect_watch)),
          max_queue_bytes_(max_queue_bytes), keep_alive_interval_(keep_alive_interval),
          writer_([this] { write_loop(); }) {}

    ~SocketSseStream() override { close(); }

    void write_event(std::string_view data) override {
        std::string payload;
        std::size_t offset = 0;
        while (offset <= data.size()) {
            const auto end = data.find('\n', offset);
            payload += "data: ";
            payload.append(data.substr(offset, end == std::string_view::npos
                                                   ? data.size() - offset
                                                   : end - offset));
            payload += "\r\n";
            if (end == std::string_view::npos) {
                break;
            }
            offset = end + 1;
        }
        payload += "\r\n";
        enqueue(make_chunk(payload));
    }

    void close() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!closing_) {
                closing_ = true;
                cv_.notify_one();
            }
        }
        if (writer_.joinable() && writer_.get_id() != std::this_thread::get_id()) {
            writer_.join();
        }
    }

private:
    static std::string make_chunk(const std::string& payload) {
        std::ostringstream size;
        size << std::hex << payload.size() << "\r\n";
        return size.str() + payload + "\r\n";
    }

    void enqueue(std::string bytes) {
        bool overflow = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closing_ || failed_) {
                throw std::runtime_error("SSE stream is closed");
            }
            if (bytes.size() > max_queue_bytes_ -
                                   std::min(max_queue_bytes_, queued_bytes_)) {
                failed_ = true;
                closing_ = true;
                overflow = true;
            } else {
                queued_bytes_ += bytes.size();
                queue_.push(std::move(bytes));
            }
        }
        cv_.notify_one();
        if (overflow) {
            notify_disconnect(disconnect_watch_);
            asio::error_code ignored;
            socket_->shutdown(Tcp::socket::shutdown_both, ignored);
            socket_->close(ignored);
            throw std::runtime_error("SSE outbound queue limit exceeded");
        }
    }

    bool write_bytes(const std::string& bytes) {
        asio::error_code error;
        asio::write(*socket_, asio::buffer(bytes), error);
        if (error) {
            CPPMCP_LOG_WARN("SSE response write failed: " << error.message());
            failed_ = true;
            notify_disconnect(disconnect_watch_);
            return false;
        }
        return true;
    }

    void write_loop() {
        for (;;) {
            std::string bytes;
            bool finish = false;
            bool keep_alive = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, keep_alive_interval_,
                             [this] { return !queue_.empty() || closing_ || failed_; });
                if (failed_) {
                    return;
                }
                if (!queue_.empty()) {
                    bytes = std::move(queue_.front());
                    queue_.pop();
                    queued_bytes_ -= bytes.size();
                } else if (closing_) {
                    finish = true;
                } else {
                    keep_alive = true;
                }
            }
            if (finish) {
                write_bytes("0\r\n\r\n");
                return;
            }
            if (keep_alive) {
                bytes = make_chunk(": keep-alive\r\n\r\n");
            }
            if (!write_bytes(bytes)) {
                return;
            }
        }
    }

    std::shared_ptr<Tcp::socket> socket_;
    std::shared_ptr<DisconnectWatch> disconnect_watch_;
    const std::size_t max_queue_bytes_;
    const std::chrono::milliseconds keep_alive_interval_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::string> queue_;
    std::size_t queued_bytes_ = 0;
    bool closing_ = false;
    std::atomic<bool> failed_{false};
    std::thread writer_;
};

class SocketExchange final : public IHttpExchange {
public:
    SocketExchange(std::shared_ptr<Tcp::socket> socket, HttpRequest request,
                   bool keep_alive, const HttpOptions& options)
        : socket_(std::move(socket)), request_(std::move(request)),
          keep_alive_(keep_alive), options_(options) {}

    ~SocketExchange() override {
        if (disconnect_watch_) {
            disconnect_watch_->stopped.store(true, std::memory_order_release);
            asio::error_code ignored;
            socket_->cancel(ignored);
        }
    }

    const HttpRequest& request() const override { return request_; }

    void respond(int status, Headers headers, std::string body) override {
        if (responded_) {
            return;
        }
        responded_ = true;
        headers.emplace_back("Content-Length", std::to_string(body.size()));
        headers.emplace_back("Connection", keep_alive_ ? "keep-alive" : "close");
        std::string response = "HTTP/1.1 " + std::to_string(status) + " " +
                               reason_phrase(status) + "\r\n";
        for (const auto& [name, value] : headers) {
            response += name + ": " + value + "\r\n";
        }
        response += "\r\n";
        response += body;
        write(std::move(response));
    }

    SseStream& begin_sse(int status, Headers headers) override {
        if (responded_) {
            throw std::logic_error("HTTP exchange already has a response");
        }
        responded_ = true;
        headers.emplace_back("Transfer-Encoding", "chunked");
        headers.emplace_back("Connection", keep_alive_ ? "keep-alive" : "close");
        std::string response = "HTTP/1.1 " + std::to_string(status) + " " +
                               reason_phrase(status) + "\r\n";
        for (const auto& [name, value] : headers) {
            response += name + ": " + value + "\r\n";
        }
        response += "\r\n";
        write(std::move(response));
        stream_ = std::make_unique<SocketSseStream>(
            socket_, disconnect_watch_, options_.sse_queue_max_bytes,
            options_.sse_keep_alive_interval);
        return *stream_;
    }

    void on_disconnect(std::function<void()> callback) override {
        disconnect_watch_ = std::make_shared<DisconnectWatch>();
        disconnect_watch_->callback = std::move(callback);
        watch_for_disconnect(socket_, disconnect_watch_);
    }

private:
    void write(std::string bytes) {
        asio::error_code error;
        asio::write(*socket_, asio::buffer(bytes), error);
        if (error) {
            CPPMCP_LOG_WARN("HTTP response write failed: " << error.message());
            if (disconnect_watch_) {
                notify_disconnect(disconnect_watch_);
            }
        }
    }

    std::shared_ptr<Tcp::socket> socket_;
    HttpRequest request_;
    bool keep_alive_;
    const HttpOptions& options_;
    bool responded_ = false;
    std::shared_ptr<DisconnectWatch> disconnect_watch_;
    std::unique_ptr<SocketSseStream> stream_;
};

bool connection_wants_keep_alive(const ParseState& state, const llhttp_t& parser) {
    if (parser.http_major > 1 || (parser.http_major == 1 && parser.http_minor >= 1)) {
        for (const auto& [name, value] : state.request.headers) {
            if (name == "Connection" || name == "connection") {
                return value != "close";
            }
        }
        return true;
    }
    for (const auto& [name, value] : state.request.headers) {
        if ((name == "Connection" || name == "connection") && value == "keep-alive") {
            return true;
        }
    }
    return false;
}

void send_bad_request(const std::shared_ptr<Tcp::socket>& socket, int status) {
    const auto response = "HTTP/1.1 " + std::to_string(status) + " " +
                          reason_phrase(status) +
                          "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    asio::error_code ignored;
    asio::write(*socket, asio::buffer(response), ignored);
    socket->shutdown(Tcp::socket::shutdown_both, ignored);
    socket->close(ignored);
}

void serve_connection(const std::shared_ptr<Tcp::socket>& socket,
                      const McpHttpHandler& handler, const HttpOptions& options) {
    ParseState state;
    state.max_header_bytes = options.max_header_bytes;
    state.max_body_bytes = options.max_body_bytes;
    asio::error_code endpoint_error;
    const auto endpoint = socket->remote_endpoint(endpoint_error);
    if (!endpoint_error) {
        state.request.peer.remote_address = endpoint.address().to_string();
    }

    auto settings = parser_settings();
    llhttp_t parser;
    llhttp_init(&parser, HTTP_REQUEST, &settings);
    parser.data = &state;
    socket->non_blocking(true);
    std::array<char, 8192> buffer{};
    std::string pending;
    bool request_started = false;
    auto deadline = std::chrono::steady_clock::now() + options.idle_timeout;

    while (socket->is_open()) {
        if (pending.empty()) {
            asio::error_code error;
            const auto count = socket->read_some(asio::buffer(buffer), error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    if (request_started) {
                        send_bad_request(socket, 408);
                    }
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            if (error || count == 0) {
                break;
            }
            if (!request_started) {
                request_started = true;
                deadline = std::chrono::steady_clock::now() + options.header_timeout;
            }
            pending.assign(buffer.data(), count);
        }

        state.complete = false;
        const bool headers_were_complete = state.headers_complete;
        const auto parsed = llhttp_execute(&parser, pending.data(), pending.size());
        if (parsed != HPE_OK && parsed != HPE_PAUSED) {
            send_bad_request(socket, state.rejection_status != 0
                                         ? state.rejection_status
                                         : 400);
            break;
        }
        if (!headers_were_complete && state.headers_complete) {
            deadline = std::chrono::steady_clock::now() + options.body_timeout;
        }
        if (!state.complete) {
            pending.clear();
            continue;
        }

        const auto* consumed_position = llhttp_get_error_pos(&parser);
        const auto consumed = consumed_position == nullptr
                                  ? pending.size()
                                  : static_cast<std::size_t>(consumed_position - pending.data());
        std::string remainder = consumed <= pending.size()
                                    ? pending.substr(consumed)
                                    : std::string{};
        const bool keep_alive = connection_wants_keep_alive(state, parser);
        bool route_found = true;
        if (options.route) {
            const auto routed_target = options.route(state.request);
            route_found = routed_target.has_value();
            if (routed_target) {
                state.request.target = *routed_target;
            }
        }
        auto exchange = std::make_shared<SocketExchange>(socket, std::move(state.request),
                                                          keep_alive, options);
        socket->non_blocking(false);
        try {
            std::vector<std::string_view> authorization;
            for (const auto& [name, value] : exchange->request().headers) {
                std::string normalized = name;
                std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                               [](unsigned char ch) {
                                   return static_cast<char>(std::tolower(ch));
                               });
                if (normalized == "authorization") {
                    authorization.push_back(value);
                }
            }
            if (!route_found) {
                exchange->respond(404, {}, "");
            } else if (!options.bearer_token.empty() &&
                       (authorization.size() != 1 ||
                        authorization.front() != "Bearer " + options.bearer_token)) {
                exchange->respond(401, {{"WWW-Authenticate", "Bearer"}}, "");
            } else {
                handler.handle(exchange);
            }
        } catch (const std::exception& error) {
            CPPMCP_LOG_ERROR("HTTP request handler failed: " << error.what());
            if (!exchange->request().method.empty()) {
                exchange->respond(500, {{"Content-Type", "text/plain"}}, "Internal Server Error");
            }
        }
        if (!keep_alive || !socket->is_open()) {
            break;
        }

        state = ParseState{};
        state.max_header_bytes = options.max_header_bytes;
        state.max_body_bytes = options.max_body_bytes;
        if (!endpoint_error) {
            state.request.peer.remote_address = endpoint.address().to_string();
        }
        llhttp_init(&parser, HTTP_REQUEST, &settings);
        parser.data = &state;
        pending = std::move(remainder);
        request_started = !pending.empty();
        deadline = std::chrono::steady_clock::now() +
                   (request_started ? options.header_timeout : options.idle_timeout);
        socket->non_blocking(true);
    }
    asio::error_code ignored;
    socket->shutdown(Tcp::socket::shutdown_both, ignored);
    socket->close(ignored);
}

std::atomic<bool> signal_shutdown{false};

void handle_signal(int) {
    signal_shutdown.store(true, std::memory_order_relaxed);
}

}  // namespace

struct AsioExecutor::Impl {
    explicit Impl(std::size_t workers) : pool(workers == 0 ? 1 : workers) {}
    asio::thread_pool pool;
};

AsioExecutor::AsioExecutor(std::size_t workers)
    : impl_(std::make_unique<Impl>(workers)) {}

AsioExecutor::~AsioExecutor() = default;

void AsioExecutor::post(std::function<void()> task) {
    if (task) {
        asio::post(impl_->pool, std::move(task));
    }
}

struct HttpServerHandle::Impl {
    Impl(Server& server, const HttpOptions& server_options)
        : options(server_options), handler(server, options.handler), acceptor(context) {
        const auto address = asio::ip::make_address(options.bind_address);
        if (!address.is_loopback() && !options.allow_non_loopback) {
            throw std::invalid_argument(
                "non-loopback HTTP bind requires allow_non_loopback=true");
        }
        if (options.max_header_bytes == 0 || options.max_body_bytes == 0 ||
            options.sse_queue_max_bytes == 0 ||
            options.header_timeout.count() <= 0 || options.body_timeout.count() <= 0 ||
            options.idle_timeout.count() <= 0 ||
            options.sse_keep_alive_interval.count() <= 0) {
            throw std::invalid_argument("HTTP limits and timeouts must be positive");
        }
        Tcp::endpoint endpoint(address, options.port);
        acceptor.open(endpoint.protocol());
        acceptor.set_option(Tcp::acceptor::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen();
        acceptor.non_blocking(true);
        bound_port = acceptor.local_endpoint().port();
        context_thread = std::thread([this] { context.run(); });
        accept_thread = std::thread([this] { accept_loop(); });
    }

    ~Impl() { stop(); }

    void accept_loop() {
        while (!stopping.load(std::memory_order_acquire)) {
            auto socket = std::make_shared<Tcp::socket>(context);
            asio::error_code error;
            acceptor.accept(*socket, error);
            if (!error) {
                {
                    std::lock_guard<std::mutex> lock(workers_mutex);
                    if (stopping.load(std::memory_order_acquire)) {
                        socket->close(error);
                        break;
                    }
                    active_sockets.push_back(socket);
                    ++active_connections;
                }
                try {
                    std::thread([this, socket] {
                        try {
                            serve_connection(socket, handler, options);
                        } catch (const std::exception& error) {
                            CPPMCP_LOG_ERROR("HTTP connection failed: " << error.what());
                            asio::error_code ignored;
                            socket->close(ignored);
                        }
                        {
                            std::lock_guard<std::mutex> worker_lock(workers_mutex);
                            active_sockets.erase(
                                std::remove(active_sockets.begin(), active_sockets.end(), socket),
                                active_sockets.end());
                            --active_connections;
                            workers_cv.notify_all();
                        }
                    }).detach();
                } catch (const std::exception& error) {
                    CPPMCP_LOG_ERROR("could not start HTTP connection worker: " << error.what());
                    asio::error_code ignored;
                    socket->close(ignored);
                    std::lock_guard<std::mutex> lock(workers_mutex);
                    active_sockets.erase(
                        std::remove(active_sockets.begin(), active_sockets.end(), socket),
                        active_sockets.end());
                    --active_connections;
                    workers_cv.notify_all();
                }
            } else if (error == asio::error::would_block ||
                       error == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } else if (!stopping.load(std::memory_order_acquire)) {
                CPPMCP_LOG_ERROR("HTTP accept failed: " << error.message());
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    }

    void stop() {
        if (stopping.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        if (accept_thread.joinable()) {
            accept_thread.join();
        }
        asio::error_code ignored;
        acceptor.close(ignored);
        {
            std::lock_guard<std::mutex> lock(workers_mutex);
            for (const auto& socket : active_sockets) {
                socket->shutdown(Tcp::socket::shutdown_both, ignored);
                socket->close(ignored);
            }
        }
        {
            std::unique_lock<std::mutex> lock(workers_mutex);
            workers_cv.wait(lock, [this] { return active_connections == 0; });
        }
        active_sockets.clear();
        work_guard.reset();
        context.stop();
        if (context_thread.joinable()) {
            context_thread.join();
        }
    }

    asio::io_context context;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_guard{
        asio::make_work_guard(context)};
    HttpOptions options;
    McpHttpHandler handler;
    Tcp::acceptor acceptor;
    std::atomic<bool> stopping{false};
    std::thread accept_thread;
    std::thread context_thread;
    std::mutex workers_mutex;
    std::condition_variable workers_cv;
    std::size_t active_connections = 0;
    std::vector<std::shared_ptr<Tcp::socket>> active_sockets;
    unsigned short bound_port = 0;
};

HttpServerHandle::HttpServerHandle(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

HttpServerHandle::~HttpServerHandle() = default;

HttpServerHandle::HttpServerHandle(HttpServerHandle&&) noexcept = default;

HttpServerHandle& HttpServerHandle::operator=(HttpServerHandle&&) noexcept = default;

void HttpServerHandle::stop() {
    if (impl_) {
        impl_->stop();
    }
}

void HttpServerHandle::wait() {
    if (!impl_) {
        return;
    }
    while (!impl_->stopping.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

unsigned short HttpServerHandle::port() const noexcept {
    return impl_ ? impl_->bound_port : 0;
}

}  // namespace cppmcp::transport

namespace cppmcp {

std::unique_ptr<transport::HttpServerHandle> Server::start_http(
    const transport::HttpOptions& options) {
    auto impl = std::make_unique<transport::HttpServerHandle::Impl>(*this, options);
    return std::unique_ptr<transport::HttpServerHandle>(new transport::HttpServerHandle(
        std::move(impl)));
}

void Server::run_http(const transport::HttpOptions& options) {
    transport::signal_shutdown.store(false, std::memory_order_relaxed);
    const auto previous_int = std::signal(SIGINT, transport::handle_signal);
    const auto previous_term = std::signal(SIGTERM, transport::handle_signal);
    auto handle = start_http(options);
    CPPMCP_LOG_INFO("HTTP server listening on " << options.bind_address << ":"
                                                 << handle->port());
    while (!transport::signal_shutdown.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    handle->stop();
    std::signal(SIGINT, previous_int);
    std::signal(SIGTERM, previous_term);
}

}  // namespace cppmcp
