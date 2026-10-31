// stdio MCP client: spawns the stdio server and exercises tools, prompts, resources,
// completions and tasks.
//
// usage: cppmcp_stdio_client [server-command]

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <thread>

#include "cppmcp/client.hpp"
#include "cppmcp/core/log.hpp"
#include "cppmcp/transport/stdio.hpp"

#include "../common/client_tour.hpp"

int main(int argc, char** argv) {
    using namespace cppmcp::examples;
    cppmcp::core::log::configure(cppmcp::core::log::Level::info);

    std::string command = argc > 1 ? argv[1] : "./cppmcp_stdio_server";
    CPPMCP_LOG_INFO("connecting to server command: " << command);

    cppmcp::Client client({"cppmcp-stdio-client", "stdio client", "0.1.0"});
    cppmcp::transport::StdioProcessSpec spec{command, {}, {}, ""};
    auto stdio = std::make_shared<cppmcp::transport::StdioClientTransport>(spec);
    // Decorator that lets FeatureClient reach prompts/resources/completions/tasks.
    auto transport = std::make_shared<RawRequestTransport>(stdio);

    std::atomic<int> resource_updates{0};
    transport->set_notification_tap([&](const cppmcp::core::Notification& n) {
        if (n.method == "notifications/resources/updated") {
            ++resource_updates;
            std::cout << "NOTIFY " << n.method << ' ' << n.params.dump() << '\n';
        } else if (n.method == "notifications/tasks/status") {
            std::cout << "NOTIFY " << n.method << ' ' << n.params.value("taskId", std::string{}) << ' '
                      << n.params.value("status", std::string{}) << '\n';
        }
    });

    int exit_code = 0;
    try {
        client.connect(transport);
        std::cout << "discover -> " << client.discover().dump() << '\n';
        std::cout << "tools/list -> " << client.list_tools().dump() << '\n';

        run_tool_tour(client);

        FeatureClient features(client);
        run_feature_tour(features);

        // Resource subscription: the server touches demo://clock every second.
        step("resources/subscribe demo://clock", [&] { return features.subscribe("demo://clock"); });
        for (int i = 0; i < 30 && resource_updates.load() == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        step("resources/read demo://clock", [&] { return features.read_resource("demo://clock"); });
        step("resources/unsubscribe demo://clock", [&] { return features.unsubscribe("demo://clock"); });
        if (resource_updates.load() == 0) {
            CPPMCP_LOG_ERROR("no resources/updated notification received");
            exit_code = 1;
        }
    } catch (const std::exception& error) {
        CPPMCP_LOG_ERROR("request failed: " << error.what());
        exit_code = 1;
    }

    client.close();
    CPPMCP_LOG_INFO("done (exit code " << exit_code << ")");
    return exit_code;
}
