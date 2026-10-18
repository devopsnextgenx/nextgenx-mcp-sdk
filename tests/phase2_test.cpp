#include "cppmcp/client.hpp"
#include "cppmcp/core/log.hpp"
#include "cppmcp/core/transport.hpp"
#include "cppmcp/server.hpp"
#include "cppmcp/transport/stdio.hpp"
#include "test.hpp"

#include <exception>

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
        // structuredContent must be an object, so scalar results are wrapped.
        CPPMCP_CHECK(call["structuredContent"].is_object());
        CPPMCP_CHECK(call["structuredContent"]["result"] == "hello");
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
        CPPMCP_CHECK(call["structuredContent"].is_object());
        CPPMCP_CHECK(call["structuredContent"]["result"] == "stdio");
    } catch (const std::exception& e) {
        CPPMCP_LOG_ERROR("stdio roundtrip threw: " << e.what());
        CPPMCP_CHECK(false);
    }
    client.close();
}