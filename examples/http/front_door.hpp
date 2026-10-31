#pragma once

// Minimal HTTP/1.1 listener (POSIX) for the example server.
//
// `Server::run_http` owns its listener and offers no hook to answer prompts/resources/
// completions/tasks. The public `McpHttpHandler::handle(IHttpExchange)` entry point does not
// need that listener, so this example accepts connections itself, tries the FeatureRegistry
// first, and otherwise hands the exchange to the SDK handler.
//
// Deliberately small: one thread per connection, Connection: close, request bodies via
// Content-Length only, no TLS. Use `--sdk-listener` for the SDK's full-featured listener.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "cppmcp/transport/http.hpp"

namespace cppmcp::examples {

namespace http_detail {

inline std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

inline const char* reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default: return "Status";
    }
}

inline bool write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const auto n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        if (n <= 0) return false;
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

}  // namespace http_detail

// Case-insensitive header lookup.
inline std::string header_value(const transport::Headers& headers, const std::string& name) {
    const auto wanted = http_detail::lower(name);
    for (const auto& [k, v] : headers) {
        if (http_detail::lower(k) == wanted) return v;
    }
    return {};
}

class PosixExchange final : public transport::IHttpExchange, public transport::SseStream {
public:
    PosixExchange(int fd, transport::HttpRequest request) : fd_(fd), request_(std::move(request)) {}

    // ---- IHttpExchange ----
    const transport::HttpRequest& request() const override { return request_; }

    void respond(int status, transport::Headers headers, std::string body) override {
        std::lock_guard lock(write_mutex_);
        if (head_sent_) return;
        head_sent_ = true;
        std::string out = "HTTP/1.1 " + std::to_string(status) + ' ' + http_detail::reason(status) + "\r\n";
        append_headers(out, headers);
        out += "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
        if (!http_detail::write_all(fd_, out) || !http_detail::write_all(fd_, body)) disconnected_ = true;
        finish();
    }

    transport::SseStream& begin_sse(int status, transport::Headers headers) override {
        std::lock_guard lock(write_mutex_);
        if (!head_sent_) {
            head_sent_ = true;
            std::string out = "HTTP/1.1 " + std::to_string(status) + ' ' + http_detail::reason(status) + "\r\n";
            bool has_type = false;
            for (const auto& [k, v] : headers) has_type = has_type || http_detail::lower(k) == "content-type";
            if (!has_type) out += "Content-Type: text/event-stream\r\n";
            append_headers(out, headers);
            out += "Cache-Control: no-cache\r\nConnection: close\r\n\r\n";  // body is close-delimited
            if (!http_detail::write_all(fd_, out)) disconnected_ = true;
        }
        return *this;
    }

    void on_disconnect(std::function<void()> callback) override {
        bool fire = false;
        {
            std::lock_guard lock(state_mutex_);
            if (disconnected_) fire = true;
            else callbacks_.push_back(std::move(callback));
        }
        if (fire) callback();
    }

    // ---- SseStream ----
    void write_event(std::string_view data) override {
        std::string frame = "event: message\n";
        std::size_t pos = 0;
        while (true) {
            const auto nl = data.find('\n', pos);
            frame += "data: ";
            frame += data.substr(pos, nl == std::string_view::npos ? nl : nl - pos);
            frame += '\n';
            if (nl == std::string_view::npos) break;
            pos = nl + 1;
        }
        frame += '\n';
        send_stream(frame);
    }
    void write_comment(std::string_view text) override { send_stream(": " + std::string(text) + "\n\n"); }
    void close() override { finish(); }

    // ---- connection loop support ----
    // Blocks until the response is complete or the peer goes away.
    void wait_until_done(std::chrono::milliseconds max_wait) {
        const auto deadline = std::chrono::steady_clock::now() + max_wait;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::unique_lock lock(state_mutex_);
                if (done_) return;
            }
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, 100) > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) {
                char c;
                if (::recv(fd_, &c, 1, MSG_PEEK | MSG_DONTWAIT) == 0) {
                    mark_disconnected();
                    return;
                }
            }
        }
    }

private:
    static void append_headers(std::string& out, const transport::Headers& headers) {
        for (const auto& [k, v] : headers) {
            const auto name = http_detail::lower(k);
            if (name == "content-length" || name == "connection" || name == "transfer-encoding") continue;
            out += k + ": " + v + "\r\n";
        }
    }

    void send_stream(const std::string& frame) {
        std::lock_guard lock(write_mutex_);
        if (!head_sent_ || disconnected_) return;
        if (!http_detail::write_all(fd_, frame)) mark_disconnected();
    }

    void finish() {
        {
            std::lock_guard lock(state_mutex_);
            done_ = true;
        }
    }

    void mark_disconnected() {
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard lock(state_mutex_);
            disconnected_ = true;
            done_ = true;
            callbacks.swap(callbacks_);
        }
        for (auto& cb : callbacks) cb();
    }

    int fd_;
    transport::HttpRequest request_;
    std::mutex write_mutex_;
    std::mutex state_mutex_;
    std::vector<std::function<void()>> callbacks_;
    bool head_sent_ = false;
    bool done_ = false;
    bool disconnected_ = false;
};

class FrontDoor {
public:
    using Handler = std::function<void(const std::shared_ptr<transport::IHttpExchange>&)>;

    FrontDoor(std::string bind_address, unsigned short port, Handler handler)
        : handler_(std::move(handler)) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) throw std::runtime_error("socket() failed");
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, bind_address.c_str(), &addr.sin_addr) != 1) {
            ::close(listen_fd_);
            throw std::invalid_argument("bind address must be an IPv4 literal: " + bind_address);
        }
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
            ::listen(listen_fd_, 64) != 0) {
            ::close(listen_fd_);
            throw std::runtime_error("cannot listen on " + bind_address + ':' + std::to_string(port));
        }
        socklen_t len = sizeof addr;
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        acceptor_ = std::thread([this] { accept_loop(); });
    }
    ~FrontDoor() { stop(); }
    FrontDoor(const FrontDoor&) = delete;
    FrontDoor& operator=(const FrontDoor&) = delete;

    unsigned short port() const noexcept { return port_; }

    void stop() {
        if (stopping_.exchange(true)) return;
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        if (acceptor_.joinable()) acceptor_.join();
        std::unique_lock lock(count_mutex_);
        count_cv_.wait_for(lock, std::chrono::seconds(5), [this] { return active_ == 0; });
    }

private:
    void accept_loop() {
        while (!stopping_) {
            sockaddr_in peer{};
            socklen_t len = sizeof peer;
            const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &len);
            if (fd < 0) {
                if (stopping_) return;
                continue;
            }
            char text[INET_ADDRSTRLEN] = {};
            ::inet_ntop(AF_INET, &peer.sin_addr, text, sizeof text);
            {
                std::lock_guard lock(count_mutex_);
                ++active_;
            }
            std::thread([this, fd, remote = std::string(text)] {
                serve(fd, remote);
                std::lock_guard lock(count_mutex_);
                --active_;
                count_cv_.notify_all();
            }).detach();
        }
    }

    static bool read_request(int fd, transport::HttpRequest& out, int& error_status) {
        constexpr std::size_t kMaxHeader = 32 * 1024, kMaxBody = 16 * 1024 * 1024;
        timeval tv{10, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        std::string buffer;
        std::size_t header_end = std::string::npos;
        char chunk[4096];
        while (header_end == std::string::npos) {
            const auto n = ::recv(fd, chunk, sizeof chunk, 0);
            if (n <= 0) return false;
            buffer.append(chunk, static_cast<std::size_t>(n));
            header_end = buffer.find("\r\n\r\n");
            if (header_end == std::string::npos && buffer.size() > kMaxHeader) {
                error_status = 413;
                return false;
            }
        }
        std::size_t pos = buffer.find("\r\n");
        const auto line = buffer.substr(0, pos);
        const auto sp1 = line.find(' ');
        const auto sp2 = line.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) {
            error_status = 400;
            return false;
        }
        out.method = line.substr(0, sp1);
        out.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
        pos += 2;
        while (pos < header_end) {
            const auto eol = buffer.find("\r\n", pos);
            const auto h = buffer.substr(pos, eol - pos);
            pos = eol + 2;
            const auto colon = h.find(':');
            if (colon == std::string::npos) continue;
            auto value = h.substr(colon + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            out.headers.emplace_back(h.substr(0, colon), value);
        }
        std::size_t length = 0;
        if (const auto cl = header_value(out.headers, "Content-Length"); !cl.empty()) {
            try {
                length = std::stoull(cl);
            } catch (...) {
                error_status = 400;
                return false;
            }
        }
        if (length > kMaxBody) {
            error_status = 413;
            return false;
        }
        out.body = buffer.substr(header_end + 4);
        while (out.body.size() < length) {
            const auto n = ::recv(fd, chunk, sizeof chunk, 0);
            if (n <= 0) return false;
            out.body.append(chunk, static_cast<std::size_t>(n));
        }
        out.body.resize(length);
        return true;
    }

    void serve(int fd, const std::string& remote) {
        transport::HttpRequest request;
        request.peer.remote_address = remote;
        int error_status = 400;
        if (!read_request(fd, request, error_status)) {
            const std::string out = std::string("HTTP/1.1 ") + std::to_string(error_status) + ' ' +
                                    http_detail::reason(error_status) +
                                    "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            http_detail::write_all(fd, out);
            ::close(fd);
            return;
        }
        auto exchange = std::make_shared<PosixExchange>(fd, std::move(request));
        try {
            handler_(exchange);
        } catch (const std::exception& e) {
            exchange->respond(500, {{"Content-Type", "text/plain"}}, e.what());
        }
        // The SDK handler may return before an SSE stream is finished; wait for it.
        exchange->wait_until_done(std::chrono::minutes(10));
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }

    Handler handler_;
    int listen_fd_ = -1;
    unsigned short port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread acceptor_;
    std::mutex count_mutex_;
    std::condition_variable count_cv_;
    std::size_t active_ = 0;
};

}  // namespace cppmcp::examples
