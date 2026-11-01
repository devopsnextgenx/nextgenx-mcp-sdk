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
#include <string_view>
#include <unordered_map>
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

class ClientCache {
public:
    explicit ClientCache(std::size_t max_entries = 256);

    [[nodiscard]] std::optional<nlohmann::json> get(std::string_view key);
    void put(std::string key, nlohmann::json value, std::chrono::milliseconds ttl);
    void erase(std::string_view key);
    void clear();
    [[nodiscard]] std::size_t size() const;

private:
    struct Entry {
        nlohmann::json value;
        std::chrono::steady_clock::time_point expires;
    };

    std::size_t max_entries_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
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
        std::function<nlohmann::json(const std::string&, const nlohmann::json&)>
            on_input;
        nlohmann::json client_capabilities = nlohmann::json::object();
        bool allow_url_elicitation = false;
        std::size_t max_rounds = 10;
        std::chrono::milliseconds timeout{5000};
    };

    Client(ClientInfo info = {"cppmcp-client", "cppmcp client", "0.1.0"},
           NegotiationPolicy policy = NegotiationPolicy::prefer_modern);
    ~Client();

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
    void subscribe_resource(
        std::string uri,
        std::function<void(const core::Notification&)> callback);
    void unsubscribe_resource(std::string_view uri);

    nlohmann::json discover();
    nlohmann::json list_tools();
    nlohmann::json list_prompts(std::optional<std::string> cursor = std::nullopt);
    std::vector<nlohmann::json> list_all_prompts();
    nlohmann::json get_prompt(const std::string& name,
                              const nlohmann::json& arguments = nlohmann::json::object());
    nlohmann::json list_resources(std::optional<std::string> cursor = std::nullopt);
    std::vector<nlohmann::json> list_all_resources();
    nlohmann::json list_resource_templates(
        std::optional<std::string> cursor = std::nullopt);
    nlohmann::json read_resource(const std::string& uri);
    nlohmann::json complete(const nlohmann::json& params);
    [[nodiscard]] ClientCache& cache() noexcept;
    nlohmann::json call_tool(const std::string& name, const nlohmann::json& args = nlohmann::json::object());
    nlohmann::json call_tool(const std::string& name, const nlohmann::json& args,
                             CallToolOptions options);
    nlohmann::json ping();

private:
    nlohmann::json feature_request(const std::string& method,
                                  nlohmann::json params = nlohmann::json::object());
    std::vector<nlohmann::json> list_all(const std::string& method,
                                         std::string_view collection);
    void handle_server_notification(const core::Notification& notification);
    void handle_server_request(const core::Request& request);
    void refresh_resource_listener();
    void resubscribe_resources();

    struct ActiveToolCall {
        CallToolOptions options;
        std::size_t input_requests = 0;
    };

    ClientInfo info_;
    NegotiationPolicy policy_;
    std::shared_ptr<cppmcp::core::ITransport> transport_;
    std::shared_ptr<cppmcp::core::Connection> connection_;
    std::vector<std::function<void()>> listen_closers_;
    std::mutex resource_subscription_update_mutex_;
    std::mutex resource_subscription_mutex_;
    std::map<std::string, std::function<void(const core::Notification&)>>
        resource_callbacks_;
    std::shared_ptr<transport::ListenHandle> resource_listen_handle_;
    std::atomic<std::uint64_t> next_progress_token_{1};
    std::mutex tool_callbacks_mutex_;
    std::map<std::string, ActiveToolCall> active_tool_calls_;
    std::mutex legacy_tool_call_mutex_;
    std::mutex logging_tool_call_mutex_;
    ClientCache cache_;
    bool legacy_transport_ = false;
};

}  // namespace cppmcp
