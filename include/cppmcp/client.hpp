#pragma once

#include <chrono>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cppmcp/core/connection.hpp"
#include "cppmcp/core/transport.hpp"
#include "cppmcp/server.hpp"
#include "cppmcp/transport/http_client.hpp"

namespace cppmcp {

struct ClientInfo {
    std::string name;
    std::string title;
    std::string version;
};

class Client {
public:
    struct CallToolOptions {
        std::string log_level;
        std::function<void(double, std::optional<double>, std::optional<std::string>)>
            on_progress;
        std::function<void(const std::string&, const std::string&, const nlohmann::json&)>
            on_log;
        std::function<nlohmann::json(const std::string&, const nlohmann::json&)>
            on_elicit;
        std::size_t max_rounds = 10;
        std::chrono::milliseconds timeout{5000};
    };

    Client(ClientInfo info = {"cppmcp-client", "cppmcp client", "0.1.0"},
           NegotiationPolicy policy = NegotiationPolicy::prefer_modern);

    void connect(std::shared_ptr<cppmcp::core::ITransport> transport);
    void connect_http(HttpClientOptions options);
    void close();
    void cancel(const cppmcp::core::RequestId& request_id);
    [[nodiscard]] std::string http_mode() const;
    [[nodiscard]] std::shared_ptr<transport::ListenHandle> listen(
        nlohmann::json filter,
        std::function<void(const core::Notification&)> callback,
        transport::ListenCallbacks callbacks = {},
        transport::ListenOptions options = {});

    nlohmann::json discover();
    nlohmann::json list_tools();
    nlohmann::json call_tool(const std::string& name, const nlohmann::json& args = nlohmann::json::object());
    nlohmann::json call_tool(const std::string& name, const nlohmann::json& args,
                             CallToolOptions options);
    nlohmann::json ping();

private:
    void handle_server_notification(const core::Notification& notification);

    ClientInfo info_;
    NegotiationPolicy policy_;
    std::shared_ptr<cppmcp::core::ITransport> transport_;
    std::shared_ptr<cppmcp::core::Connection> connection_;
    std::vector<std::function<void()>> listen_closers_;
    std::atomic<std::uint64_t> next_progress_token_{1};
    std::mutex tool_callbacks_mutex_;
    std::map<std::string, CallToolOptions> active_tool_calls_;
};

}  // namespace cppmcp
