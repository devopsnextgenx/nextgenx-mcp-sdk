#pragma once

#include <functional>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cppmcp/server.hpp"

namespace cppmcp::transport {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct PeerInfo {
    std::string remote_address;
};

struct HttpRequest {
    std::string method;
    std::string target;
    Headers headers;
    std::string body;
    PeerInfo peer;
};

class SseStream {
public:
    virtual ~SseStream() = default;
    virtual void write_event(std::string_view data) = 0;
    virtual void close() = 0;
};

class IHttpExchange {
public:
    virtual ~IHttpExchange() = default;
    virtual const HttpRequest& request() const = 0;
    virtual void respond(int status, Headers headers, std::string body) = 0;
    virtual SseStream& begin_sse(int status, Headers headers) = 0;
    virtual void on_disconnect(std::function<void()> callback) = 0;
};

enum class HttpResponseMode {
    automatic,
    json_only,
    sse_only,
};

struct HttpHandlerOptions {
    std::string path = "/mcp";
    HttpResponseMode response_mode = HttpResponseMode::automatic;
    std::vector<std::string> allowed_origins;
    std::vector<std::string> allowed_hosts;
    std::function<bool(const HttpRequest&)> authenticator;
};

struct HttpOptions {
    std::string bind_address = "127.0.0.1";
    unsigned short port = 8000;
    std::string bearer_token;
    bool allow_non_loopback = false;
    HttpHandlerOptions handler;
    std::function<std::optional<std::string>(const HttpRequest&)> route;
    std::size_t max_header_bytes = 32 * 1024;
    std::size_t max_body_bytes = 16 * 1024 * 1024;
    std::size_t sse_queue_max_bytes = 1024 * 1024;
    std::chrono::milliseconds header_timeout{5000};
    std::chrono::milliseconds body_timeout{10000};
    std::chrono::milliseconds idle_timeout{30000};
    std::chrono::milliseconds sse_keep_alive_interval{15000};
};

class McpHttpHandler {
public:
    explicit McpHttpHandler(Server& server, HttpHandlerOptions options = {});

    void handle(const std::shared_ptr<IHttpExchange>& exchange) const;

private:
    Server& server_;
    HttpHandlerOptions options_;
};

[[nodiscard]] std::string encode_mirrored_header_value(std::string_view value);
[[nodiscard]] bool decode_mirrored_header_value(std::string_view value, std::string& decoded);

}  // namespace cppmcp::transport
