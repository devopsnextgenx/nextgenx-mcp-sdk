#include "cppmcp/transport/http.hpp"
#include "test.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using Json = nlohmann::json;
using cppmcp::Server;
using cppmcp::ToolDefinition;
using cppmcp::transport::Headers;
using cppmcp::transport::HttpHandlerOptions;
using cppmcp::transport::HttpRequest;
using cppmcp::transport::HttpResponseMode;
using cppmcp::transport::IHttpExchange;
using cppmcp::transport::McpHttpHandler;
using cppmcp::transport::SseStream;

class MockSseStream final : public SseStream {
public:
    void write_event(std::string_view data) override { events.emplace_back(data); }
    void close() override { closed = true; }

    std::vector<std::string> events;
    bool closed = false;
};

class MockExchange final : public IHttpExchange {
public:
    explicit MockExchange(HttpRequest request) : request_(std::move(request)) {}

    const HttpRequest& request() const override { return request_; }

    void respond(int response_status, Headers response_headers, std::string response_body) override {
        status = response_status;
        headers = std::move(response_headers);
        body = std::move(response_body);
    }

    SseStream& begin_sse(int response_status, Headers response_headers) override {
        status = response_status;
        headers = std::move(response_headers);
        is_sse = true;
        return stream;
    }

    void on_disconnect(std::function<void()> callback) override {
        disconnect_callback = std::move(callback);
    }

    HttpRequest request_;
    int status = 0;
    Headers headers;
    std::string body;
    bool is_sse = false;
    MockSseStream stream;
    std::function<void()> disconnect_callback;
};

HttpRequest make_request(std::string method, Json params = Json::object(),
                         std::string protocol_version = "2026-07-28") {
    params["_meta"]["io.modelcontextprotocol/protocolVersion"] = protocol_version;
    params["_meta"]["io.modelcontextprotocol/clientCapabilities"] = Json::object();
    Json body = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", std::move(params)}};
    HttpRequest request{
        .method = "POST",
        .target = "/mcp",
        .headers = {{"Accept", "application/json, text/event-stream"},
                    {"Content-Type", "application/json; charset=utf-8"},
                    {"MCP-Protocol-Version", protocol_version},
                    {"Mcp-Method", method}},
        .body = body.dump(),
        .peer = cppmcp::transport::PeerInfo{},
    };
    return request;
}

void add_name_header(HttpRequest& request, const std::string& name) {
    request.headers.emplace_back("Mcp-Name",
                                 cppmcp::transport::encode_mirrored_header_value(name));
}

const std::string* response_header(const Headers& headers, const std::string& name) {
    const auto it = std::find_if(headers.begin(), headers.end(), [&](const auto& header) {
        return header.first == name;
    });
    return it == headers.end() ? nullptr : &it->second;
}

Json response_json(const MockExchange& exchange) {
    return Json::parse(exchange.body);
}

void add_echo_tool(Server& server, std::string value = "echo") {
    server.add_tool(ToolDefinition{
        .name = "echo",
        .title = "Echo",
        .description = "Returns a server-specific value.",
        .input_schema = Json::object({{"type", "object"}}),
        .output_schema = Json::object(),
        .annotations = cppmcp::ToolAnnotation{},
        .meta = Json::object(),
        .handler = [value = std::move(value)](const Json&, const Json&) { return value; },
    });
}

}  // namespace

void test_http_handler() {
    using cppmcp::core::ErrorCode;
    using cppmcp::transport::encode_mirrored_header_value;
    using cppmcp::transport::decode_mirrored_header_value;

    Server server({"http-test-server", "HTTP test server", "1.0"});
    add_echo_tool(server);
    constexpr auto non_ascii_name = "\xE5\xB7\xA5\xE5\x85\xB7";
    server.add_tool(ToolDefinition{
        .name = non_ascii_name,
        .title = "Encoded name",
        .description = "Checks the encoded Mcp-Name value.",
        .input_schema = Json::object({{"type", "object"}}),
        .output_schema = Json::object(),
        .annotations = cppmcp::ToolAnnotation{},
        .meta = Json::object(),
        .handler = [](const Json&, const Json&) { return "encoded"; },
    });
    McpHttpHandler handler(server);

    auto discover = std::make_shared<MockExchange>(
        make_request("server/discover"));
    handler.handle(discover);
    CPPMCP_CHECK(discover->status == 200);
    CPPMCP_CHECK(response_header(discover->headers, "Content-Type") != nullptr);
    CPPMCP_CHECK(response_json(*discover)["result"]["serverInfo"]["name"] == "http-test-server");
    CPPMCP_CHECK(response_json(*discover)["result"]["protocolVersions"] ==
                  Json::array({"2026-07-28"}));

    auto call_request = make_request("tools/call", {{"name", "echo"}, {"arguments", Json::object()}});
    add_name_header(call_request, "echo");
    auto call = std::make_shared<MockExchange>(std::move(call_request));
    handler.handle(call);
    CPPMCP_CHECK(call->status == 200);
    CPPMCP_CHECK(response_json(*call)["result"]["structuredContent"]["result"] == "echo");

    auto encoded_name_request =
        make_request("tools/call", {{"name", non_ascii_name}, {"arguments", Json::object()}});
    add_name_header(encoded_name_request, non_ascii_name);
    auto encoded_name = std::make_shared<MockExchange>(std::move(encoded_name_request));
    handler.handle(encoded_name);
    CPPMCP_CHECK(encoded_name->status == 200);
    CPPMCP_CHECK(response_json(*encoded_name)["result"]["structuredContent"]["result"] == "encoded");

    auto wrong_case_request = make_request("tools/list");
    for (auto& [name, _] : wrong_case_request.headers) {
        if (name == "MCP-Protocol-Version") {
            name = "mcp-protocol-version";
        } else if (name == "Mcp-Method") {
            name = "mCP-mETHOD";
        }
    }
    auto wrong_case = std::make_shared<MockExchange>(std::move(wrong_case_request));
    handler.handle(wrong_case);
    CPPMCP_CHECK(wrong_case->status == 200);

    auto mismatch_request = make_request("tools/list");
    mismatch_request.headers[2].second = "2025-11-25";
    auto mismatch_exchange = std::make_shared<MockExchange>(std::move(mismatch_request));
    handler.handle(mismatch_exchange);
    CPPMCP_CHECK(mismatch_exchange->status == 400);
    CPPMCP_CHECK(response_json(*mismatch_exchange)["error"]["code"] ==
                  static_cast<int>(ErrorCode::header_mismatch));

    auto absent_method_request = make_request("tools/list");
    absent_method_request.headers.erase(
        std::remove_if(absent_method_request.headers.begin(), absent_method_request.headers.end(),
                       [](const auto& header) { return header.first == "Mcp-Method"; }),
        absent_method_request.headers.end());
    auto absent_method = std::make_shared<MockExchange>(std::move(absent_method_request));
    handler.handle(absent_method);
    CPPMCP_CHECK(absent_method->status == 400);
    CPPMCP_CHECK(response_json(*absent_method)["error"]["code"] ==
                  static_cast<int>(ErrorCode::header_mismatch));

    auto duplicate_version_request = make_request("tools/list");
    duplicate_version_request.headers.emplace_back("mcp-protocol-version", "2026-07-28");
    auto duplicate_version = std::make_shared<MockExchange>(std::move(duplicate_version_request));
    handler.handle(duplicate_version);
    CPPMCP_CHECK(duplicate_version->status == 400);
    CPPMCP_CHECK(response_json(*duplicate_version)["error"]["code"] ==
                  static_cast<int>(ErrorCode::header_mismatch));

    auto unsupported_version_request = make_request("tools/list", Json::object(), "2025-11-25");
    auto unsupported_version = std::make_shared<MockExchange>(std::move(unsupported_version_request));
    handler.handle(unsupported_version);
    CPPMCP_CHECK(unsupported_version->status == 400);
    CPPMCP_CHECK(response_json(*unsupported_version)["error"]["code"] ==
                  static_cast<int>(ErrorCode::unsupported_protocol_version));

    auto missing_capabilities_request = make_request("tools/list");
    auto missing_capabilities_body = Json::parse(missing_capabilities_request.body);
    missing_capabilities_body["params"]["_meta"].erase(
        "io.modelcontextprotocol/clientCapabilities");
    missing_capabilities_request.body = missing_capabilities_body.dump();
    auto missing_capabilities =
        std::make_shared<MockExchange>(std::move(missing_capabilities_request));
    handler.handle(missing_capabilities);
    CPPMCP_CHECK(missing_capabilities->status == 400);
    CPPMCP_CHECK(response_json(*missing_capabilities)["error"]["code"] ==
                  static_cast<int>(ErrorCode::invalid_request));

    auto missing_name_request = make_request("tools/call", {{"name", "echo"}});
    auto missing_name = std::make_shared<MockExchange>(std::move(missing_name_request));
    handler.handle(missing_name);
    CPPMCP_CHECK(missing_name->status == 400);
    CPPMCP_CHECK(response_json(*missing_name)["error"]["code"] ==
                  static_cast<int>(ErrorCode::header_mismatch));

    auto wrong_name_request = make_request("tools/call", {{"name", "echo"}});
    wrong_name_request.headers.emplace_back("Mcp-Name", "other");
    auto wrong_name = std::make_shared<MockExchange>(std::move(wrong_name_request));
    handler.handle(wrong_name);
    CPPMCP_CHECK(wrong_name->status == 400);
    CPPMCP_CHECK(response_json(*wrong_name)["error"]["code"] ==
                  static_cast<int>(ErrorCode::header_mismatch));

    auto bad_accept_request = make_request("tools/list");
    bad_accept_request.headers[0].second = "text/plain";
    auto bad_accept = std::make_shared<MockExchange>(std::move(bad_accept_request));
    handler.handle(bad_accept);
    CPPMCP_CHECK(bad_accept->status == 406);

    auto zero_quality_request = make_request("tools/list");
    zero_quality_request.headers[0].second =
        "application/json;q=0, text/event-stream;q=0, */*;q=1";
    auto zero_quality = std::make_shared<MockExchange>(std::move(zero_quality_request));
    handler.handle(zero_quality);
    CPPMCP_CHECK(zero_quality->status == 406);

    auto bad_content_type_request = make_request("tools/list");
    bad_content_type_request.headers[1].second = "text/plain";
    auto bad_content_type = std::make_shared<MockExchange>(std::move(bad_content_type_request));
    handler.handle(bad_content_type);
    CPPMCP_CHECK(bad_content_type->status == 415);

    auto get_request = make_request("tools/list");
    get_request.method = "GET";
    auto get = std::make_shared<MockExchange>(std::move(get_request));
    handler.handle(get);
    CPPMCP_CHECK(get->status == 405);
    CPPMCP_CHECK(response_header(get->headers, "Allow") != nullptr);
    CPPMCP_CHECK(*response_header(get->headers, "Allow") == "POST");

    auto missing_endpoint_request = make_request("tools/list");
    missing_endpoint_request.target = "/other";
    auto missing_endpoint = std::make_shared<MockExchange>(std::move(missing_endpoint_request));
    handler.handle(missing_endpoint);
    CPPMCP_CHECK(missing_endpoint->status == 404);

    auto forbidden_origin_request = make_request("tools/list");
    forbidden_origin_request.headers.emplace_back("Origin", "https://attacker.example");
    auto forbidden_origin = std::make_shared<MockExchange>(std::move(forbidden_origin_request));
    handler.handle(forbidden_origin);
    CPPMCP_CHECK(forbidden_origin->status == 403);

    HttpHandlerOptions origin_options;
    origin_options.allowed_origins = {"https://trusted.example"};
    McpHttpHandler origin_handler(server, origin_options);
    auto allowed_origin_request = make_request("tools/list");
    allowed_origin_request.headers.emplace_back("Origin", "https://TRUSTED.example");
    auto allowed_origin = std::make_shared<MockExchange>(std::move(allowed_origin_request));
    origin_handler.handle(allowed_origin);
    CPPMCP_CHECK(allowed_origin->status == 200);

    auto loopback_origin_request = make_request("tools/list");
    loopback_origin_request.headers.emplace_back("Origin", "http://localhost:3000");
    auto loopback_origin = std::make_shared<MockExchange>(std::move(loopback_origin_request));
    handler.handle(loopback_origin);
    CPPMCP_CHECK(loopback_origin->status == 200);

    HttpHandlerOptions sse_options;
    sse_options.response_mode = HttpResponseMode::sse_only;
    McpHttpHandler sse_handler(server, sse_options);
    auto sse_request = make_request("tools/list");
    sse_request.headers[0].second = "text/event-stream";
    auto sse = std::make_shared<MockExchange>(std::move(sse_request));
    sse_handler.handle(sse);
    CPPMCP_CHECK(sse->status == 200);
    CPPMCP_CHECK(sse->is_sse);
    CPPMCP_CHECK(sse->stream.closed);
    CPPMCP_CHECK(sse->stream.events.size() == 1);
    CPPMCP_CHECK(Json::parse(sse->stream.events.front())["result"].contains("tools"));

    CPPMCP_CHECK(encode_mirrored_header_value("us-west1") == "us-west1");
    constexpr auto utf8_value = "Hello, \xE4\xB8\x96\xE7\x95\x8C";
    CPPMCP_CHECK(encode_mirrored_header_value(utf8_value) ==
                  "=?base64?SGVsbG8sIOS4lueVjA==?=");
    CPPMCP_CHECK(encode_mirrored_header_value("").empty());
    const auto sentinel_value = "=?base64?not-a-value?=";
    CPPMCP_CHECK(encode_mirrored_header_value(sentinel_value) != sentinel_value);
    std::string decoded;
    CPPMCP_CHECK(decode_mirrored_header_value("=?base64?SGVsbG8sIOS4lueVjA==?=", decoded));
    CPPMCP_CHECK(decoded == utf8_value);
    CPPMCP_CHECK(decode_mirrored_header_value("", decoded));
    CPPMCP_CHECK(decoded.empty());
    CPPMCP_CHECK(!decode_mirrored_header_value("=?base64?%%%?=", decoded));
    CPPMCP_CHECK(!decode_mirrored_header_value(" unsafe ", decoded));

    Server server_a({"server-a", "Server A", "1"});
    Server server_b({"server-b", "Server B", "1"});
    add_echo_tool(server_a, "A");
    add_echo_tool(server_b, "B");
    McpHttpHandler handler_a(server_a);
    McpHttpHandler handler_b(server_b);
    auto round_robin_request_a = make_request("tools/call", {{"name", "echo"}});
    add_name_header(round_robin_request_a, "echo");
    auto round_robin_request_b = round_robin_request_a;
    auto round_robin_a = std::make_shared<MockExchange>(std::move(round_robin_request_a));
    auto round_robin_b = std::make_shared<MockExchange>(std::move(round_robin_request_b));
    handler_a.handle(round_robin_a);
    handler_b.handle(round_robin_b);
    CPPMCP_CHECK(response_json(*round_robin_a)["result"]["structuredContent"]["result"] == "A");
    CPPMCP_CHECK(response_json(*round_robin_b)["result"]["structuredContent"]["result"] == "B");
}

void test_http_handler_header_params_stateless() {
    const auto make_tool = [](std::string instance_value) {
        return cppmcp::ToolDefinition{
            .name = "lookup",
            .title = "Lookup",
            .description = "Return a mirrored region value.",
            .input_schema = Json{{"type", "object"},
                                 {"properties", {{"region", {{"type", "string"}}}}}},
            .output_schema = Json::object(),
            .annotations = cppmcp::ToolAnnotation{},
            .meta = Json::object(),
            .handler = [instance_value = std::move(instance_value)](
                           const Json& args, const Json&) {
                return Json{{"region", args.at("region")},
                            {"instance", instance_value}};
            },
        }.header_param("region");
    };

    Server first({"header-first", "Header first", "1"});
    Server second({"header-second", "Header second", "1"});
    first.add_tool(make_tool("first"));
    second.add_tool(make_tool("second"));
    McpHttpHandler first_handler(first);
    McpHttpHandler second_handler(second);

    const auto make_call = [](const std::string& region) {
        auto request = make_request(
            "tools/call", {{"name", "lookup"}, {"arguments", {{"region", region}}}});
        add_name_header(request, "lookup");
        request.headers.emplace_back(
            "Mcp-Param-region",
            cppmcp::transport::encode_mirrored_header_value(region));
        return request;
    };

    auto first_call = std::make_shared<MockExchange>(make_call("north"));
    auto second_call = std::make_shared<MockExchange>(make_call("south"));
    first_handler.handle(first_call);
    second_handler.handle(second_call);
    CPPMCP_CHECK(first_call->status == 200);
    CPPMCP_CHECK(second_call->status == 200);
    CPPMCP_CHECK(response_json(*first_call)["result"]["structuredContent"]["region"] ==
                 "north");
    CPPMCP_CHECK(response_json(*second_call)["result"]["structuredContent"]["region"] ==
                 "south");
    CPPMCP_CHECK(response_json(*first_call)["result"]["structuredContent"]["instance"] ==
                 "first");
    CPPMCP_CHECK(response_json(*second_call)["result"]["structuredContent"]["instance"] ==
                 "second");

    auto list = std::make_shared<MockExchange>(make_request("tools/list"));
    first_handler.handle(list);
    CPPMCP_CHECK(response_json(*list)["result"]["tools"][0]["inputSchema"]["properties"]
                                      ["region"]["x-mcp-header"] == "Mcp-Param-region");

    auto mismatch_request = make_call("north");
    mismatch_request.headers.erase(
        std::remove_if(mismatch_request.headers.begin(), mismatch_request.headers.end(),
                       [](const auto& header) { return header.first == "Mcp-Param-region"; }),
        mismatch_request.headers.end());
    auto mismatch_exchange = std::make_shared<MockExchange>(std::move(mismatch_request));
    first_handler.handle(mismatch_exchange);
    CPPMCP_CHECK(mismatch_exchange->status == 400);
    CPPMCP_CHECK(response_json(*mismatch_exchange)["error"]["code"] ==
                 static_cast<int>(cppmcp::core::ErrorCode::header_mismatch));

    bool registration_rejected = false;
    try {
        first.add_tool(cppmcp::ToolDefinition{
            .name = "invalid",
            .title = "Invalid",
            .description = "Invalid header parameter declaration.",
            .input_schema = Json{{"type", "object"}, {"properties", Json::object()}},
            .output_schema = Json::object(),
            .annotations = cppmcp::ToolAnnotation{},
            .meta = Json::object(),
            .handler = [](const Json&, const Json&) { return Json::object(); },
        }.header_param("missing"));
    } catch (const std::invalid_argument&) {
        registration_rejected = true;
    }
    CPPMCP_CHECK(registration_rejected);
}
