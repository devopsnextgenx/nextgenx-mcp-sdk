#include <cppmcp/server.hpp>
#include <cppmcp/transport/http.hpp>

int main() {
    cppmcp::Server server;
    const auto encoded = cppmcp::transport::encode_mirrored_header_value("value");
    return encoded == "value" && !server.has_tool("absent") ? 0 : 1;
}
