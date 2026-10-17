#include <iostream>
#include <string>

#include "cppmcp/client.hpp"
#include "cppmcp/transport/stdio.hpp"

int main(int argc, char** argv) {
    std::string command = argc > 1 ? argv[1] : "./cppmcp_stdio_server";
    cppmcp::Client client({"cppmcp-stdio-client", "stdio client", "0.1.0"});
    cppmcp::transport::StdioProcessSpec spec{command, {}, {}, ""};
    auto transport = std::make_shared<cppmcp::transport::StdioClientTransport>(spec);
    client.connect(transport);

    const auto discover = client.discover();
    std::cout << discover.dump() << '\n';

    const auto tools = client.list_tools();
    std::cout << tools.dump() << '\n';

    const auto result = client.call_tool("echo", { {"value", "hello"} });
    std::cout << result.dump() << '\n';

    client.close();
    return 0;
}
