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
#include "cppmcp/core/request_state.hpp"
#include "cppmcp/core/version.hpp"
#include "cppmcp/transport/http_server.hpp"

namespace cppmcp {

namespace transport {
struct HttpOptions;
class McpHttpHandler;
class SubscriptionRegistry;
}  // namespace transport

namespace examples {
class FeatureRegistry;
class PromptBuilder;
class ResourceBuilder;
class ResourceTemplateBuilder;
}  // namespace examples

struct ServerInfo {
    std::string name;
    std::string title;
    std::string version;
};

class IRequestStateSubjectProvider {
public:
    virtual ~IRequestStateSubjectProvider() = default;
    virtual std::string subject(const core::Request& request) const = 0;
};

struct ServerOptions {
    core::SchemaValidationLimits schema_limits;
    std::vector<core::RequestStateKey> request_state_keys;
    std::chrono::seconds request_state_ttl{600};
    std::size_t max_request_state_bytes = 8192;
    std::size_t max_mrtr_rounds = 10;
    std::shared_ptr<const core::IRequestStateCodec> request_state_codec;
    std::shared_ptr<const IRequestStateSubjectProvider> subject_provider;
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

struct InputRequired {
    nlohmann::json input_requests = nlohmann::json::object();
    nlohmann::json private_state = nlohmann::json::object();

    nlohmann::json to_json() const {
        return {{"resultType", "input_required"},
                {"inputRequests", input_requests},
                {"privateState", private_state}};
    }
};

struct PromptResult {
    nlohmann::json payload = nlohmann::json::object();
};

struct ReadResult {
    nlohmann::json payload = nlohmann::json::object();
};

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
    static InputRequired input_required(nlohmann::json input_requests,
                                       nlohmann::json private_state = nlohmann::json::object()) {
        return {std::move(input_requests), std::move(private_state)};
    }
};

using ToolHandlerResult = std::variant<nlohmann::json, ToolResult, InputRequired>;
class ToolContext;

class ToolBuilder;

struct ToolDefinition {
    std::string name;
    std::string title;
    std::string description;
    nlohmann::json input_schema = nlohmann::json::object({{"type", "object"}});
    nlohmann::json output_schema = nlohmann::json::object();
    ToolAnnotation annotations;
    nlohmann::json meta = nlohmann::json::object();
    std::function<ToolHandlerResult(const nlohmann::json&, const nlohmann::json&)> handler;

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
                std::size_t maximum_notifications = 100,
                nlohmann::json client_capabilities = nlohmann::json::object());

    void emit(nlohmann::json event) const;
    void report_progress(double progress, std::optional<double> total = std::nullopt,
                         std::optional<std::string> message = std::nullopt) const;
    [[deprecated("MCP logging is deprecated in protocol 2026-07-28; use stderr or OpenTelemetry")]]
    void log(std::string_view level, std::string logger_name,
             nlohmann::json data) const;
    void set_log_level(std::string level);
    void finish() noexcept;
    [[nodiscard]] const nlohmann::json& input_responses() const noexcept;
    void set_input_responses(nlohmann::json responses);
    [[nodiscard]] const std::string& request_state() const noexcept;
    void set_request_state(std::string state);
    [[nodiscard]] const nlohmann::json& state() const noexcept;
    void set_state(nlohmann::json state);
    [[nodiscard]] const nlohmann::json& client_capabilities() const noexcept;
    void set_client_capabilities(nlohmann::json capabilities);
    void set_input_request_handler(std::function<nlohmann::json(const nlohmann::json&)> handler);
    nlohmann::json request_input(const nlohmann::json& request) const;
    bool can_request_input() const noexcept;
    void set_request_subject(std::string subject);
    const std::string& request_subject() const noexcept;
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
    nlohmann::json input_responses_ = nlohmann::json::object();
    nlohmann::json state_ = nlohmann::json::object();
    std::string request_state_;
    nlohmann::json client_capabilities_ = nlohmann::json::object();
    std::function<nlohmann::json(const nlohmann::json&)> input_request_handler_;
    std::string request_subject_ = "anonymous";
};

using ContextToolHandler = std::function<ToolHandlerResult(
    const nlohmann::json&, const nlohmann::json&, ToolContext&)>;
using RequestHandler =
    std::function<core::Message(const core::Request&, ToolContext&)>;

class Server {
public:
    explicit Server(ServerInfo info = {"cppmcp", "cppmcp", "0.1.0"},
                    core::SchemaValidationLimits schema_limits = {});
    Server(ServerInfo info, ServerOptions options);

    void add_tool(ToolDefinition tool);
    void add_tool(ToolDefinition tool, ContextToolHandler context_handler);
    void add_tool(ToolBuilder tool);
    void add_tool(ToolBuilder tool, ContextToolHandler context_handler);
    examples::PromptBuilder prompt(std::string name);
    examples::ResourceBuilder resource(std::string uri);
    examples::ResourceTemplateBuilder resource_template(std::string uri_template);
    examples::FeatureRegistry& features();
    void add_request_handler(std::string method, RequestHandler handler);
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
    core::Message handle_registered_request(const core::Request& request,
                                            ToolContext* context,
                                            const RequestHandler& handler);
    std::string request_protocol_version(const core::Json& params) const;
    std::string request_method_name(const core::Request& request) const;

    mutable std::mutex mutex_;
    ServerInfo info_;
    std::map<std::string, ToolDefinition> tools_;
    std::map<std::string, ContextToolHandler> context_handlers_;
    std::map<std::string, std::vector<HeaderParamDefinition>> header_params_;
    std::map<std::string, CompiledToolSchemas> compiled_tool_schemas_;
    std::map<std::string, RequestHandler> request_handlers_;
    std::shared_ptr<core::ISchemaValidator> schema_validator_;
    core::SchemaValidationLimits schema_limits_;
    std::atomic_bool validate_output_{true};
    std::optional<std::string> legacy_log_level_;
    std::atomic<std::shared_ptr<const core::IRequestStateCodec>> request_state_codec_;
    std::shared_ptr<const IRequestStateSubjectProvider> subject_provider_;
    std::size_t max_mrtr_rounds_ = 10;
    nlohmann::json legacy_client_capabilities_ = nlohmann::json::object();
    std::shared_ptr<transport::SubscriptionRegistry> subscriptions_;
    std::shared_ptr<examples::FeatureRegistry> feature_registry_;

    friend class transport::McpHttpHandler;
};

}  // namespace cppmcp
