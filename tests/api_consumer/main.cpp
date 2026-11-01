#include "cppmcp/client.hpp"
#include "cppmcp/core/transport.hpp"
#include "cppmcp/core/uri_template.hpp"
#include "cppmcp/server.hpp"
#include "cppmcp/server/file_resource_provider.hpp"
#include "cppmcp/server/features.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using Json = nlohmann::json;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Json result(const cppmcp::core::Message& message) {
    const auto* response = std::get_if<cppmcp::core::ResultResponse>(&message);
    require(response != nullptr, "expected successful MCP response");
    return response->result;
}

void exercise_protocol(bool modern) {
    const auto root = std::filesystem::temp_directory_path() / "cppmcp-api-consumer";
    std::filesystem::create_directories(root);
    {
        std::ofstream file(root / "hello.txt", std::ios::binary);
        file << "installed-api";
    }

    cppmcp::ServerOptions server_options;
    server_options.request_state_keys = {{"consumer", std::string(64, 'a')}};
    cppmcp::Server server({"api-consumer", "API consumer", "1.0"}, server_options);
    cppmcp::examples::FeatureOptions options;
    options.page_size = 1;
    options.resource_root = root.string();
    server.features().configure(options);

    server.prompt("greet")
        .title("Greeting")
        .description("Return a greeting prompt.")
        .argument("name", "Name", "Name to greet", true,
                  cppmcp::examples::prefix_provider({"Ada", "Grace"}))
        .handler([](const cppmcp::examples::CallContext&, const Json& args) {
            return cppmcp::PromptResult{{
                {"messages", Json::array({{{"role", "user"},
                                           {"content", {{"type", "text"},
                                                        {"text", "Hello " + args["name"].get<std::string>()}}}}})}}};
        });
    server.prompt("second")
        .handler([](const cppmcp::examples::CallContext&, const Json&) {
            return cppmcp::PromptResult{{"messages", Json::array()}};
        });
    server.resource("demo://static")
        .name("static")
        .title("Static resource")
        .mime_type("text/plain")
        .annotations({{"audience", Json::array({"user"})}, {"priority", 0.5}})
        .handler([](const cppmcp::examples::CallContext&) {
            return cppmcp::ReadResult{{
                {"contents", Json::array({{{"uri", "demo://static"}, {"text", "static"}}})}}};
        });
    server.resource_template("demo://item/{id}")
        .name("item")
        .completion("id", cppmcp::examples::prefix_provider({"1", "42"}))
        .handler([](const cppmcp::examples::CallContext&,
                    const cppmcp::core::UriTemplate::Variables& vars) {
            const auto id = vars.at("id").get<std::string>();
            return cppmcp::ReadResult{{
                {"contents", Json::array({{{"uri", "demo://item/" + id}, {"text", id}}})}}};
        });
    server.features().add_file_resource_template();

    cppmcp::ToolDefinition pending;
    pending.name = "pending";
    pending.handler = [](const Json&, const Json&) {
        return cppmcp::ToolResult::input_required(Json::object());
    };
    server.add_tool(std::move(pending));

    const auto params = [modern](Json value) {
        if (modern) {
            value["_meta"] = {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}};
        }
        return value;
    };
    const auto prompt_list = result(server.handle_request(
        {1, "prompts/list", params(Json::object())}));
    require(prompt_list["prompts"].size() == 1, "page size was not applied");
    const auto cursor = prompt_list.value("nextCursor", std::string{});
    require(!cursor.empty(), "missing stable prompt cursor");
    const auto next_prompt = result(server.handle_request(
        {2, "prompts/list", params(Json{{"cursor", cursor}})}));
    require(next_prompt["prompts"][0]["name"] == "second", "cursor did not resume after first prompt");

    const auto prompt = result(server.handle_request(
        {3, "prompts/get", params(Json{{"name", "greet"},
                                      {"arguments", {{"name", "Ada"}}}})}));
    require(prompt["messages"][0]["content"]["text"] == "Hello Ada", "prompt handler failed");
    const auto completion = result(server.handle_request(
        {4, "completion/complete", params(Json{
            {"ref", {{"type", "ref/prompt"}, {"name", "greet"}}},
            {"argument", {{"name", "name"}, {"value", "A"}}}})}));
    require(completion["completion"]["values"] == Json::array({"Ada"}),
            "prompt completion provider failed");

    const auto static_resource = result(server.handle_request(
        {5, "resources/read", params(Json{{"uri", "demo://static"}})}));
    require(static_resource["contents"][0]["text"] == "static", "static resource failed");
    const auto template_resource = result(server.handle_request(
        {6, "resources/read", params(Json{{"uri", "demo://item/42"}})}));
    require(template_resource["contents"][0]["text"] == "42", "resource template failed");
    const auto file_resource = result(server.handle_request(
        {7, "resources/read", params(Json{{"uri", "demo://files/hello.txt"}})}));
    require(file_resource["contents"][0]["text"] == "installed-api",
            "public file provider failed");

    const auto subscribed = result(server.handle_request(
        {8, "resources/subscribe", params(Json{{"uri", "demo://static"}})}));
    require(subscribed.is_object(), "resource subscription failed");
    require(server.features().remove_prompt("second"), "runtime prompt removal failed");
    require(server.features().remove_resource("demo://static"),
            "runtime resource removal failed");

    const auto typed = result(server.handle_request(
        {9, "tools/call", params(Json{{"name", "pending"},
                                      {"arguments", Json::object()}})}));
    require(typed.value("resultType", std::string{}) == "input_required",
            "typed InputRequired result was not preserved");

    std::filesystem::remove_all(root);
}

void exercise_typed_client() {
    cppmcp::Server server({"typed-client-server", "typed client", "1.0"});
    cppmcp::examples::FeatureOptions options;
    options.page_size = 1;
    server.features().configure(options);
    server.prompt("greet")
        .handler([](const cppmcp::examples::CallContext&, const Json&) {
            return cppmcp::PromptResult{{
                {"messages", Json::array({{{"role", "user"},
                                           {"content", {{"type", "text"},
                                                        {"text", "Hello"}}}}})}}};
        });
    server.prompt("second")
        .handler([](const cppmcp::examples::CallContext&, const Json&) {
            return cppmcp::PromptResult{{{"messages", Json::array()}}};
        });
    server.resource("demo://client")
        .name("client")
        .mime_type("text/plain")
        .handler([](const cppmcp::examples::CallContext&) {
            return cppmcp::ReadResult{{
                {"contents", Json::array({{{"uri", "demo://client"}, {"text", "read"}}})}}};
        });

    auto [client_transport, server_transport] =
        cppmcp::core::make_memory_transport_pair();
    const std::weak_ptr<cppmcp::core::MemoryTransport> weak_server_transport =
        server_transport;
    server_transport->on_message([&server, weak_server_transport](
                                     const cppmcp::core::Message& message) {
        const auto response = server.handle_message(message);
        if (std::holds_alternative<cppmcp::core::ResultResponse>(response) ||
            std::holds_alternative<cppmcp::core::ErrorResponse>(response)) {
            if (const auto transport = weak_server_transport.lock()) {
                transport->send(response);
            }
        }
    });
    cppmcp::Client client;
    client.connect(client_transport);
    require(client.list_prompts()["prompts"].size() == 1,
            "typed client list page failed");
    require(client.list_all_prompts().size() == 2,
            "typed client cursor pagination failed");
    require(client.get_prompt("greet")["messages"][0]["content"]["text"] == "Hello",
            "typed client prompt request failed");
    require(client.list_resources()["resources"].size() == 1,
            "typed client resource list failed");
    require(client.read_resource("demo://client")["contents"][0]["text"] == "read",
            "typed client resource read failed");
    client.close();
}

}  // namespace

int main() {
    try {
        exercise_protocol(false);
        exercise_protocol(true);
        exercise_typed_client();
    } catch (const std::exception& error) {
        std::cerr << "api consumer failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
