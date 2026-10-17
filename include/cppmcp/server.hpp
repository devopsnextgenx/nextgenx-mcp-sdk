#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cppmcp/core/json_rpc.hpp"
#include "cppmcp/core/meta.hpp"
#include "cppmcp/core/version.hpp"

namespace cppmcp {

struct ServerInfo {
    std::string name;
    std::string title;
    std::string version;
};

struct ToolAnnotation {
    bool read_only_hint = false;
    bool destructive_hint = true;
    bool idempotent_hint = true;
    bool open_world_hint = false;
};

struct ToolDefinition {
    std::string name;
    std::string title;
    std::string description;
    nlohmann::json input_schema = nlohmann::json::object({{"type", "object"}});
    nlohmann::json output_schema = nlohmann::json::object();
    ToolAnnotation annotations;
    nlohmann::json meta = nlohmann::json::object();
    std::function<nlohmann::json(const nlohmann::json&, const nlohmann::json&)> handler;
};

struct ToolResult {
    nlohmann::json payload;
    bool is_error = false;

    static ToolResult text(std::string text) {
        return ToolResult{nlohmann::json{{"type", "text"}, {"text", std::move(text)}}, false};
    }
    static ToolResult structured(nlohmann::json value) {
        return ToolResult{std::move(value), false};
    }
    static ToolResult error_text(std::string error) {
        return ToolResult{nlohmann::json{{"type", "text"}, {"text", std::move(error)}}, true};
    }
};

class Server {
public:
    explicit Server(ServerInfo info = {"cppmcp", "cppmcp", "0.1.0"});

    void add_tool(ToolDefinition tool);
    bool has_tool(const std::string& name) const;
    std::vector<ToolDefinition> tools() const;

    nlohmann::json discover() const;
    core::Message handle_message(const core::Message& message);
    core::Message handle_request(const core::Request& request);
    core::Message handle_notification(const core::Notification& notification);

private:
    std::string request_protocol_version(const core::Json& params) const;
    std::string request_method_name(const core::Request& request) const;

    mutable std::mutex mutex_;
    ServerInfo info_;
    std::map<std::string, ToolDefinition> tools_;
};

}  // namespace cppmcp
