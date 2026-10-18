#include "cppmcp/transport/http.hpp"
#include "cppmcp/core/log.hpp"

#include <llhttp.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace {

int ignore_data(llhttp_t*, const char*, std::size_t) {
    return 0;
}

int ignore_complete(llhttp_t*) {
    return 0;
}

void parse_http_request(std::string_view request) {
    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_url = ignore_data;
    settings.on_header_field = ignore_data;
    settings.on_header_value = ignore_data;
    settings.on_body = ignore_data;
    settings.on_message_complete = ignore_complete;
    llhttp_t parser;
    llhttp_init(&parser, HTTP_REQUEST, &settings);
    (void)llhttp_execute(&parser, request.data(), request.size());
}

class FuzzSseStream final : public cppmcp::transport::SseStream {
public:
    void write_event(std::string_view) override {}
    void close() override {}
};

class FuzzExchange final : public cppmcp::transport::IHttpExchange {
public:
    explicit FuzzExchange(cppmcp::transport::HttpRequest request)
        : request_(std::move(request)) {}

    const cppmcp::transport::HttpRequest& request() const override { return request_; }
    void respond(int, cppmcp::transport::Headers, std::string) override {}
    cppmcp::transport::SseStream& begin_sse(
        int, cppmcp::transport::Headers) override {
        return stream_;
    }
    void on_disconnect(std::function<void()>) override {}

private:
    cppmcp::transport::HttpRequest request_;
    FuzzSseStream stream_;
};

void fuzz_handler(cppmcp::transport::McpHttpHandler& handler, std::string target,
                  std::string host) {
    cppmcp::transport::HttpRequest request{
        .method = "GET",
        .target = std::move(target),
        .headers = {{"Host", std::move(host)}},
        .body = {},
        .peer = {},
    };
    handler.handle(std::make_shared<FuzzExchange>(std::move(request)));
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 4096) {
        return 0;
    }
    cppmcp::core::log::configure(cppmcp::core::log::Level::error);
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    const auto separator = input.find('\0');
    const auto target = input.substr(0, separator);
    const auto host = separator == std::string_view::npos
                          ? std::string_view{}
                          : input.substr(separator + 1);
    const std::string wire_request =
        "GET " + std::string(target) + " HTTP/1.1\r\nHost: " +
        std::string(host) + "\r\n\r\n";

    constexpr std::string_view valid_request =
        "GET /mcp HTTP/1.1\r\nHost: example.test\r\n\r\n";
    parse_http_request(valid_request);
    parse_http_request(wire_request);

    cppmcp::Server server;
    cppmcp::transport::HttpHandlerOptions options;
    options.allowed_hosts = {"example.test"};
    cppmcp::transport::McpHttpHandler handler(server, std::move(options));
    fuzz_handler(handler, "/mcp", "example.test");
    fuzz_handler(handler, std::string(target), "example.test");
    fuzz_handler(handler, "/mcp", std::string(host));
    return 0;
}
