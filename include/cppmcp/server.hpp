#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cppmcp/core/json_rpc.hpp"
#include "cppmcp/core/json_schema.hpp"
#include "cppmcp/core/meta.hpp"
#include "cppmcp/core/version.hpp"
#include "cppmcp/transport/http_server.hpp"

namespace cppmcp {

namespace transport {
struct HttpOptions;
class McpHttpHandler;
class SubscriptionRegistry;
}  // namespace transport

struct ServerInfo {
    std::string name;
    std::string title;
    std::string version;
};

struct ToolAnnotation {
    bool read_only_hint = false;
    bool destructive_hint = false;
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
                std::function<void(nlohmann::json)> emitter,
                const nlohmann::json& request_params = nlohmann::json::object(),
                std::chrono::milliseconds minimum_notification_interval =
                    std::chrono::milliseconds(0),
                std::size_t maximum_notifications = 100);

    void emit(nlohmann::json event) const;
    void report_progress(double progress, std::optional<double> total = std::nullopt,
                         std::optional<std::string> message = std::nullopt) const;
    [[deprecated("MCP logging is deprecated in protocol 2026-07-28; use stderr or OpenTelemetry")]]
    void log(std::string_view level, std::string logger_name,
             nlohmann::json data) const;
    void set_log_level(std::string level);
    void finish() noexcept;
    bool cancelled() const noexcept;
    std::stop_token cancellation_token() const noexcept;

private:
    std::stop_token cancellation_;
    std::function<void(nlohmann::json)> emitter_;
    std::optional<nlohmann::json> progress_token_;
    std::optional<std::string> log_level_;
    std::chrono::milliseconds minimum_notification_interval_{0};
    std::size_t maximum_notifications_ = 100;
    mutable std::mutex emission_mutex_;
    mutable std::size_t notification_count_ = 0;
    mutable std::optional<double> last_progress_;
    mutable std::chrono::steady_clock::time_point last_notification_{};
    bool finished_ = false;
};

using ContextToolHandler =
    std::function<nlohmann::json(const nlohmann::json&, const nlohmann::json&, ToolContext&)>;

struct ToolResult {
    nlohmann::json payload;
    bool is_error = false;

    static ToolResult text(std::string text) {
        return ToolResult{nlohmann::json{{"content", nlohmann::json::array(
                                                      {{{"type", "text"},
                                                        {"text", std::move(text)}}})}},
                          false};
    }
    static ToolResult structured(nlohmann::json value, bool also_text = true) {
        auto payload = nlohmann::json::object();
        if (also_text) {
            payload["content"] = nlohmann::json::array(
                {{{"type", "text"}, {"text", value.dump()}}});
        } else {
            payload["content"] = nlohmann::json::array();
        }
        payload["structuredContent"] = std::move(value);
        return ToolResult{std::move(payload), false};
    }
    static ToolResult error_text(std::string error) {
        return ToolResult{nlohmann::json{{"content", nlohmann::json::array(
                                                      {{{"type", "text"},
                                                        {"text", std::move(error)}}})}},
                          true};
    }
    nlohmann::json to_json() const {
        return {{"__cppmcp_tool_result", true}, {"payload", payload}, {"isError", is_error}};
    }
    operator nlohmann::json() const { return to_json(); }
};

class Server {
public:
    explicit Server(ServerInfo info = {"cppmcp", "cppmcp", "0.1.0"},
                    core::SchemaValidationLimits schema_limits = {});

    void add_tool(ToolDefinition tool);
    void add_tool(ToolDefinition tool, ContextToolHandler context_handler);
    void add_tool(ToolBuilder tool);
    void add_tool(ToolBuilder tool, ContextToolHandler context_handler);
    void set_validate_output(bool enabled) noexcept;
    bool has_tool(const std::string& name) const;
    std::vector<ToolDefinition> tools() const;
    std::vector<HeaderParamDefinition> tool_header_params(const std::string& name) const;

    nlohmann::json discover() const;
    core::Message handle_message(const core::Message& message, ToolContext* context = nullptr);
    core::Message handle_request(const core::Request& request, ToolContext* context = nullptr);
    core::Message handle_notification(const core::Notification& notification);

    void notify_tools_list_changed();
    void notify_prompts_list_changed();
    void notify_resources_list_changed();
    void notify_resource_updated(std::string uri);

    void run_http(const transport::HttpOptions& options);
    std::unique_ptr<transport::HttpServerHandle> start_http(const transport::HttpOptions& options);

private:
    struct CompiledToolSchemas {
        std::shared_ptr<const core::ICompiledSchema> input;
        std::shared_ptr<const core::ICompiledSchema> output;
    };

    CompiledToolSchemas compile_tool_schemas(const ToolDefinition& tool) const;
    std::string request_protocol_version(const core::Json& params) const;
    std::string request_method_name(const core::Request& request) const;

    mutable std::mutex mutex_;
    ServerInfo info_;
    std::map<std::string, ToolDefinition> tools_;
    std::map<std::string, ContextToolHandler> context_handlers_;
    std::map<std::string, std::vector<HeaderParamDefinition>> header_params_;
    std::map<std::string, CompiledToolSchemas> compiled_tool_schemas_;
    std::shared_ptr<core::ISchemaValidator> schema_validator_;
    core::SchemaValidationLimits schema_limits_;
    std::atomic_bool validate_output_{true};
    std::optional<std::string> legacy_log_level_;
    std::shared_ptr<transport::SubscriptionRegistry> subscriptions_;

    friend class transport::McpHttpHandler;
};

}  // namespace cppmcp
