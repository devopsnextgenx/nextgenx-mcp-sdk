#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cppmcp/core/connection.hpp"
#include "cppmcp/core/transport.hpp"
#include "cppmcp/server.hpp"

namespace cppmcp {

enum class NegotiationPolicy {
    prefer_modern,
    modern_only,
    legacy_only,
};

struct ClientInfo {
    std::string name;
    std::string title;
    std::string version;
};

class Client {
public:
    Client(ClientInfo info = {"cppmcp-client", "cppmcp client", "0.1.0"},
           NegotiationPolicy policy = NegotiationPolicy::prefer_modern);

    void connect(std::shared_ptr<cppmcp::core::ITransport> transport);
    void close();
    void cancel(const cppmcp::core::RequestId& request_id);

    nlohmann::json discover();
    nlohmann::json list_tools();
    nlohmann::json call_tool(const std::string& name, const nlohmann::json& args = nlohmann::json::object());
    nlohmann::json ping();

private:
    ClientInfo info_;
    NegotiationPolicy policy_;
    std::shared_ptr<cppmcp::core::ITransport> transport_;
    std::shared_ptr<cppmcp::core::Connection> connection_;
};

}  // namespace cppmcp
