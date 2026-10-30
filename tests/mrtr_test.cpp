#include "cppmcp/core/request_state.hpp"
#include "cppmcp/client.hpp"
#include "cppmcp/transport/stdio.hpp"
#include "cppmcp/server.hpp"
#include "test.hpp"

#include <chrono>
#include <future>
#include <stdexcept>
#include <string>

namespace {

using Json = nlohmann::json;
constexpr auto secret = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

template <typename Fn>
bool rejects(Fn&& fn) {
    try {
        fn();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

Json input_requests() {
    return {{"confirm", {{"method", "elicitation/create"},
                         {"params", {{"mode", "form"}, {"message", "Confirm?"},
                                     {"requestedSchema", {{"type", "object"},
                                         {"properties", {{"confirm", {{"type", "boolean"}}}}},
                                         {"required", {"confirm"}}, {"additionalProperties", false}}}}}}}};
}

cppmcp::core::Request call() {
    return {1, "tools/call", {{"name", "confirm"}, {"arguments", {{"project", "alpha"}}},
        {"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                   {"io.modelcontextprotocol/clientCapabilities", {{"elicitation", {{"form", Json::object()}}}}}}}}};
}

void register_tool(cppmcp::Server& server) {
    cppmcp::ToolDefinition tool;
    tool.name = "confirm";
    server.add_tool(std::move(tool), [](const Json&, const Json&, cppmcp::ToolContext& context) {
        if (context.input_responses().empty()) {
            return cppmcp::ToolResult::input_required(input_requests(), {{"checkpoint", 7}}).to_json();
        }
        return cppmcp::ToolResult::structured({{"state", context.state()},
                                               {"response", context.input_responses().at("confirm")}}).to_json();
    });
}

Json initial_result(cppmcp::Server& server) {
    const auto response = server.handle_request(call());
    const auto* result = std::get_if<cppmcp::core::ResultResponse>(&response);
    CPPMCP_CHECK(result != nullptr);
    return result ? result->result : Json::object();
}

cppmcp::core::Request continuation(const Json& first) {
    auto request = call();
    request.id = 2;
    request.params["requestState"] = first.at("requestState");
    request.params["inputResponses"] = {{"confirm", {{"action", "accept"}, {"content", {{"confirm", true}}}}}};
    return request;
}

Json run_client(cppmcp::Server& server, cppmcp::Client::CallToolOptions options) {
    auto [client_transport, server_transport] = cppmcp::core::make_memory_transport_pair();
    std::vector<cppmcp::core::RequestId> ids;
    server_transport->on_message([&](const cppmcp::core::Message& message) {
        if (const auto* request = std::get_if<cppmcp::core::Request>(&message)) {
            for (const auto& previous : ids) CPPMCP_CHECK(previous != request->id);
            ids.push_back(request->id);
        }
        server_transport->send(server.handle_message(message));
    });
    cppmcp::Client client;
    client.connect(client_transport);
    try {
        auto result = client.call_tool("confirm", {{"project", "alpha"}}, std::move(options));
        client.close();
        return result;
    } catch (...) {
        client.close();
        throw;
    }
}

}  // namespace

void test_mrtr() {
    cppmcp::Server server;
    register_tool(server);
    const auto first = initial_result(server);
    CPPMCP_CHECK(first.value("resultType", std::string{}) == "input_required");
    CPPMCP_CHECK(first.contains("requestState"));
    CPPMCP_CHECK(!first.contains("privateState"));
    if (!first.contains("requestState")) return;
    const auto response = server.handle_request(continuation(first));
    const auto* result = std::get_if<cppmcp::core::ResultResponse>(&response);
    CPPMCP_CHECK(result != nullptr);
    if (result) {
        CPPMCP_CHECK(result->result["resultType"] == "complete");
        CPPMCP_CHECK(result->result["structuredContent"]["state"]["checkpoint"] == 7);
    }
    auto bad = continuation(first);
    bad.params["arguments"]["project"] = "beta";
    CPPMCP_CHECK(std::holds_alternative<cppmcp::core::ErrorResponse>(server.handle_request(bad)));
    bad = continuation(first);
    bad.params.erase("requestState");
    CPPMCP_CHECK(std::holds_alternative<cppmcp::core::ErrorResponse>(server.handle_request(bad)));
    bad.params["requestState"] = 42;
    CPPMCP_CHECK(std::holds_alternative<cppmcp::core::ErrorResponse>(server.handle_request(bad)));
    bad = continuation(first);
    bad.params["requestState"] = first["requestState"].get<std::string>() + "x";
    CPPMCP_CHECK(std::holds_alternative<cppmcp::core::ErrorResponse>(server.handle_request(bad)));
}

void test_elicit() {
    cppmcp::Server server;
    register_tool(server);
    auto unsupported = call();
    unsupported.params["_meta"]["io.modelcontextprotocol/clientCapabilities"] = Json::object();
    const auto response = server.handle_request(unsupported);
    const auto* error = std::get_if<cppmcp::core::ErrorResponse>(&response);
    CPPMCP_CHECK(error != nullptr);
    if (error) CPPMCP_CHECK(error->error.code == -32021);
    const auto first = initial_result(server);
    if (!first.contains("requestState")) return;
    auto bad = continuation(first);
    bad.params["inputResponses"]["confirm"]["content"]["confirm"] = "yes";
    CPPMCP_CHECK(std::holds_alternative<cppmcp::core::ErrorResponse>(server.handle_request(bad)));
    bad = continuation(first);
    bad.params["inputResponses"]["unknown"] = {{"action", "cancel"}};
    CPPMCP_CHECK(std::holds_alternative<cppmcp::core::ErrorResponse>(server.handle_request(bad)));
    bad = continuation(first);
    bad.params["inputResponses"]["confirm"]["action"] = "unknown";
    CPPMCP_CHECK(std::holds_alternative<cppmcp::core::ErrorResponse>(server.handle_request(bad)));
    for (const auto* action : {"decline", "cancel"}) {
        auto declined = continuation(first);
        declined.params["inputResponses"]["confirm"] = {{"action", action}};
        CPPMCP_CHECK(std::holds_alternative<cppmcp::core::ResultResponse>(server.handle_request(declined)));
    }
}

void test_request_state() {
    using namespace std::chrono;
    auto now = system_clock::time_point(seconds(1000));
    cppmcp::core::HmacRequestStateCodec codec({{"key1", secret}}, seconds(10), 8192, [&] { return now; });
    cppmcp::core::RequestStateBinding binding;
    binding.method = "tools/call";
    binding.name = "confirm";
    binding.params = {{"arguments", {{"project", "alpha"}}}};
    binding.subject = "subject-1";
    binding.private_payload = {{"checkpoint", 7}};
    binding.input_request_ids = {"confirm"};
    binding.round = 1;
    const auto token = codec.issue(binding);
    CPPMCP_CHECK(codec.verify(token, binding).private_payload == binding.private_payload);
    CPPMCP_CHECK(codec.verify(token, binding).input_request_ids == binding.input_request_ids);
    CPPMCP_CHECK(rejects([&] { codec.verify(token + "x", binding); }));
    CPPMCP_CHECK(rejects([&] { codec.verify("v1.invalid", binding); }));
    auto changed = binding;
    changed.subject = "subject-2";
    CPPMCP_CHECK(rejects([&] { codec.verify(token, changed); }));
    changed = binding;
    changed.name = "other";
    CPPMCP_CHECK(rejects([&] { codec.verify(token, changed); }));
    changed = binding;
    changed.method = "prompts/get";
    CPPMCP_CHECK(rejects([&] { codec.verify(token, changed); }));
    changed = binding;
    changed.params["arguments"]["project"] = "beta";
    CPPMCP_CHECK(rejects([&] { codec.verify(token, changed); }));
    now += seconds(10);
    CPPMCP_CHECK(rejects([&] { codec.verify(token, binding); }));
    now -= seconds(11);
    CPPMCP_CHECK(rejects([&] { codec.verify(token, binding); }));
    now = system_clock::time_point(seconds(-1000));
    CPPMCP_CHECK(codec.verify(codec.issue(binding), binding).issued_at == -1000);
    CPPMCP_CHECK(rejects([&] { cppmcp::core::HmacRequestStateCodec invalid(
        std::vector<cppmcp::core::RequestStateKey>{{"key", "bad"}}); }));
    CPPMCP_CHECK(rejects([&] { cppmcp::core::HmacRequestStateCodec invalid({{"key", secret}}, seconds(0)); }));
}

void test_two_instance() {
    cppmcp::ServerOptions options;
    options.request_state_keys = {{"shared", secret}};
    cppmcp::Server first({"first", "first", "1"}, options);
    cppmcp::Server second({"second", "second", "1"}, options);
    register_tool(first);
    register_tool(second);
    const auto initial = initial_result(first);
    if (!initial.contains("requestState")) return;
    CPPMCP_CHECK(std::holds_alternative<cppmcp::core::ResultResponse>(second.handle_request(continuation(initial))));
    cppmcp::Server foreign;
    register_tool(foreign);
    CPPMCP_CHECK(std::holds_alternative<cppmcp::core::ErrorResponse>(foreign.handle_request(continuation(initial))));
    cppmcp::core::HmacRequestStateCodec old(
        std::vector<cppmcp::core::RequestStateKey>{{"old", secret}});
    cppmcp::core::HmacRequestStateCodec rotated({{"old", secret}, {"new", std::string(64, 'a')}});
    cppmcp::core::RequestStateBinding binding;
    const auto old_token = old.issue(binding);
    CPPMCP_CHECK(rotated.verify(old_token, binding).subject == binding.subject);
    CPPMCP_CHECK(rejects([&] { old.verify(rotated.issue(binding), binding); }));
}

void test_mrtr_client() {
    cppmcp::Server server;
    register_tool(server);
    cppmcp::Client::CallToolOptions options;
    int invocations = 0;
    options.on_elicit = [&](const std::string& id, const Json& request) {
        ++invocations;
        CPPMCP_CHECK(id == "confirm");
        CPPMCP_CHECK(request["params"]["mode"] == "form");
        return Json{{"action", "accept"}, {"content", {{"confirm", true}}}};
    };
    const auto result = run_client(server, options);
    CPPMCP_CHECK(invocations == 1);
    CPPMCP_CHECK(result["structuredContent"]["state"]["checkpoint"] == 7);

    cppmcp::ToolDefinition pending;
    pending.name = "confirm";
    server.add_tool(pending, [](const Json&, const Json&, cppmcp::ToolContext& context) {
        return context.state().value("done", false)
            ? cppmcp::ToolResult::structured(true).to_json()
            : cppmcp::ToolResult::input_required(Json::object(), {{"done", true}}).to_json();
    });
    CPPMCP_CHECK(run_client(server, {})["structuredContent"] == true);

    server.add_tool(pending, [](const Json&, const Json&, cppmcp::ToolContext&) {
        return cppmcp::ToolResult::input_required(input_requests()).to_json();
    });
    options.max_rounds = 1;
    bool limited = false;
    try {
        (void)run_client(server, options);
    } catch (const std::runtime_error& error) {
        limited = std::string(error.what()).find("TooManyRounds") != std::string::npos;
    }
    CPPMCP_CHECK(limited);
}

void test_elicit_url() {
    cppmcp::Server server;
    cppmcp::ToolDefinition tool;
    tool.name = "confirm";
    server.add_tool(tool, [](const Json&, const Json&, cppmcp::ToolContext& context) {
        return context.input_responses().empty()
            ? cppmcp::ToolResult::input_required({{"authorize", {{"method", "elicitation/create"},
                {"params", {{"mode", "url"}, {"message", "Authorize"},
                             {"url", "https://example.test/authorize"}}}}}}).to_json()
            : cppmcp::ToolResult::structured(true).to_json();
    });
    cppmcp::Client::CallToolOptions options;
    options.allow_url_elicitation = true;
    options.on_elicit = [](const std::string&, const Json& request) {
        CPPMCP_CHECK(request["params"]["url"] == "https://example.test/authorize");
        return Json{{"action", "accept"}};
    };
    CPPMCP_CHECK(run_client(server, options)["structuredContent"] == true);
    auto missing = call();
    const auto response = server.handle_request(missing);
    const auto* error = std::get_if<cppmcp::core::ErrorResponse>(&response);
    CPPMCP_CHECK(error != nullptr);
    if (error) CPPMCP_CHECK(error->error.code == -32021);
    for (const auto& definition : {Json{{"type", "object"}}, Json{{"type", "string"}, {"format", "password"}}}) {
        CPPMCP_CHECK(rejects([&] { cppmcp::core::validate_elicitation_form_schema(
            {{"type", "object"}, {"properties", {{"value", definition}}}}); }));
    }
    CPPMCP_CHECK(rejects([&] { cppmcp::core::validate_elicitation_form_schema(
        {{"type", "object"}, {"properties", {{"api_token", {{"type", "string"}}}}}}); }));
}

void test_mrtr_generic() {
    cppmcp::Server server;
    cppmcp::ToolDefinition tool;
    tool.name = "confirm";
    server.add_tool(tool, [](const Json&, const Json&, cppmcp::ToolContext& context) {
        return context.input_responses().empty()
            ? cppmcp::ToolResult::input_required({{"roots", {{"method", "roots/list"}, {"params", Json::object()}}}}).to_json()
            : cppmcp::ToolResult::structured(context.input_responses()["roots"]["roots"]).to_json();
    });
    cppmcp::Client::CallToolOptions options;
    options.client_capabilities = {{"roots", Json::object()}};
    options.on_input = [](const std::string& id, const Json& request) {
        CPPMCP_CHECK(id == "roots");
        CPPMCP_CHECK(request["method"] == "roots/list");
        return Json{{"roots", Json::array({{{"uri", "file:///workspace"}}})}};
    };
    CPPMCP_CHECK(run_client(server, options)["structuredContent"][0]["uri"] == "file:///workspace");
    const auto response = server.handle_request(call());
    const auto* error = std::get_if<cppmcp::core::ErrorResponse>(&response);
    CPPMCP_CHECK(error != nullptr);
    if (error) CPPMCP_CHECK(error->error.code == -32021);
}

void test_legacy_adapter() {
    cppmcp::Server server;
    register_tool(server);
    auto legacy = call();
    legacy.params.erase("_meta");
    cppmcp::ToolContext context({}, {}, legacy.params, std::chrono::milliseconds(0), 100,
                               {{"elicitation", {{"form", Json::object()}}}});
    int invocations = 0;
    context.set_input_request_handler([&](const Json& request) {
        ++invocations;
        CPPMCP_CHECK(request["method"] == "elicitation/create");
        return Json{{"action", "accept"}, {"content", {{"confirm", true}}}};
    });
    const auto response = server.handle_message(legacy, &context);
    const auto* result = std::get_if<cppmcp::core::ResultResponse>(&response);
    CPPMCP_CHECK(result != nullptr);
    if (result) CPPMCP_CHECK(result->result["structuredContent"]["state"]["checkpoint"] == 7);
    CPPMCP_CHECK(invocations == 1);
    CPPMCP_CHECK(!context.request_state().empty());
    const auto modern = server.handle_message(call(), &context);
    const auto* pending = std::get_if<cppmcp::core::ResultResponse>(&modern);
    CPPMCP_CHECK(pending != nullptr);
    if (pending) CPPMCP_CHECK(pending->result["resultType"] == "input_required");
    CPPMCP_CHECK(invocations == 1);
}

void test_http_progress_live() {
    cppmcp::Server server;
    std::promise<void> received;
    auto delivered = received.get_future();
    bool live = false;
    cppmcp::ToolDefinition tool;
    tool.name = "confirm";
    server.add_tool(tool, [&](const Json&, const Json&, cppmcp::ToolContext& context) {
        context.report_progress(1, 1);
        live = delivered.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
        return Json{{"live", live}};
    });
    cppmcp::transport::HttpOptions server_options;
    server_options.port = 0;
    auto listener = server.start_http(server_options);
    cppmcp::HttpClientOptions http_options;
    http_options.url = "http://127.0.0.1:" + std::to_string(listener->port()) + "/mcp";
    http_options.policy = cppmcp::NegotiationPolicy::modern_only;
    cppmcp::Client client;
    client.connect_http(http_options);
    cppmcp::Client::CallToolOptions options;
    options.on_progress = [&](double, std::optional<double>, std::optional<std::string>) { received.set_value(); };
    const auto result = client.call_tool("confirm", Json::object(), options);
    CPPMCP_CHECK(result["structuredContent"]["live"] == true);
    client.close();
    listener->stop();
}

void test_http_legacy_elicit() {
    cppmcp::Server server;
    register_tool(server);
    cppmcp::transport::HttpOptions server_options;
    server_options.port = 0;
    server_options.handler.input_request_timeout = std::chrono::seconds(2);
    auto listener = server.start_http(server_options);
    cppmcp::HttpClientOptions http_options;
    http_options.url = "http://127.0.0.1:" + std::to_string(listener->port()) + "/mcp";
    http_options.policy = cppmcp::NegotiationPolicy::legacy_only;
    http_options.client_capabilities = {{"elicitation", {{"form", Json::object()}}}};
    cppmcp::Client client;
    client.connect_http(http_options);
    cppmcp::Client::CallToolOptions options;
    options.on_elicit = [](const std::string& id, const Json&) {
        CPPMCP_CHECK(id.starts_with("server-"));
        return Json{{"action", "accept"}, {"content", {{"confirm", true}}}};
    };
    const auto result = client.call_tool("confirm", Json::object(), options);
    CPPMCP_CHECK(result["structuredContent"]["state"]["checkpoint"] == 7);
    CPPMCP_CHECK(!result.contains("resultType"));
    client.close();
    listener->stop();
}

void test_stdio_legacy_elicit() {
    auto transport = std::make_shared<cppmcp::transport::StdioClientTransport>(
        cppmcp::transport::StdioProcessSpec{"./cppmcp_stdio_server", {}, {}, "."});
    cppmcp::core::Connection connection(transport);
    std::atomic<int> requests{0};
    connection.set_request_handler([&](const cppmcp::core::Request& input) {
        ++requests;
        CPPMCP_CHECK(input.method == "elicitation/create");
        transport->send(cppmcp::core::ResultResponse{input.id,
            {{"action", "accept"}, {"content", {{"confirm", true}}}}});
    });
    connection.start();
    (void)connection.send_request("initialize", {{"protocolVersion", "2025-11-25"},
        {"capabilities", {{"elicitation", {{"form", Json::object()}}}}}}).get();
    const auto result = connection.send_request("tools/call", {{"name", "delete_project"},
        {"arguments", {{"project", "stdio"}}}}).get();
    CPPMCP_CHECK(result["content"][0]["text"] == "deleted stdio");
    CPPMCP_CHECK(requests.load() == 1);
    CPPMCP_CHECK(!result.contains("resultType"));
    connection.close();
}

void test_request_state_subject() {
    cppmcp::Server server;
    register_tool(server);
    auto request = call();
    cppmcp::ToolContext first({}, {}, request.params);
    first.set_request_subject("alice");
    const auto response = server.handle_request(request, &first);
    const auto* pending = std::get_if<cppmcp::core::ResultResponse>(&response);
    CPPMCP_CHECK(pending != nullptr);
    if (!pending) return;
    const auto retry = continuation(pending->result);
    cppmcp::ToolContext other({}, {}, retry.params);
    other.set_request_subject("bob");
    CPPMCP_CHECK(std::holds_alternative<cppmcp::core::ErrorResponse>(server.handle_request(retry, &other)));
    other.set_request_subject("alice");
    CPPMCP_CHECK(std::holds_alternative<cppmcp::core::ResultResponse>(server.handle_request(retry, &other)));
}