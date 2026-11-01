#include "cppmcp/client.hpp"
#include "cppmcp/core/log.hpp"
#include "cppmcp/core/transport.hpp"
#include "cppmcp/server.hpp"
#include "cppmcp/server/features.hpp"
#include "cppmcp/transport/stdio.hpp"
#include "test.hpp"

#include <exception>
#include <condition_variable>
#include <mutex>
#include <string_view>

namespace {

void attach_server(const std::shared_ptr<cppmcp::core::MemoryTransport>& transport,
                   cppmcp::Server& server) {
    const std::weak_ptr<cppmcp::core::MemoryTransport> weak_transport = transport;
    transport->on_message([&server, weak_transport](const cppmcp::core::Message& message) {
        const auto response = server.handle_message(message);
        if (std::holds_alternative<cppmcp::core::ResultResponse>(response) ||
            std::holds_alternative<cppmcp::core::ErrorResponse>(response)) {
            if (const auto peer = weak_transport.lock()) {
                peer->send(response);
            }
        }
    });
}

void add_client_test_features(cppmcp::Server& server) {
    cppmcp::examples::FeatureOptions options;
    options.page_size = 1;
    server.features().configure(options);
    server.prompt("greet")
        .argument("name", "Name", "Name to greet", true,
                  cppmcp::examples::prefix_provider({"Ada", "Grace"}))
        .handler([](const cppmcp::examples::CallContext&, const nlohmann::json& args) {
            return cppmcp::PromptResult{{{"messages", nlohmann::json::array(
                {{{"role", "user"},
                  {"content", {{"type", "text"},
                               {"text", "Hello " + args.at("name").get<std::string>()}}}}})}}};
        });
    server.prompt("second").handler([](const cppmcp::examples::CallContext&,
                                       const nlohmann::json&) {
        return cppmcp::PromptResult{{"messages", nlohmann::json::array()}};
    });
    server.resource("demo://static")
        .name("static")
        .mime_type("text/plain")
        .handler([](const cppmcp::examples::CallContext&) {
            return cppmcp::ReadResult{{{"contents", nlohmann::json::array(
                {{{"uri", "demo://static"}, {"text", "static"}}})}}};
        });
    server.resource_template("demo://item/{id}")
        .name("item")
        .completion("id", cppmcp::examples::prefix_provider({"1", "42"}))
        .handler([](const cppmcp::examples::CallContext&,
                    const cppmcp::core::UriTemplate::Variables& variables) {
            const auto id = variables.at("id").get<std::string>();
            return cppmcp::ReadResult{{{"contents", nlohmann::json::array(
                {{{"uri", "demo://item/" + id}, {"text", id}}})}}};
        });
}

}  // namespace

void test_phase2_memory_roundtrip() {
    using namespace cppmcp;
    using namespace cppmcp::core;

    auto [client_transport, server_transport] = make_memory_transport_pair();
    Server server({"phase2-memory-server", "phase2 memory server", "0.1.0"});
    server.add_tool({
        .name = "echo",
        .title = "Echo",
        .description = "Echo a value.",
        .input_schema = nlohmann::json::object({{"type", "object"}}),
        .output_schema = nlohmann::json::object(),
        .annotations = ToolAnnotation{},
        .meta = nlohmann::json::object(),
        .handler = [](const nlohmann::json& args, const nlohmann::json&) {
            return args.value("value", "");
        },
    });

    server_transport->on_message([&](const Message& message) {
        const auto response = server.handle_message(message);
        if (std::holds_alternative<ResultResponse>(response) ||
            std::holds_alternative<ErrorResponse>(response)) {
            server_transport->send(response);
        }
    });

    Client client({"phase2-memory-client", "phase2 memory client", "0.1.0"});
    client.connect(client_transport);

    try {
        const auto discover = client.discover();
        if (!discover.contains("serverInfo")) {
            CPPMCP_LOG_ERROR("unexpected discover result: " << discover.dump());
        }
        CPPMCP_CHECK(discover.contains("serverInfo"));

        const auto tools = client.list_tools();
        CPPMCP_CHECK(tools.contains("tools"));
        CPPMCP_CHECK(tools["tools"].size() == 1);
        // Unset schema / meta must not be advertised as empty objects.
        CPPMCP_CHECK(!tools["tools"][0].contains("outputSchema"));
        CPPMCP_CHECK(!tools["tools"][0].contains("_meta"));

        const auto call = client.call_tool("echo", {{"value", "hello"}});
        CPPMCP_CHECK(call.contains("structuredContent"));
        CPPMCP_CHECK(call["structuredContent"] == "hello");
    } catch (const std::exception& e) {
        CPPMCP_LOG_ERROR("memory roundtrip threw: " << e.what());
        CPPMCP_CHECK(false);
    }
    client.close();
}

void test_phase2_stdio_roundtrip() {
    using namespace cppmcp;
    cppmcp::transport::StdioProcessSpec spec{"./cppmcp_stdio_server", {}, {}, "."};
    auto transport = std::make_shared<cppmcp::transport::StdioClientTransport>(spec);
    Client client({"phase2-stdio-client", "phase2 stdio client", "0.1.0"});

    try {
        client.connect(transport);

        const auto discover = client.discover();
        if (!discover.contains("serverInfo")) {
            CPPMCP_LOG_ERROR("unexpected discover result: " << discover.dump());
        }
        CPPMCP_CHECK(discover.contains("serverInfo"));

        const auto tools = client.list_tools();
        CPPMCP_CHECK(tools.contains("tools"));

        const auto call = client.call_tool("echo", {{"value", "stdio"}});
        CPPMCP_CHECK(call.contains("structuredContent"));
        CPPMCP_CHECK(call["structuredContent"] == "stdio");
    } catch (const std::exception& e) {
        CPPMCP_LOG_ERROR("stdio roundtrip threw: " << e.what());
        CPPMCP_CHECK(false);
    }
    client.close();
}

void test_client_api(std::string_view selected) {
    using namespace cppmcp;
    using namespace cppmcp::core;

    auto [client_transport, server_transport] = make_memory_transport_pair();
    Server server({"client-api-server", "client api server", "1.0"});
    add_client_test_features(server);
    attach_server(server_transport, server);
    Client client({"client-api-test", "client api test", "1.0"});
    client.connect(client_transport);

    if (selected == "client_prompts") {
        const auto page = client.list_prompts();
        CPPMCP_CHECK(page["prompts"].size() == 1);
        CPPMCP_CHECK(client.list_all_prompts().size() == 2);
        const auto prompt = client.get_prompt("greet", {{"name", "Ada"}});
        CPPMCP_CHECK(prompt["messages"][0]["content"]["text"] == "Hello Ada");
    } else if (selected == "client_resources") {
        CPPMCP_CHECK(client.list_resources()["resources"].size() == 1);
        CPPMCP_CHECK(client.list_all_resources().size() == 1);
        CPPMCP_CHECK(client.list_resource_templates()["resourceTemplates"].size() == 1);
        CPPMCP_CHECK(client.read_resource("demo://static")["contents"][0]["text"] == "static");
    } else if (selected == "client_completion") {
        const auto response = client.complete({
            {"ref", {{"type", "ref/prompt"}, {"name", "greet"}}},
            {"argument", {{"name", "name"}, {"value", "A"}}}});
        CPPMCP_CHECK(response["completion"]["values"] == nlohmann::json::array({"Ada"}));
    } else if (selected == "client_cache") {
        ClientCache cache(2);
        cache.put("entry", {{"value", 1}}, std::chrono::seconds(1));
        CPPMCP_CHECK(cache.get("entry").value()["value"] == 1);
        CPPMCP_CHECK(cache.size() == 1);
        cache.put("expired", true, std::chrono::milliseconds(0));
        CPPMCP_CHECK(!cache.get("expired").has_value());
        (void)client.list_prompts();
        CPPMCP_CHECK(client.cache().size() > 0);
        server_transport->send(Notification{
            "notifications/resources/updated", {{"uri", "demo://static"}}});
        CPPMCP_CHECK(client.cache().size() == 0);
    } else if (selected == "client_resubscribe") {
        std::mutex update_mutex;
        std::condition_variable update_cv;
        std::size_t updates = 0;
        transport::HttpOptions server_options;
        server_options.port = 0;
        server_options.mode = transport::HttpMode::modern;
        server_options.handler.mode = transport::HttpMode::modern;
        auto http_server = server.start_http(server_options);
        const auto make_http_client_options = [&] {
            HttpClientOptions options;
            options.url = "http://127.0.0.1:" + std::to_string(http_server->port()) + "/mcp";
            options.policy = NegotiationPolicy::modern_only;
            return options;
        };
        client.connect_http(make_http_client_options());
        client.subscribe_resource("demo://static", [&](const Notification&) {
            {
                std::lock_guard lock(update_mutex);
                ++updates;
            }
            update_cv.notify_one();
        });
        const auto wait_for_update = [&](std::size_t expected) {
            std::unique_lock lock(update_mutex);
            return update_cv.wait_for(lock, std::chrono::seconds(2),
                                      [&] { return updates >= expected; });
        };
        server.notify_resource_updated("demo://static");
        CPPMCP_CHECK(wait_for_update(1));
        client.connect_http(make_http_client_options());
        server.notify_resource_updated("demo://static");
        CPPMCP_CHECK(wait_for_update(2));
        client.close();
    }

    client.close();
}