#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

#include "cppmcp/server.hpp"
#include "cppmcp/transport/stdio.hpp"

int main() {
    cppmcp::Server server({"cppmcp-stdio-server", "stdio server", "0.1.0"});
    server.add_tool({
        .name = "echo",
        .title = "Echo",
        .description = "Echo a value back to the client.",
        .handler = [](const nlohmann::json& args, const nlohmann::json&) {
            return args.value("value", nlohmann::json(nullptr));
        },
    });
    server.add_tool({
        .name = "add",
        .title = "Add",
        .description = "Add two numbers.",
        .handler = [](const nlohmann::json& args, const nlohmann::json&) {
            const auto a = args.value("a", 0);
            const auto b = args.value("b", 0);
            return a + b;
        },
    });

    auto transport = std::make_shared<cppmcp::transport::StdioServerTransport>();
    transport->on_message([&](const cppmcp::core::Message& message) {
        auto response = server.handle_message(message);
        if (std::holds_alternative<cppmcp::core::ResultResponse>(response) ||
            std::holds_alternative<cppmcp::core::ErrorResponse>(response)) {
            transport->send(response);
        }
    });

    transport->start();
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
