#include "cppmcp/server.hpp"
#include "cppmcp/transport/http.hpp"
#include "test.hpp"

#include <asio.hpp>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

using Tcp = asio::ip::tcp;

std::string response_headers(unsigned short port, const std::string& request) {
    asio::io_context context;
    Tcp::socket socket(context);
    socket.connect(Tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    asio::write(socket, asio::buffer(request));
    asio::streambuf response;
    asio::error_code error;
    asio::read_until(socket, response, "\r\n\r\n", error);
    if (error && error != asio::error::eof) {
        return {};
    }
    return {asio::buffers_begin(response.data()), asio::buffers_end(response.data())};
}

std::string request(const std::string& method, const std::string& target,
                    const std::string& headers = {}, const std::string& body = {}) {
    return method + " " + target + " HTTP/1.1\r\n" +
           (headers.empty() ? "Host: 127.0.0.1\r\n" : headers) +
           "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" +
           body;
}

std::string response_status(unsigned short port, const std::string& raw_request) {
    const auto response = response_headers(port, raw_request);
    const auto line_end = response.find("\r\n");
    return response.substr(0, line_end);
}

std::string json_request_body() {
    return R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{"name":"test","version":"1"},"io.modelcontextprotocol/clientCapabilities":{}}}})";
}

std::string valid_tools_request(unsigned short port, const std::string& target = "/mcp",
                                const std::string& extra_headers = {}) {
    const auto body = json_request_body();
    return request("POST", target,
                   "Host: 127.0.0.1:" + std::to_string(port) + "\r\n" +
                       "Content-Type: application/json\r\n"
                       "Accept: application/json, text/event-stream\r\n"
                       "MCP-Protocol-Version: 2026-07-28\r\n"
                       "Mcp-Method: tools/list\r\n" + extra_headers,
                   body);
}

cppmcp::transport::HttpOptions quick_options() {
    cppmcp::transport::HttpOptions options;
    options.port = 0;
    options.idle_timeout = std::chrono::milliseconds(500);
    options.header_timeout = std::chrono::milliseconds(150);
    options.body_timeout = std::chrono::milliseconds(150);
    return options;
}

}  // namespace

void test_http_server_methods() {
    cppmcp::Server server;
    auto options = quick_options();
    auto listener = server.start_http(options);
    const auto port = listener->port();
    CPPMCP_CHECK(response_status(port, request("GET", "/mcp")) == "HTTP/1.1 405 Method Not Allowed");
    CPPMCP_CHECK(response_status(port, request("DELETE", "/mcp")) == "HTTP/1.1 405 Method Not Allowed");
    listener->stop();
}

void test_http_server_host_origin() {
    cppmcp::Server server;
    auto options = quick_options();
    auto listener = server.start_http(options);
    const auto port = listener->port();
    CPPMCP_CHECK(response_status(
                     port, request("POST", "/mcp",
                                   "Host: evil.example\r\nContent-Type: application/json\r\n")) ==
                 "HTTP/1.1 403 Forbidden");
    CPPMCP_CHECK(response_status(
                     port, request("POST", "/mcp",
                                   "Host: 127.0.0.1\r\nOrigin: https://evil.example\r\n")) ==
                 "HTTP/1.1 403 Forbidden");
    CPPMCP_CHECK(response_status(
                     port, request("POST", "/mcp",
                                   "Host: 127.0.0.1\r\n"
                                   "Accept: application/json, text/event-stream\r\n"
                                   "Content-Type: text/plain\r\n", "x")) ==
                 "HTTP/1.1 415 Unsupported Media Type");
    listener->stop();
}

void test_http_server_smuggling() {
    cppmcp::Server server;
    auto options = quick_options();
    auto listener = server.start_http(options);
    const auto port = listener->port();
    const auto host = "Host: 127.0.0.1:" + std::to_string(port) + "\r\n";
    CPPMCP_CHECK(response_status(
                     port, "POST /mcp HTTP/1.1\r\n" + host +
                               "Content-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n") ==
                 "HTTP/1.1 400 Bad Request");
    CPPMCP_CHECK(response_status(
                     port, "POST /mcp HTTP/1.1\r\n" + host +
                               "Content-Length: 1\r\nContent-Length: 2\r\n\r\nxx") ==
                 "HTTP/1.1 400 Bad Request");
    CPPMCP_CHECK(response_status(
                     port, "POST /mcp HTTP/1.1\r\n" + host +
                               "Content-Length: 99999999\r\n\r\n") ==
                 "HTTP/1.1 413 Payload Too Large");
    listener->stop();
}

void test_http_server_slowloris() {
    cppmcp::Server server;
    auto options = quick_options();
    options.header_timeout = std::chrono::milliseconds(100);
    options.body_timeout = std::chrono::milliseconds(100);
    auto listener = server.start_http(options);
    const auto port = listener->port();
    const auto host = "Host: 127.0.0.1:" + std::to_string(port) + "\r\n";

    asio::io_context context;
    Tcp::socket headers_socket(context);
    headers_socket.connect(Tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    const std::string partial_headers = "POST /mcp HTTP/1.1\r\n" + host +
                                        "Content-Length: 1\r\n";
    asio::write(headers_socket, asio::buffer(partial_headers));
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
    asio::streambuf headers_response;
    asio::error_code headers_error;
    asio::read_until(headers_socket, headers_response, "\r\n\r\n", headers_error);
    const std::string header_result(asio::buffers_begin(headers_response.data()),
                                    asio::buffers_end(headers_response.data()));
    CPPMCP_CHECK(header_result.starts_with("HTTP/1.1 408 Request Timeout"));

    Tcp::socket body_socket(context);
    body_socket.connect(Tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    const std::string partial_body = "POST /mcp HTTP/1.1\r\n" + host +
                                     "Content-Length: 10\r\n\r\nx";
    asio::write(body_socket, asio::buffer(partial_body));
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
    asio::streambuf body_response;
    asio::error_code body_error;
    asio::read_until(body_socket, body_response, "\r\n\r\n", body_error);
    const std::string body_result(asio::buffers_begin(body_response.data()),
                                  asio::buffers_end(body_response.data()));
    CPPMCP_CHECK(body_result.starts_with("HTTP/1.1 408 Request Timeout"));
    listener->stop();
}

void test_http_server_auth_route_bind() {
    cppmcp::Server server;
    auto options = quick_options();
    options.bearer_token = "secret";
    options.handler.authenticator = [](const cppmcp::transport::HttpRequest& request) {
        for (const auto& [name, value] : request.headers) {
            if (name == "X-Auth" && value == "accepted") {
                return true;
            }
        }
        return false;
    };
    options.route = [](const cppmcp::transport::HttpRequest& request)
        -> std::optional<std::string> {
        if (request.target == "/alias") {
            return "/mcp";
        }
        return std::nullopt;
    };
    auto listener = server.start_http(options);
    const auto port = listener->port();
    const auto denied = valid_tools_request(port, "/alias");
    CPPMCP_CHECK(response_status(port, denied) == "HTTP/1.1 401 Unauthorized");
    const auto no_custom_auth =
        valid_tools_request(port, "/alias", "Authorization: Bearer secret\r\n");
    CPPMCP_CHECK(response_status(port, no_custom_auth) == "HTTP/1.1 401 Unauthorized");
    const auto authorized = valid_tools_request(
        port, "/alias", "Authorization: Bearer secret\r\nX-Auth: accepted\r\n");
    CPPMCP_CHECK(response_status(port, authorized) == "HTTP/1.1 200 OK");
    CPPMCP_CHECK(response_status(
                     port, valid_tools_request(
                               port, "/not-routed", "Authorization: Bearer secret\r\n"
                                                     "X-Auth: accepted\r\n")) ==
                 "HTTP/1.1 404 Not Found");
    listener->stop();

    auto unsafe = quick_options();
    unsafe.bind_address = "0.0.0.0";
    bool refused = false;
    try {
        auto rejected = server.start_http(unsafe);
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    CPPMCP_CHECK(refused);
}
