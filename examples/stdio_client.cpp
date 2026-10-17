#include <exception>
#include <memory>
#include <string>

#include "cppmcp/client.hpp"
#include "cppmcp/core/log.hpp"
#include "cppmcp/transport/stdio.hpp"

int main(int argc, char** argv) {
    cppmcp::core::log::configure(cppmcp::core::log::Level::info);

    std::string command = argc > 1 ? argv[1] : "./cppmcp_stdio_server";
    CPPMCP_LOG_INFO("connecting to server command: " << command);

    cppmcp::Client client({"cppmcp-stdio-client", "stdio client", "0.1.0"});
    cppmcp::transport::StdioProcessSpec spec{command, {}, {}, ""};
    auto transport = std::make_shared<cppmcp::transport::StdioClientTransport>(spec);

    int exit_code = 0;
    try {
        client.connect(transport);

        const auto discover = client.discover();
        CPPMCP_LOG_INFO("discover result: " << discover.dump());

        const auto tools = client.list_tools();
        CPPMCP_LOG_INFO("tools/list result: " << tools.dump());

        const auto resultecho = client.call_tool("echo", {{"value", "hello"}});
        CPPMCP_LOG_INFO("tools/call echo result: " << resultecho.dump());

        const auto resultadd = client.call_tool("add", {{"a", 15}, {"b", 7}});
        CPPMCP_LOG_INFO("tools/call add result: " << resultadd.dump());
    } catch (const std::exception& error) {
        CPPMCP_LOG_ERROR("request failed: " << error.what());
        exit_code = 1;
    }

    client.close();
    CPPMCP_LOG_INFO("done (exit code " << exit_code << ")");
    return exit_code;
}