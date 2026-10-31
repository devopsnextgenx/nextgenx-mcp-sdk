#pragma once

// A small HTTP/1.1 listener that sits in front of cppmcp's McpHttpHandler.
//
// Why it exists: Server::run_http() owns the listener and routes every request into
// Server::handle_message, and the public Server API has no hook for prompts / resources /
// completions / tasks. This front-end uses only public interfaces:
//
//   * POST bodies holding a single JSON-RPC request are offered to a `FeatureHook`
//     (FeatureRegistry::try_handle). If it answers, the reply is plain application/json.
//   * Everything else (tools, discover, GET listen streams, DELETE, batches ...) is handed
//     unchanged to `McpHttpHandler::handle` through an `IHttpExchange` implemented here.
//   * Result payloads that look like a discover/initialize result are passed through a
//     `CapabilityPatch` so the new features are advertised.
//
// Scope: demo-grade. POSIX sockets, one thread per connection, `Connection: close`,
// no TLS and no chunked *request* bodies. Use Server::run_http() when you need those.

#if defined(_WIN32)
#error "http_frontend.hpp is a POSIX demo listener; on Windows use Server::run_http()."
#endif

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "cppmcp/core/json_rpc.hpp"
#include "cppmcp/core/log.hpp"
#include "cppmcp/transport/http.hpp"

namespace cppmcp::examples {

using Json = nlohmann::json;

struct FrontendOptions {
    std::string bind_address = "127.0.0.1";
    unsigned short port = 8000;
    std::string path = "/mcp";
    std::string bearer_token;
    std::size_t max_header_bytes = 32 * 1024;
    std::size_t max_body_bytes = 16 * 1024 * 1024;
    std::chrono::milliseconds header_timeout{5000};
    std::chrono::milliseconds body_timeout{10000};
};

using FeatureHook = std::function<std::optional<core::Message>(const core::Request&)>;
using CapabilityPatch = std::function<void(Json& result)>;
using ExchangeHandler = std::function<void(const std::shared_ptr<transport::IHttpExchange>&)>;

namespace frontend_detail {

inline const char* reason(int status) {
    switch (status) {
        case 100: return "Continue";
        case 200: return "OK";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 431: return "Request Header Fields Too Large";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return status >= 500 ? "Internal Server Error" : "OK";
    }
}

inline std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::optional<std::string> header_value(const transport::Headers& headers,
                                               std::string_view name) {
    const auto wanted = lower(std::string(name));
    for (const auto& [key, value] : headers) {
        if (lower(key) == wanted) return value;
    }
    return std::nullopt;
}

inline bool constant_time_equals(std::string_view a, std::string_view b) {
    unsigned char diff = static_cast<unsigned char>(a.size() ^ b.size());
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

// Blocking send of the whole buffer, bounded by `timeout` of no progress.
inline bool send_all(int fd, std::string_view data, std::chrono::milliseconds timeout) {
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto n = ::send(fd, data.data() + sent, data.size() - sent, flags);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd p{fd, POLLOUT, 0};
            if (::poll(&p, 1, static_cast<int>(timeout.count())) > 0) continue;
        }
        return false;
    }
    return true;
}

inline std::string format_head(int status, const transport::Headers& headers, bool chunked,
                               std::optional<std::size_t> content_length) {
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason(status) + "\r\n";
    bool has_connection = false;
    for (const auto& [key, value] : headers) {
        const auto k = lower(key);
        if (k == "content-length" || k == "transfer-encoding") continue;  // we set our own framing
        has_connection = has_connection || k == "connection";
        head += key + ": " + value + "\r\n";
    }
    if (!has_connection) head += "Connection: close\r\n";
    if (chunked) head += "Transfer-Encoding: chunked\r\n";
    else if (content_length) head += "Content-Length: " + std::to_string(*content_length) + "\r\n";
    head += "\r\n";
    return head;
}

inline void write_response(int fd, int status, const transport::Headers& headers,
                           std::string_view body) {
    send_all(fd, format_head(status, headers, false, body.size()), std::chrono::seconds(5));
    if (!body.empty()) send_all(fd, body, std::chrono::seconds(5));
}

}  // namespace frontend_detail

// One accepted connection. Shared with McpHttpHandler, which may answer from another
// thread, so the fd is closed only in the destructor and writes are guarded by `closed_`.
class SocketExchange final : public transport::IHttpExchange,
                             public std::enable_shared_from_this<SocketExchange> {
public:
    SocketExchange(int fd, transport::HttpRequest request, CapabilityPatch patch)
        : fd_(fd), request_(std::move(request)), patch_(std::move(patch)) {}
    ~SocketExchange() override {
        if (fd_ >= 0) ::close(fd_);
    }

    const transport::HttpRequest& request() const override { return request_; }

    void respond(int status, transport::Headers headers, std::string body) override {
        std::lock_guard lock(mutex_);
        if (closed_ || sse_started_ || responded_) return;
        responded_ = true;
        patch_body(body);
        if (!write_locked(frontend_detail::format_head(status, headers, false, body.size())) ||
            (!body.empty() && !write_locked(body))) {
            // write_locked already marked the exchange as disconnected
        }
        finish_locked();
    }

    transport::SseStream& begin_sse(int status, transport::Headers headers) override {
        std::lock_guard lock(mutex_);
        if (!sse_) sse_ = std::make_unique<Sse>(*this);
        if (!closed_ && !sse_started_ && !responded_) {
            sse_started_ = true;
            write_locked(frontend_detail::format_head(status, headers, true, std::nullopt));
        }
        return *sse_;
    }

    void on_disconnect(std::function<void()> callback) override {
        bool fire_now = false;
        {
            std::lock_guard lock(mutex_);
            if (disconnected_) fire_now = true;
            else disconnect_callbacks_.push_back(std::move(callback));
        }
        if (fire_now && callback) callback();
    }

    // ---- used by the connection thread ----
    int fd() const noexcept { return fd_; }
    bool finished() const {
        std::lock_guard lock(mutex_);
        return finished_;
    }
    bool wait_finished(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return finished_; });
    }
    void notify_disconnect() {
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard lock(mutex_);
            if (disconnected_) return;
            disconnected_ = true;
            callbacks.swap(disconnect_callbacks_);
        }
        for (auto& cb : callbacks) {
            if (cb) cb();
        }
    }
    // Stop further writes and unblock the peer; the fd itself is closed in the destructor.
    void abort() {
        std::lock_guard lock(mutex_);
        if (!closed_) {
            closed_ = true;
            ::shutdown(fd_, SHUT_RDWR);
        }
        finished_ = true;
        cv_.notify_all();
    }

private:
    class Sse final : public transport::SseStream {
    public:
        explicit Sse(SocketExchange& owner) : owner_(owner) {}
        void write_event(std::string_view data) override {
            std::string payload(data);
            owner_.patch_body(payload);
            std::string frame = "event: message\n";
            std::size_t start = 0;
            for (;;) {  // each payload line needs its own "data:" prefix
                const auto nl = payload.find('\n', start);
                frame += "data: " + payload.substr(start, nl == std::string::npos ? nl : nl - start) + "\n";
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
            frame += "\n";
            std::lock_guard lock(owner_.mutex_);
            owner_.write_chunk_locked(frame);
        }
        void write_comment(std::string_view text) override {
            std::lock_guard lock(owner_.mutex_);
            owner_.write_chunk_locked(": " + std::string(text) + "\n\n");
        }
        void close() override {
            std::lock_guard lock(owner_.mutex_);
            if (!owner_.closed_ && !owner_.finished_) owner_.write_locked("0\r\n\r\n");
            owner_.finish_locked();
        }

    private:
        SocketExchange& owner_;
    };

    void patch_body(std::string& body) {
        if (!patch_ || body.find("\"capabilities\"") == std::string::npos) return;
        auto json = Json::parse(body, nullptr, false);
        if (json.is_discarded() || !json.is_object() || !json.contains("result")) return;
        patch_(json["result"]);
        body = json.dump();
    }

    bool write_locked(std::string_view data) {
        if (closed_) return false;
        if (!frontend_detail::send_all(fd_, data, std::chrono::seconds(5))) {
            closed_ = true;
            mark_disconnected_locked();
            return false;
        }
        return true;
    }
    bool write_chunk_locked(std::string_view data) {
        if (closed_ || !sse_started_) return false;
        char size[32];
        std::snprintf(size, sizeof size, "%zx\r\n", data.size());
        return write_locked(std::string(size) + std::string(data) + "\r\n");
    }
    void mark_disconnected_locked() {
        // Callbacks may call back into us; defer to the connection thread's poll loop.
        disconnected_pending_ = true;
    }
    void finish_locked() {
        finished_ = true;
        cv_.notify_all();
    }

    int fd_;
    transport::HttpRequest request_;
    CapabilityPatch patch_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unique_ptr<Sse> sse_;
    std::vector<std::function<void()>> disconnect_callbacks_;
    bool closed_ = false, responded_ = false, sse_started_ = false, finished_ = false;
    bool disconnected_ = false, disconnected_pending_ = false;

public:
    bool take_pending_disconnect() {
        std::lock_guard lock(mutex_);
        return std::exchange(disconnected_pending_, false);
    }
};

class HttpFrontend {
public:
    HttpFrontend(FrontendOptions options, FeatureHook hook, CapabilityPatch patch,
                 ExchangeHandler fallback)
        : options_(std::move(options)), hook_(std::move(hook)), patch_(std::move(patch)),
          fallback_(std::move(fallback)) {}
    ~HttpFrontend() { stop(); }
    HttpFrontend(const HttpFrontend&) = delete;
    HttpFrontend& operator=(const HttpFrontend&) = delete;

    void start() {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        addrinfo* found = nullptr;
        const auto port = std::to_string(options_.port);
        if (const int rc = ::getaddrinfo(options_.bind_address.c_str(), port.c_str(), &hints, &found);
            rc != 0) {
            throw std::runtime_error("cannot resolve bind address '" + options_.bind_address +
                                     "': " + ::gai_strerror(rc));
        }
        std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard(found, ::freeaddrinfo);
        listen_fd_ = ::socket(found->ai_family, found->ai_socktype, found->ai_protocol);
        if (listen_fd_ < 0) throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
        const int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (::bind(listen_fd_, found->ai_addr, found->ai_addrlen) != 0 || ::listen(listen_fd_, 128) != 0) {
            const std::string message = std::strerror(errno);
            ::close(listen_fd_);
            listen_fd_ = -1;
            throw std::runtime_error("bind/listen on " + options_.bind_address + ":" + port + ": " + message);
        }
        sockaddr_storage bound{};
        socklen_t length = sizeof bound;
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &length);
        port_ = bound.ss_family == AF_INET6
                    ? ntohs(reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port)
                    : ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
        acceptor_ = std::thread([this] { accept_loop(); });
        CPPMCP_LOG_INFO("http front-end listening on " << options_.bind_address << ':' << port_
                                                       << options_.path);
    }

    void stop() {
        if (stopping_.exchange(true)) return;
        if (acceptor_.joinable()) acceptor_.join();
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (active_.load() > 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (active_.load() > 0) CPPMCP_LOG_WARN("stopping with " << active_.load() << " live connection(s)");
    }

    [[nodiscard]] unsigned short port() const noexcept { return port_; }

private:
    void accept_loop() {
        while (!stopping_.load()) {
            pollfd p{listen_fd_, POLLIN, 0};
            if (::poll(&p, 1, 200) <= 0) continue;
            sockaddr_storage peer{};
            socklen_t length = sizeof peer;
            const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &length);
            if (fd < 0) continue;
            char host[INET6_ADDRSTRLEN] = "";
            if (peer.ss_family == AF_INET6) {
                ::inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(&peer)->sin6_addr, host, sizeof host);
            } else {
                ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&peer)->sin_addr, host, sizeof host);
            }
#ifdef SO_NOSIGPIPE
            ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one_, sizeof one_);
#endif
            active_.fetch_add(1);
            std::thread([this, fd, remote = std::string(host)] {
                try {
                    serve(fd, remote);
                } catch (const std::exception& error) {
                    CPPMCP_LOG_WARN("connection error: " << error.what());
                }
                active_.fetch_sub(1);
            }).detach();
        }
    }

    static bool wait_readable(int fd, std::chrono::milliseconds timeout) {
        pollfd p{fd, POLLIN, 0};
        return ::poll(&p, 1, static_cast<int>(timeout.count())) > 0;
    }

    // Reads until "\r\n\r\n". Returns false (after replying) on protocol problems.
    bool read_head(int fd, std::string& head, std::string& leftover) {
        std::string buffer;
        const auto deadline = std::chrono::steady_clock::now() + options_.header_timeout;
        char chunk[4096];
        for (;;) {
            if (const auto end = buffer.find("\r\n\r\n"); end != std::string::npos) {
                head = buffer.substr(0, end);
                leftover = buffer.substr(end + 4);
                return true;
            }
            if (buffer.size() > options_.max_header_bytes) {
                frontend_detail::write_response(fd, 431, {}, "");
                return false;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0 || stopping_.load() || !wait_readable(fd, remaining)) return false;
            const auto n = ::recv(fd, chunk, sizeof chunk, 0);
            if (n <= 0) return false;
            buffer.append(chunk, static_cast<std::size_t>(n));
        }
    }

    bool parse_head(const std::string& head, transport::HttpRequest& request) {
        const auto line_end = head.find("\r\n");
        const auto request_line = head.substr(0, line_end);
        const auto sp1 = request_line.find(' ');
        const auto sp2 = request_line.rfind(' ');
        if (sp1 == std::string::npos || sp2 == sp1) return false;
        request.method = request_line.substr(0, sp1);
        request.target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
        std::size_t pos = line_end == std::string::npos ? head.size() : line_end + 2;
        while (pos < head.size()) {
            auto next = head.find("\r\n", pos);
            if (next == std::string::npos) next = head.size();
            const auto line = head.substr(pos, next - pos);
            const auto colon = line.find(':');
            if (colon != std::string::npos && colon > 0) {
                auto value = line.substr(colon + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                value.erase(value.find_last_not_of(" \t") + 1);
                request.headers.emplace_back(line.substr(0, colon), std::move(value));
            }
            pos = next + 2;
        }
        return true;
    }

    static std::optional<core::Request> as_request(const Json& json) {
        if (!json.is_object() || !json.contains("method") || !json["method"].is_string() ||
            !json.contains("id")) {
            return std::nullopt;
        }
        core::Request request;
        if (json["id"].is_string()) request.id = json["id"].get<std::string>();
        else if (json["id"].is_number_integer()) request.id = json["id"].get<std::int64_t>();
        else return std::nullopt;
        request.method = json["method"].get<std::string>();
        request.params = json.value("params", Json::object());
        if (!request.params.is_object()) request.params = Json::object();
        return request;
    }

    void serve(int fd, const std::string& remote) {
        std::string head, leftover;
        transport::HttpRequest request;
        request.peer.remote_address = remote;
        if (!read_head(fd, head, leftover)) {
            ::close(fd);
            return;
        }
        if (!parse_head(head, request)) {
            frontend_detail::write_response(fd, 400, {}, "");
            ::close(fd);
            return;
        }

        const auto path = request.target.substr(0, request.target.find('?'));
        if (!options_.bearer_token.empty()) {
            const auto auth = frontend_detail::header_value(request.headers, "authorization");
            const std::string prefix = "Bearer ";
            if (!auth || auth->rfind(prefix, 0) != 0 ||
                !frontend_detail::constant_time_equals(auth->substr(prefix.size()), options_.bearer_token)) {
                frontend_detail::write_response(
                    fd, 401, {{"WWW-Authenticate", "Bearer"}, {"Content-Type", "text/plain"}}, "unauthorized");
                ::close(fd);
                return;
            }
        }
        if (path != options_.path) {
            frontend_detail::write_response(fd, 404, {{"Content-Type", "text/plain"}}, "not found");
            ::close(fd);
            return;
        }

        if (frontend_detail::header_value(request.headers, "transfer-encoding")) {
            frontend_detail::write_response(fd, 501, {}, "chunked request bodies are not supported");
            ::close(fd);
            return;
        }
        std::size_t content_length = 0;
        if (const auto value = frontend_detail::header_value(request.headers, "content-length")) {
            const auto parsed = std::from_chars(value->data(), value->data() + value->size(), content_length);
            if (parsed.ec != std::errc{} || parsed.ptr != value->data() + value->size()) {
                frontend_detail::write_response(fd, 400, {}, "bad content-length");
                ::close(fd);
                return;
            }
        }
        if (content_length > options_.max_body_bytes) {
            frontend_detail::write_response(fd, 413, {}, "");
            ::close(fd);
            return;
        }
        if (content_length > leftover.size()) {
            if (const auto expect = frontend_detail::header_value(request.headers, "expect");
                expect && frontend_detail::lower(*expect) == "100-continue") {
                frontend_detail::send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", std::chrono::seconds(5));
            }
        }
        request.body = std::move(leftover);
        const auto body_deadline = std::chrono::steady_clock::now() + options_.body_timeout;
        char chunk[8192];
        while (request.body.size() < content_length) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                body_deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0 || stopping_.load() || !wait_readable(fd, remaining)) {
                ::close(fd);
                return;
            }
            const auto n = ::recv(fd, chunk, std::min(sizeof chunk, content_length - request.body.size()), 0);
            if (n <= 0) {
                ::close(fd);
                return;
            }
            request.body.append(chunk, static_cast<std::size_t>(n));
        }
        request.body.resize(content_length);

        // 1. Feature methods are answered here.
        if (request.method == "POST" && hook_ && !request.body.empty()) {
            const auto json = Json::parse(request.body, nullptr, false);
            if (const auto rpc = json.is_discarded() ? std::nullopt : as_request(json)) {
                std::optional<core::Message> reply;
                try {
                    reply = hook_(*rpc);
                } catch (const std::exception& error) {
                    reply = core::ErrorResponse{
                        rpc->id, core::RpcError{static_cast<std::int32_t>(core::ErrorCode::internal_error),
                                                error.what(), std::nullopt}};
                }
                if (reply) {
                    frontend_detail::write_response(fd, 200, {{"Content-Type", "application/json"}},
                                                    core::serialize_message(*reply));
                    ::close(fd);
                    return;
                }
            }
        }

        // 2. Everything else belongs to the SDK.
        auto exchange = std::make_shared<SocketExchange>(fd, std::move(request), patch_);
        try {
            fallback_(exchange);
        } catch (const std::exception& error) {
            CPPMCP_LOG_WARN("handler threw: " << error.what());
            exchange->respond(500, {{"Content-Type", "text/plain"}}, "internal error");
        }
        await(*exchange);
        exchange->abort();
    }

    // Keep the connection thread alive while the handler (possibly on another thread)
    // finishes the response; translate peer hang-ups into on_disconnect callbacks.
    void await(SocketExchange& exchange) {
        while (!exchange.finished() && !stopping_.load()) {
            if (exchange.take_pending_disconnect()) {
                exchange.notify_disconnect();
                exchange.wait_finished(std::chrono::seconds(2));
                return;
            }
            pollfd p{exchange.fd(), POLLIN, 0};
            if (::poll(&p, 1, 100) > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) {
                char byte;
                const auto n = ::recv(exchange.fd(), &byte, 1, MSG_PEEK | MSG_DONTWAIT);
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    exchange.notify_disconnect();
                    exchange.wait_finished(std::chrono::seconds(2));
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));  // unread pipelined bytes
            }
        }
    }

    FrontendOptions options_;
    FeatureHook hook_;
    CapabilityPatch patch_;
    ExchangeHandler fallback_;
    int listen_fd_ = -1;
    unsigned short port_ = 0;
    std::thread acceptor_;
    std::atomic<bool> stopping_{false};
    std::atomic<int> active_{0};
#ifdef SO_NOSIGPIPE
    int one_ = 1;
#endif
};

}  // namespace cppmcp::examples
