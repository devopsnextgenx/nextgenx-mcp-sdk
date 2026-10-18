#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cppmcp/core/json_rpc.hpp"
#include "cppmcp/core/meta.hpp"
#include "cppmcp/core/version.hpp"
#include "cppmcp/transport/http_server.hpp"

namespace cppmcp {

namespace transport {
struct HttpOptions;
}  // namespace transport

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

struct HeaderParamDefinition {
    std::string name;
    bool required = true;
    std::string type = "string";
};

class ToolBuilder;

struct ToolDefinition {
    std::string name;
    std::string title;
    std::string description;
    nlohmann::json input_schema = nlohmann::json::object({{"type", "object"}});
    nlohmann::json output_schema = nlohmann::json::object();
    ToolAnnotation annotations;
    nlohmann::json meta = nlohmann::json::object();
    std::function<nlohmann::json(const nlohmann::json&, const nlohmann::json&)> handler;

    ToolBuilder header_param(std::string name, bool required = true) &;
    ToolBuilder header_param(std::string name, bool required = true) &&;
};

class ToolBuilder {
public:
    explicit ToolBuilder(ToolDefinition tool);

    ToolBuilder& header_param(std::string name, bool required = true);

private:
    void validate_and_annotate();

    ToolDefinition tool_;
    std::vector<HeaderParamDefinition> header_params_;

    friend class Server;
};

class ToolContext {
public:
    ToolContext() = default;
    ToolContext(std::stop_token cancellation,
                std::function<void(nlohmann::json)> emitter);

    void emit(nlohmann::json event) const;
    bool cancelled() const noexcept;
    std::stop_token cancellation_token() const noexcept;

private:
    std::stop_token cancellation_;
    std::function<void(nlohmann::json)> emitter_;
};

using ContextToolHandler =
    std::function<nlohmann::json(const nlohmann::json&, const nlohmann::json&, ToolContext&)>;

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
    void add_tool(ToolDefinition tool, ContextToolHandler context_handler);
    void add_tool(ToolBuilder tool);
    void add_tool(ToolBuilder tool, ContextToolHandler context_handler);
    bool has_tool(const std::string& name) const;
    std::vector<ToolDefinition> tools() const;
    std::vector<HeaderParamDefinition> tool_header_params(const std::string& name) const;

    nlohmann::json discover() const;
    core::Message handle_message(const core::Message& message, ToolContext* context = nullptr);
    core::Message handle_request(const core::Request& request, ToolContext* context = nullptr);
    core::Message handle_notification(const core::Notification& notification);

    void run_http(const transport::HttpOptions& options);
    std::unique_ptr<transport::HttpServerHandle> start_http(const transport::HttpOptions& options);

private:
    std::string request_protocol_version(const core::Json& params) const;
    std::string request_method_name(const core::Request& request) const;

    mutable std::mutex mutex_;
    ServerInfo info_;
    std::map<std::string, ToolDefinition> tools_;
    std::map<std::string, ContextToolHandler> context_handlers_;
    std::map<std::string, std::vector<HeaderParamDefinition>> header_params_;
};

}  // namespace cppmcp
