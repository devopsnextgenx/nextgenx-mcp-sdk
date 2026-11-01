#include "cppmcp/server.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <array>
#include <set>
#include <stdexcept>
#include <string_view>

#include "cppmcp/core/log.hpp"
#include "cppmcp/server/features.hpp"
#include "cppmcp/transport/http.hpp"

namespace cppmcp {
namespace {

bool valid_header_param_name(std::string_view name) {
    constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
    return !name.empty() && std::all_of(name.begin(), name.end(), [&](unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= 'a' && ch <= 'z') ||
               punctuation.find(static_cast<char>(ch)) != std::string_view::npos;
    });
}

std::string lowercase_ascii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

int log_level_rank(std::string_view level) {
    constexpr std::array<std::pair<std::string_view, int>, 8> levels = {{
        {"emergency", 0}, {"alert", 1}, {"critical", 2}, {"error", 3},
        {"warning", 4}, {"notice", 5}, {"info", 6}, {"debug", 7},
    }};
    const auto match = std::find_if(levels.begin(), levels.end(), [&](const auto& entry) {
        return entry.first == level;
    });
    return match == levels.end() ? -1 : match->second;
}

nlohmann::json sanitize_log_value(const nlohmann::json& value) {
    if (value.is_object()) {
        auto sanitized = nlohmann::json::object();
        for (auto item = value.begin(); item != value.end(); ++item) {
            const auto key = lowercase_ascii(item.key());
            if (key.find("password") != std::string::npos ||
                key.find("secret") != std::string::npos ||
                key.find("token") != std::string::npos ||
                key.find("authorization") != std::string::npos ||
                key.find("api_key") != std::string::npos ||
                key.find("apikey") != std::string::npos) {
                sanitized[item.key()] = "[redacted]";
            } else {
                sanitized[item.key()] = sanitize_log_value(item.value());
            }
        }
        return sanitized;
    }
    if (value.is_array()) {
        auto sanitized = nlohmann::json::array();
        for (const auto& item : value) {
            sanitized.push_back(sanitize_log_value(item));
        }
        return sanitized;
    }
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        if (text.find_first_of("\r\n") != std::string::npos ||
            (!text.empty() && text.front() == '/') || text.find(":\\") != std::string::npos) {
            return "[redacted]";
        }
    }
    return value;
}

core::Json tool_payload(const ToolDefinition& tool) {
    core::Json item = {
        {"name", tool.name},
        {"description", tool.description},
        {"inputSchema", tool.input_schema},
        {"annotations", {
            {"readOnlyHint", tool.annotations.read_only_hint},
            {"destructiveHint", tool.annotations.destructive_hint},
            {"idempotentHint", tool.annotations.idempotent_hint},
            {"openWorldHint", tool.annotations.open_world_hint},
        }},
    };
    if (!tool.title.empty()) {
        item["title"] = tool.title;
    }
    // output_schema and meta default to empty objects; an empty outputSchema would
    // be read by clients as a declared schema, so only advertise them when set.
    if (!tool.output_schema.empty()) {
        item["outputSchema"] = tool.output_schema;
    }
    if (!tool.meta.empty()) {
        item["_meta"] = tool.meta;
    }
    return item;
}

}  // namespace

Server::CompiledToolSchemas Server::compile_tool_schemas(const ToolDefinition& tool) const {
    const auto valid_name_char = [](unsigned char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
               (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-';
    };
    if (tool.name.empty() || tool.name.size() > 128 ||
        !std::all_of(tool.name.begin(), tool.name.end(), [&](unsigned char ch) {
            return valid_name_char(ch);
        })) {
        throw std::invalid_argument(
            "tool name must match [A-Za-z0-9_.-]{1,128}");
    }
    if (!tool.input_schema.is_object() ||
        tool.input_schema.value("type", std::string{}) != "object") {
        throw std::invalid_argument("tool '" + tool.name +
                                    "' inputSchema must have root type 'object'");
    }
    CompiledToolSchemas schemas;
    try {
        schemas.input = schema_validator_->compile(tool.input_schema, schema_limits_);
        if (!tool.output_schema.empty()) {
            schemas.output = schema_validator_->compile(tool.output_schema, schema_limits_);
        }
    } catch (const std::exception& error) {
        throw std::invalid_argument("tool '" + tool.name + "' schema: " + error.what());
    }
    return schemas;
}

Server::Server(ServerInfo info, core::SchemaValidationLimits schema_limits)
    : info_(std::move(info)),
      schema_validator_(core::make_json_schema_validator()),
      schema_limits_(schema_limits),
      request_state_codec_(std::make_shared<core::HmacRequestStateCodec>(
          std::vector<core::RequestStateKey>{})),
      subscriptions_(std::make_shared<transport::SubscriptionRegistry>()),
      feature_registry_(std::make_shared<examples::FeatureRegistry>()) {
    feature_registry_->set_tool_executor(examples::make_tool_executor(*this));
    feature_registry_->set_list_changed_notifier([this](const std::string& kind) {
        if (kind == "prompts") notify_prompts_list_changed();
        else notify_resources_list_changed();
    });
}

Server::Server(ServerInfo info, ServerOptions options)
    : info_(std::move(info)),
      schema_validator_(core::make_json_schema_validator()),
      schema_limits_(options.schema_limits),
      request_state_codec_(options.request_state_codec ? std::move(options.request_state_codec) :
          std::make_shared<core::HmacRequestStateCodec>(
          std::move(options.request_state_keys), options.request_state_ttl,
          options.max_request_state_bytes)),
      subject_provider_(std::move(options.subject_provider)),
      max_mrtr_rounds_(options.max_mrtr_rounds),
      subscriptions_(std::make_shared<transport::SubscriptionRegistry>()),
      feature_registry_(std::make_shared<examples::FeatureRegistry>()) {
    if (max_mrtr_rounds_ == 0) {
        throw std::invalid_argument("max_mrtr_rounds must be greater than zero");
    }
    feature_registry_->set_tool_executor(examples::make_tool_executor(*this));
    feature_registry_->set_list_changed_notifier([this](const std::string& kind) {
        if (kind == "prompts") notify_prompts_list_changed();
        else notify_resources_list_changed();
    });
}

void Server::notify_tools_list_changed() {
    subscriptions_->notify_tools_list_changed();
}

void Server::notify_prompts_list_changed() {
    subscriptions_->notify_prompts_list_changed();
}

void Server::notify_resources_list_changed() {
    subscriptions_->notify_resources_list_changed();
}

void Server::notify_resource_updated(std::string uri) {
    if (feature_registry_) feature_registry_->resource_updated(uri);
    subscriptions_->notify_resource_updated(uri);
}

examples::FeatureRegistry& Server::features() {
    return *feature_registry_;
}

examples::PromptBuilder Server::prompt(std::string name) {
    return examples::PromptBuilder(features(), std::move(name));
}

examples::ResourceBuilder Server::resource(std::string uri) {
    return examples::ResourceBuilder(features(), std::move(uri));
}

examples::ResourceTemplateBuilder Server::resource_template(std::string uri_template) {
    return examples::ResourceTemplateBuilder(features(), std::move(uri_template));
}

ToolBuilder::ToolBuilder(ToolDefinition tool) : tool_(std::move(tool)) {}

ToolBuilder& ToolBuilder::header_param(std::string name, bool required) {
    header_params_.push_back({std::move(name), required});
    return *this;
}

ToolBuilder ToolDefinition::header_param(std::string name, bool required) & {
    return ToolBuilder(*this).header_param(std::move(name), required);
}

ToolBuilder ToolDefinition::header_param(std::string name, bool required) && {
    return ToolBuilder(std::move(*this)).header_param(std::move(name), required);
}

void ToolBuilder::validate_and_annotate() {
    if (header_params_.empty()) {
        return;
    }
    auto& schema = tool_.input_schema;
    if (!schema.is_object()) {
        throw std::invalid_argument(
            "header parameters require an object inputSchema with properties");
    }
    const auto type = schema.find("type");
    const auto properties = schema.find("properties");
    if (!schema.is_object() || type == schema.end() || !type->is_string() ||
        type->get<std::string>() != "object" || properties == schema.end() ||
        !properties->is_object()) {
        throw std::invalid_argument(
            "header parameters require an object inputSchema with properties");
    }

    auto required_it = schema.find("required");
    core::Json required = required_it == schema.end() ? core::Json::array() : *required_it;
    if (!required.is_array()) {
        throw std::invalid_argument("inputSchema.required must be an array");
    }
    std::set<std::string> names;
    for (auto& header_param : header_params_) {
        if (!valid_header_param_name(header_param.name) ||
            !names.insert(lowercase_ascii(header_param.name)).second) {
            throw std::invalid_argument("header parameter names must be unique HTTP tokens");
        }
        auto property = schema["properties"].find(header_param.name);
        if (property == schema["properties"].end() || !property->is_object() ||
            !property->contains("type") || !(*property)["type"].is_string() ||
            ((*property)["type"].get<std::string>() != "string" &&
             (*property)["type"].get<std::string>() != "integer" &&
             (*property)["type"].get<std::string>() != "boolean") ||
            property->contains("x-mcp-header")) {
            throw std::invalid_argument(
                "header parameters must name an unannotated string, integer, or boolean property");
        }
        header_param.type = (*property)["type"].get<std::string>();
        (*property)["x-mcp-header"] = header_param.name;
        const auto already_required = std::find(required.begin(), required.end(),
                                                core::Json(header_param.name)) != required.end();
        if (header_param.required && !already_required) {
            required.push_back(header_param.name);
        } else if (!header_param.required && already_required) {
            throw std::invalid_argument(
                "optional header parameters cannot be required by inputSchema");
        }
    }
    if (!required.empty() || schema.contains("required")) {
        schema["required"] = std::move(required);
    }
}

ToolContext::ToolContext(std::stop_token cancellation,
                         std::function<void(nlohmann::json)> emitter,
                         const nlohmann::json& request_params,
                         std::chrono::milliseconds minimum_notification_interval,
                                                 std::size_t maximum_notifications,
                                                 nlohmann::json client_capabilities)
    : cancellation_(cancellation),
      emitter_(std::move(emitter)),
      minimum_notification_interval_(minimum_notification_interval),
            maximum_notifications_(maximum_notifications),
            input_responses_(request_params.value("inputResponses", nlohmann::json::object())),
            request_state_(request_params.contains("requestState") && request_params["requestState"].is_string()
                         ? request_params["requestState"].get<std::string>() : std::string{}),
            client_capabilities_(std::move(client_capabilities)) {
        const auto meta = core::meta::get_meta(request_params);
        if (client_capabilities_.empty()) {
                client_capabilities_ = meta.value("io.modelcontextprotocol/clientCapabilities",
                                                                                 nlohmann::json::object());
        }
    const auto token = meta.find("progressToken");
    if (token != meta.end() && (token->is_string() || token->is_number_integer())) {
        progress_token_ = *token;
    }
    const auto level = meta.find("io.modelcontextprotocol/logLevel");
    if (level != meta.end() && level->is_string() &&
        log_level_rank(level->get_ref<const std::string&>()) >= 0) {
        log_level_ = level->get<std::string>();
    }
}

void ToolContext::emit(nlohmann::json event) const {
    if (!emitter_) {
        throw std::logic_error("request context does not support emissions");
    }
    std::lock_guard<std::mutex> lock(emission_mutex_);
    if (finished_ || cancelled() || notification_count_ >= maximum_notifications_) {
        return;
    }
    if (event.is_object() && event.value("method", std::string{}) == "notifications/message") {
        if (!log_level_) {
            return;
        }
        auto& params = event["params"];
        const auto level = params.value("level", std::string{});
        const auto message_rank = log_level_rank(level);
        const auto threshold_rank = log_level_rank(*log_level_);
        if (message_rank < 0 || threshold_rank < 0 || message_rank > threshold_rank) {
            return;
        }
        if (params.contains("data")) {
            params["data"] = sanitize_log_value(params["data"]);
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (notification_count_ > 0 && minimum_notification_interval_.count() > 0 &&
        now - last_notification_ < minimum_notification_interval_) {
        return;
    }
    emitter_(std::move(event));
    ++notification_count_;
    last_notification_ = now;
}

void ToolContext::report_progress(double progress, std::optional<double> total,
                                  std::optional<std::string> message) const {
    if (!progress_token_) {
        return;
    }
    if (!std::isfinite(progress) || (total && !std::isfinite(*total))) {
        throw std::invalid_argument("progress values must be finite numbers");
    }
    if (!emitter_) {
        throw std::logic_error("request context does not support emissions");
    }
    std::lock_guard<std::mutex> lock(emission_mutex_);
    if (finished_ || cancelled() || notification_count_ >= maximum_notifications_) {
        return;
    }
    if (last_progress_ && progress <= *last_progress_) {
        throw std::invalid_argument("progress values must strictly increase");
    }
    const auto now = std::chrono::steady_clock::now();
    if (notification_count_ > 0 && minimum_notification_interval_.count() > 0 &&
        now - last_notification_ < minimum_notification_interval_) {
        return;
    }
    nlohmann::json params = {{"progressToken", *progress_token_}, {"progress", progress}};
    if (total) {
        params["total"] = *total;
    }
    if (message) {
        params["message"] = std::move(*message);
    }
    emitter_({{"jsonrpc", "2.0"}, {"method", "notifications/progress"},
              {"params", std::move(params)}});
    last_progress_ = progress;
    ++notification_count_;
    last_notification_ = now;
}

void ToolContext::log(std::string_view level, std::string logger_name,
                      nlohmann::json data) const {
    emit({{"jsonrpc", "2.0"}, {"method", "notifications/message"},
          {"params", {{"level", level}, {"logger", std::move(logger_name)},
                       {"data", std::move(data)}}}});
}

void ToolContext::set_log_level(std::string level) {
    if (log_level_rank(level) < 0) {
        throw std::invalid_argument("unrecognized log level");
    }
    std::lock_guard<std::mutex> lock(emission_mutex_);
    log_level_ = std::move(level);
}

void ToolContext::finish() noexcept {
    std::lock_guard<std::mutex> lock(emission_mutex_);
    finished_ = true;
}

const nlohmann::json& ToolContext::input_responses() const noexcept {
    return input_responses_;
}

void ToolContext::set_input_responses(nlohmann::json responses) {
    input_responses_ = responses.is_object() ? std::move(responses)
                                             : nlohmann::json::object();
}

const std::string& ToolContext::request_state() const noexcept {
    return request_state_;
}

void ToolContext::set_request_state(std::string state) {
    request_state_ = std::move(state);
}

const nlohmann::json& ToolContext::state() const noexcept {
    return state_;
}

void ToolContext::set_state(nlohmann::json state) {
    state_ = std::move(state);
}

const nlohmann::json& ToolContext::client_capabilities() const noexcept {
    return client_capabilities_;
}

void ToolContext::set_client_capabilities(nlohmann::json capabilities) {
    client_capabilities_ = capabilities.is_object() ? std::move(capabilities)
                                                     : nlohmann::json::object();
}

bool ToolContext::cancelled() const noexcept {
    return cancellation_.stop_requested();
}

void ToolContext::set_input_request_handler(
    std::function<nlohmann::json(const nlohmann::json&)> handler) {
    input_request_handler_ = std::move(handler);
}

nlohmann::json ToolContext::request_input(const nlohmann::json& request) const {
    if (!input_request_handler_ || cancelled()) {
        throw std::runtime_error("Input request unavailable or cancelled");
    }
    return input_request_handler_(request);
}

bool ToolContext::can_request_input() const noexcept {
    return static_cast<bool>(input_request_handler_);
}

void ToolContext::set_request_subject(std::string subject) {
    if (subject.empty()) throw std::invalid_argument("Request-state subject must not be empty");
    request_subject_ = std::move(subject);
}

const std::string& ToolContext::request_subject() const noexcept {
    return request_subject_;
}

std::stop_token ToolContext::cancellation_token() const noexcept {
    return cancellation_;
}

void Server::add_tool(ToolDefinition tool) {
    const auto schemas = compile_tool_schemas(tool);
    std::lock_guard<std::mutex> lock(mutex_);
    CPPMCP_LOG_DEBUG("registering tool '" << tool.name << "'");
    context_handlers_.erase(tool.name);
    header_params_.erase(tool.name);
    compiled_tool_schemas_[tool.name] = schemas;
    tools_[tool.name] = std::move(tool);
}

void Server::add_tool(ToolDefinition tool, ContextToolHandler context_handler) {
    if (!context_handler) {
        throw std::invalid_argument("context tool handler must not be empty");
    }
    const auto schemas = compile_tool_schemas(tool);
    std::lock_guard<std::mutex> lock(mutex_);
    CPPMCP_LOG_DEBUG("registering context tool '" << tool.name << "'");
    header_params_.erase(tool.name);
    compiled_tool_schemas_[tool.name] = schemas;
    context_handlers_[tool.name] = std::move(context_handler);
    tools_[tool.name] = std::move(tool);
}

void Server::add_tool(ToolBuilder tool) {
    tool.validate_and_annotate();
    const auto schemas = compile_tool_schemas(tool.tool_);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto name = tool.tool_.name;
    CPPMCP_LOG_DEBUG("registering tool '" << name << "' with "
                                          << tool.header_params_.size()
                                          << " mirrored parameter(s)");
    context_handlers_.erase(name);
    compiled_tool_schemas_[name] = schemas;
    header_params_[name] = std::move(tool.header_params_);
    tools_[name] = std::move(tool.tool_);
}

void Server::add_tool(ToolBuilder tool, ContextToolHandler context_handler) {
    if (!context_handler) {
        throw std::invalid_argument("context tool handler must not be empty");
    }
    tool.validate_and_annotate();
    const auto schemas = compile_tool_schemas(tool.tool_);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto name = tool.tool_.name;
    CPPMCP_LOG_DEBUG("registering context tool '" << name << "' with "
                                                  << tool.header_params_.size()
                                                  << " mirrored parameter(s)");
    compiled_tool_schemas_[name] = schemas;
    context_handlers_[name] = std::move(context_handler);
    header_params_[name] = std::move(tool.header_params_);
    tools_[name] = std::move(tool.tool_);
}

void Server::add_request_handler(std::string method, RequestHandler handler) {
    if (method.empty() || !handler) {
        throw std::invalid_argument("request handler requires a method and callback");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    request_handlers_[std::move(method)] = std::move(handler);
}

void Server::set_validate_output(bool enabled) noexcept {
    validate_output_.store(enabled, std::memory_order_relaxed);
}

bool Server::has_tool(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tools_.find(name) != tools_.end();
}

std::vector<ToolDefinition> Server::tools() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ToolDefinition> result;
    result.reserve(tools_.size());
    for (const auto& [_, tool] : tools_) {
        result.push_back(tool);
    }
    return result;
}

std::vector<HeaderParamDefinition> Server::tool_header_params(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = header_params_.find(name);
    return it == header_params_.end() ? std::vector<HeaderParamDefinition>{} : it->second;
}

nlohmann::json Server::discover() const {
    nlohmann::json result = {
        {"protocolVersions", nlohmann::json::array({"2026-07-28", "2025-11-25"})},
        {"capabilities", {{"tools", {{"listChanged", true}}},
                          {"logging", nlohmann::json::object()}}},
        {"serverInfo", {{"name", info_.name}, {"title", info_.title}, {"version", info_.version}}}};
    feature_registry_->augment_discover(result);
    return result;
}

std::string Server::request_protocol_version(const core::Json& params) const {
    const auto meta = core::meta::get_meta(params);
    const auto it = meta.find("io.modelcontextprotocol/protocolVersion");
    if (it == meta.end() || !it.value().is_string()) {
        return "";
    }
    return it.value().get<std::string>();
}

std::string Server::request_method_name(const core::Request& request) const {
    return request.method;
}

core::Message Server::handle_notification(const core::Notification& notification) {
    CPPMCP_LOG_DEBUG("notification method=" << notification.method);
    if (notification.method == "notifications/initialized") {
        return core::Notification{"notifications/initialized", {}};
    }
    CPPMCP_LOG_WARN("unhandled notification method=" << notification.method);
    return core::ErrorResponse{
        std::nullopt,
        core::protocol_error(core::ErrorCode::method_not_found,
                             "Notification method not implemented")};
}

core::Message Server::handle_request(const core::Request& request, ToolContext* context) {
    const auto method = request.method;
    const auto version = request_protocol_version(request.params);
    CPPMCP_LOG_DEBUG("request method=" << method << " protocolVersion='" << version << "'");

    if (!version.empty()) {
        const auto meta = core::meta::get_meta(request.params);
        const auto level = meta.find("io.modelcontextprotocol/logLevel");
        if (level != meta.end() &&
            (!level->is_string() || log_level_rank(level->get<std::string>()) < 0)) {
            return core::ErrorResponse{
                request.id,
                core::protocol_error(core::ErrorCode::invalid_params,
                                     "Invalid logging level")};
        }
    }

    if (method == "server/discover") {
        return core::ResultResponse{request.id, discover()};
    }

    if (method == "initialize") {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            legacy_client_capabilities_ = request.params.value("capabilities", nlohmann::json::object());
        }
        nlohmann::json result = {{"protocolVersion", "2025-11-25"},
                                 {"capabilities", {{"tools", {{"listChanged", true}}},
                                                   {"logging", nlohmann::json::object()}}},
                                 {"serverInfo", {{"name", info_.name},
                                                 {"title", info_.title},
                                                 {"version", info_.version}}}};
        feature_registry_->augment_discover(result);
        return core::ResultResponse{request.id, std::move(result)};
    }

    if (method == "ping") {
        return core::ResultResponse{request.id, core::Json::object()};
    }

    if (method == "logging/setLevel") {
        if (!version.empty()) {
            return core::ErrorResponse{
                request.id,
                core::protocol_error(core::ErrorCode::method_not_found,
                                     "logging/setLevel is only available to legacy clients")};
        }
        const auto level = request.params.value("level", std::string{});
        if (log_level_rank(level) < 0) {
            return core::ErrorResponse{
                request.id,
                core::protocol_error(core::ErrorCode::invalid_params,
                                     "Invalid logging level")};
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            legacy_log_level_ = level;
        }
        return core::ResultResponse{request.id, core::Json::object()};
    }

    if (method == "tools/list") {
        std::lock_guard<std::mutex> lock(mutex_);
        nlohmann::json tools = nlohmann::json::array();
        for (const auto& [_, tool] : tools_) {
            tools.push_back(tool_payload(tool));
        }
        nlohmann::json result = {{"tools", std::move(tools)}};
        if (!version.empty()) {
            result["ttlMs"] = 1000;
            result["cacheScope"] = "private";
            result["resultType"] = "complete";
        }
        return core::ResultResponse{request.id, std::move(result)};
    }

    RequestHandler request_handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = request_handlers_.find(method);
        if (it != request_handlers_.end()) request_handler = it->second;
    }
    if (request_handler) {
        return handle_registered_request(request, context, request_handler);
    }

    if (feature_registry_) {
        if (auto feature_result = feature_registry_->try_handle(request, context)) {
            return std::move(*feature_result);
        }
    }

    if (method == "tools/call") {
        const auto name = request.params.value("name", std::string{});
        if (name.empty()) {
            CPPMCP_LOG_WARN("tools/call without a tool name");
            return core::ErrorResponse{request.id,
                                      core::protocol_error(core::ErrorCode::invalid_params,
                                                           "A tool name is required")};
        }

        // Copy the handler out so the server mutex is not held while user code runs
        // (a handler that calls add_tool(), or a slow one, must not block the server).
        decltype(ToolDefinition::handler) handler;
        ContextToolHandler context_handler;
        CompiledToolSchemas schemas;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = tools_.find(name);
            if (it != tools_.end()) {
                found = true;
                handler = it->second.handler;
                const auto contextual = context_handlers_.find(name);
                if (contextual != context_handlers_.end()) {
                    context_handler = contextual->second;
                }
                const auto compiled = compiled_tool_schemas_.find(name);
                if (compiled != compiled_tool_schemas_.end()) {
                    schemas = compiled->second;
                }
            }
        }
        if (!found) {
            CPPMCP_LOG_WARN("tools/call for unknown tool '" << name << "'");
            return core::ErrorResponse{request.id,
                                      core::protocol_error(core::ErrorCode::invalid_params,
                                                           "Unknown tool: " + name)};
        }

        auto finalize = [&](nlohmann::json payload) -> core::Message {
            if (!version.empty()) {
                payload["resultType"] = "complete";
                payload["_meta"] = {{"io.modelcontextprotocol/serverInfo",
                                     {{"name", info_.name}, {"title", info_.title}, {"version", info_.version}}}};
            }
            return core::ResultResponse{request.id, std::move(payload)};
        };

        ToolContext fallback({}, {}, request.params);
        auto& tool_context = context == nullptr ? fallback : *context;
        if (version.empty() && tool_context.client_capabilities().empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            tool_context.set_client_capabilities(legacy_client_capabilities_);
        }
        core::RequestStateBinding binding;
        const auto request_state_codec = request_state_codec_.load();
        binding.method = method;
        binding.name = name;
        binding.params = request.params;
        binding.subject = subject_provider_ ? subject_provider_->subject(request) : tool_context.request_subject();
        std::size_t round = 0;
        try {
            tool_context.set_request_state(request.params.value("requestState", std::string{}));
            const auto responses = request.params.value("inputResponses", nlohmann::json::object());
            if (!responses.is_object()) {
                throw std::invalid_argument("inputResponses must be an object");
            }
            if (request.params.contains("requestState")) {
                const auto claims = request_state_codec->verify(
                    request.params.at("requestState").get<std::string>(), binding);
                if (claims.round > max_mrtr_rounds_) {
                    throw std::invalid_argument("TooManyRounds: continuation limit reached");
                }
                round = claims.round;
                const auto& pending = claims.private_payload.at("inputRequests");
                for (auto response = responses.begin(); response != responses.end(); ++response) {
                    if (std::find(claims.input_request_ids.begin(), claims.input_request_ids.end(),
                                  response.key()) == claims.input_request_ids.end()) {
                        throw std::invalid_argument("Unknown input response ID");
                    }
                    const auto& value = response.value();
                    const auto& pending_request = pending.at(response.key());
                    const auto input_method = pending_request.at("method").get<std::string>();
                    if (input_method != "elicitation/create") {
                        if (!value.is_object() ||
                            (input_method == "roots/list" &&
                             (!value.contains("roots") || !value["roots"].is_array())) ||
                            (input_method == "sampling/createMessage" &&
                             (!value.contains("model") || !value["model"].is_string() ||
                              !value.contains("role") || !value["role"].is_string() ||
                              !value.contains("content")))) {
                            throw std::invalid_argument("Invalid input response");
                        }
                        continue;
                    }
                    if (!value.is_object() || !value.contains("action") ||
                        !value["action"].is_string()) {
                        throw std::invalid_argument("Input response requires an action");
                    }
                    const auto action = value["action"].get<std::string>();
                    if (action != "accept" && action != "decline" && action != "cancel") {
                        throw std::invalid_argument("Invalid elicitation action");
                    }
                    const auto& params = pending.at(response.key()).at("params");
                    if (action == "accept" && params.value("mode", std::string("form")) == "form") {
                        const auto validator = schema_validator_->compile(
                            params.at("requestedSchema"), schema_limits_);
                        if (!value.contains("content") || validator->validate(value.at("content"))) {
                            throw std::invalid_argument("Invalid elicitation response content");
                        }
                    }
                }
                tool_context.set_state(claims.private_payload.at("state"));
            } else if (!responses.empty()) {
                throw std::invalid_argument("inputResponses requires requestState");
            }
            tool_context.set_input_responses(responses);
        } catch (const std::exception&) {
            return core::ErrorResponse{request.id,
                core::protocol_error(core::ErrorCode::invalid_params,
                                     "Invalid tool continuation")};
        }

        CPPMCP_LOG_DEBUG("calling tool '" << name << "'");
        if (context != nullptr && version.empty()) {
            std::optional<std::string> level;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                level = legacy_log_level_;
            }
            if (level) {
                context->set_log_level(*level);
            }
        }
        const auto arguments = request.params.value("arguments", nlohmann::json::object());
        if (schemas.input) {
            const auto validation_error = schemas.input->validate(arguments);
            if (validation_error) {
                std::string message = "Invalid tool arguments";
                if (!validation_error->instance_path.empty()) {
                    message += " at " + validation_error->instance_path;
                }
                message += ": " + validation_error->message;
                return finalize({{"content", nlohmann::json::array(
                                               {{{"type", "text"}, {"text", message}}})},
                                 {"isError", true}});
            }
        }
        ToolHandlerResult result;
        try {
            if (context_handler) {
                result = context_handler(arguments, request.params, tool_context);
            } else {
                result = handler(arguments, request.params);
            }
        } catch (const std::exception&) {
            CPPMCP_LOG_ERROR("tool '" << name << "' execution failed");
            nlohmann::json content = nlohmann::json::array();
            content.push_back({{"type", "text"},
                               {"text", "Tool execution failed"}});
            return finalize({{"content", std::move(content)}, {"isError", true}});
        }

        nlohmann::json payload;
        bool is_error = false;
        if (auto* tool_result = std::get_if<ToolResult>(&result)) {
            payload = std::move(tool_result->payload);
            is_error = tool_result->is_error;
        } else if (const auto* input_required = std::get_if<InputRequired>(&result)) {
            payload = input_required->to_json();
        } else {
            auto raw_result = std::move(std::get<nlohmann::json>(result));
            payload = {{"content", nlohmann::json::array(
                                        {{{"type", "text"}, {"text", raw_result.dump()}}})},
                       {"structuredContent", std::move(raw_result)}};
        }
        if (payload.value("resultType", std::string{}) == "input_required") {
            try {
                const auto requests = payload.value("inputRequests", nlohmann::json::object());
                if (!requests.is_object()) {
                    throw std::invalid_argument("inputRequests must be an object");
                }
                const auto capabilities = tool_context.client_capabilities();
                const auto elicitation = capabilities.value("elicitation", nlohmann::json::object());
                for (auto input = requests.begin(); input != requests.end(); ++input) {
                    if (input.key().empty() || !input.value().is_object()) {
                        throw std::invalid_argument("Unsupported input request");
                    }
                    const auto input_method = input.value().value("method", std::string{});
                    if (input_method != "elicitation/create") {
                        const auto capability = input_method == "roots/list" ? "roots" :
                            input_method == "sampling/createMessage" ? "sampling" : "";
                        if (std::string_view(capability).empty()) {
                            throw std::invalid_argument("Unsupported input request method");
                        }
                        if (!capabilities.contains(capability) || !capabilities[capability].is_object()) {
                            return core::ErrorResponse{request.id,
                                core::protocol_error(core::ErrorCode::missing_required_client_capability,
                                    "Client does not support the required input method",
                                    {{"requiredCapabilities", {{capability, nlohmann::json::object()}}}})};
                        }
                        binding.input_request_ids.push_back(input.key());
                        continue;
                    }
                    const auto& params = input.value().at("params");
                    const auto mode = params.value("mode", std::string("form"));
                    if (mode != "form" && mode != "url") {
                        throw std::invalid_argument("Invalid elicitation mode");
                    }
                    if (!elicitation.is_object() || !elicitation.contains(mode) || !elicitation[mode].is_object()) {
                        return core::ErrorResponse{request.id,
                            core::protocol_error(core::ErrorCode::missing_required_client_capability,
                                "Client does not support the required elicitation mode",
                                {{"requiredCapabilities", {{"elicitation", {{mode, nlohmann::json::object()}}}}}})};
                    }
                    if (!params.contains("message") || !params["message"].is_string()) {
                        throw std::invalid_argument("Elicitation requires a message");
                    }
                    if (mode == "form") {
                        const auto& schema = params.at("requestedSchema");
                        core::validate_elicitation_form_schema(schema);
                        (void)schema_validator_->compile(schema, schema_limits_);
                    } else if (!params.contains("url") || !params["url"].is_string() ||
                               !params["url"].get_ref<const std::string&>().starts_with("https://")) {
                        throw std::invalid_argument("URL elicitation requires an HTTPS URL");
                    }
                    binding.input_request_ids.push_back(input.key());
                }
                if (round >= max_mrtr_rounds_) {
                    return core::ErrorResponse{request.id,
                        core::protocol_error(core::ErrorCode::invalid_params,
                                             "TooManyRounds: continuation limit reached")};
                }
                binding.round = round + 1;
                binding.private_payload = {
                    {"state", payload.value("privateState", tool_context.state())},
                    {"inputRequests", requests}};
                payload.erase("privateState");
                payload["requestState"] = request_state_codec->issue(binding);
                return core::ResultResponse{request.id, std::move(payload)};
            } catch (const std::exception&) {
                return core::ErrorResponse{request.id,
                    core::protocol_error(core::ErrorCode::internal_error,
                                         "Invalid input-required tool result")};
            }
        }
        if (is_error) {
            payload["isError"] = true;
        } else if (schemas.output && validate_output_.load(std::memory_order_relaxed)) {
            const auto structured = payload.find("structuredContent");
            const auto validation_error = structured == payload.end()
                                              ? std::optional<core::SchemaValidationError>(
                                                    core::SchemaValidationError{
                                                        {}, "structuredContent is missing"})
                                              : schemas.output->validate(*structured);
            if (validation_error) {
                CPPMCP_LOG_ERROR("tool '" << name << "' returned output that violates its schema");
                return core::ErrorResponse{
                    request.id,
                    core::protocol_error(core::ErrorCode::internal_error,
                                         "Tool output did not conform to its declared schema")};
            }
        }
        return finalize(std::move(payload));
    }

    if (method == "notifications/cancelled") {
        return core::ErrorResponse{request.id,
                                  core::protocol_error(core::ErrorCode::method_not_found,
                                                       "Cancellation notification requires a live request context")};
    }

    CPPMCP_LOG_WARN("unknown method '" << method << "'");
    return core::ErrorResponse{request.id,
                              core::protocol_error(core::ErrorCode::method_not_found,
                                                   "Unknown method")};
}

core::Message Server::handle_registered_request(const core::Request& request,
                                                ToolContext* context,
                                                const RequestHandler& handler) {
    ToolContext fallback({}, {}, request.params);
    auto& operation_context = context == nullptr ? fallback : *context;
    const auto version = request_protocol_version(request.params);
    auto capabilities = nlohmann::json::object();
    if (!version.empty()) {
        const auto meta = core::meta::get_meta(request.params);
        const auto found = meta.find("io.modelcontextprotocol/clientCapabilities");
        if (found != meta.end() && found->is_object()) capabilities = *found;
    } else {
        capabilities = operation_context.client_capabilities();
        if (capabilities.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            capabilities = legacy_client_capabilities_;
        }
    }
    operation_context.set_client_capabilities(capabilities);

    std::string target;
    if (request.method == "prompts/get") {
        target = request.params.value("name", std::string{});
    } else if (request.method == "resources/read") {
        target = request.params.value("uri", std::string{});
    }
    if (target.empty()) {
        return core::ErrorResponse{request.id,
            core::protocol_error(core::ErrorCode::invalid_params,
                                 "Operation target is required")};
    }

    core::RequestStateBinding binding;
    binding.method = request.method;
    binding.name = target;
    binding.params = request.params;
    binding.subject = subject_provider_ ? subject_provider_->subject(request)
                                        : operation_context.request_subject();
    const auto codec = request_state_codec_.load();
    nlohmann::json responses = nlohmann::json::object();
    std::size_t round = 0;
    try {
        if (request.params.contains("inputResponses")) {
            responses = request.params.at("inputResponses");
            if (!responses.is_object()) throw std::invalid_argument("inputResponses must be an object");
        }
        if (request.params.contains("requestState")) {
            const auto claims = codec->verify(
                request.params.at("requestState").get<std::string>(), binding);
            round = claims.round;
            const auto pending = claims.private_payload.at("inputRequests");
            for (auto response = responses.begin(); response != responses.end(); ++response) {
                if (!pending.contains(response.key()) || !response.value().is_object() ||
                    !response.value().contains("action") ||
                    !response.value()["action"].is_string()) {
                    throw std::invalid_argument("Invalid elicitation response");
                }
                const auto action = response.value()["action"].get<std::string>();
                if (action != "accept" && action != "decline" && action != "cancel") {
                    throw std::invalid_argument("Invalid elicitation action");
                }
                const auto& input_params = pending.at(response.key()).at("params");
                if (action == "accept" &&
                    input_params.value("mode", std::string("form")) == "form") {
                    const auto validator = schema_validator_->compile(
                        input_params.at("requestedSchema"), schema_limits_);
                    if (!response.value().contains("content") ||
                        validator->validate(response.value().at("content"))) {
                        throw std::invalid_argument("Invalid elicitation response content");
                    }
                }
            }
            operation_context.set_state(claims.private_payload.at("state"));
        } else if (!responses.empty()) {
            throw std::invalid_argument("inputResponses requires requestState");
        }
        operation_context.set_request_state(
            request.params.value("requestState", std::string{}));
        operation_context.set_input_responses(std::move(responses));
    } catch (const std::exception&) {
        return core::ErrorResponse{request.id,
            core::protocol_error(core::ErrorCode::invalid_params,
                                 "Invalid operation continuation")};
    }

    core::Message handled;
    try {
        handled = handler(request, operation_context);
    } catch (const std::exception&) {
        return core::ErrorResponse{request.id,
            core::protocol_error(core::ErrorCode::internal_error,
                                 "Request handler failed")};
    }
    if (const auto* error = std::get_if<core::ErrorResponse>(&handled)) return *error;
    const auto* completed = std::get_if<core::ResultResponse>(&handled);
    if (completed == nullptr || !completed->result.is_object()) {
        return core::ErrorResponse{request.id,
            core::protocol_error(core::ErrorCode::internal_error,
                                 "Request handler returned an invalid result")};
    }
    auto result = completed->result;

    if (result.value("resultType", std::string{}) == "input_required") {
        try {
            const auto requests = result.value("inputRequests", nlohmann::json::object());
            if (!requests.is_object() || requests.empty()) {
                throw std::invalid_argument("inputRequests must be a non-empty object");
            }
            const auto elicitation = capabilities.value("elicitation", nlohmann::json::object());
            for (auto input = requests.begin(); input != requests.end(); ++input) {
                if (input.key().empty() || !input.value().is_object() ||
                    input.value().value("method", std::string{}) != "elicitation/create" ||
                    !input.value().contains("params") || !input.value()["params"].is_object()) {
                    throw std::invalid_argument("Unsupported input request");
                }
                const auto& input_params = input.value()["params"];
                const auto mode = input_params.value("mode", std::string("form"));
                if (mode != "form" && mode != "url") {
                    throw std::invalid_argument("Invalid elicitation mode");
                }
                if (!elicitation.is_object() || !elicitation.contains(mode) ||
                    !elicitation[mode].is_object()) {
                    return core::ErrorResponse{request.id,
                        core::protocol_error(
                            core::ErrorCode::missing_required_client_capability,
                            "Client does not support the required elicitation mode",
                            {{"requiredCapabilities", {{"elicitation", {{mode, nlohmann::json::object()}}}}}})};
                }
                if (mode == "form") {
                    core::validate_elicitation_form_schema(input_params.at("requestedSchema"));
                    (void)schema_validator_->compile(input_params.at("requestedSchema"),
                                                     schema_limits_);
                } else if (!input_params.contains("url") ||
                           !input_params["url"].is_string() ||
                           !input_params["url"].get<std::string>().starts_with("https://")) {
                    throw std::invalid_argument("URL elicitation requires HTTPS");
                }
                binding.input_request_ids.push_back(input.key());
            }
            if (round >= max_mrtr_rounds_) {
                return core::ErrorResponse{request.id,
                    core::protocol_error(core::ErrorCode::invalid_params,
                                         "TooManyRounds: continuation limit reached")};
            }
            binding.round = round + 1;
            binding.private_payload = {
                {"state", result.value("privateState", operation_context.state())},
                {"inputRequests", requests}};
            result.erase("privateState");
            result["requestState"] = codec->issue(binding);
        } catch (const std::exception&) {
            return core::ErrorResponse{request.id,
                core::protocol_error(core::ErrorCode::internal_error,
                                     "Invalid input-required request result")};
        }
    } else if (!version.empty()) {
        result["resultType"] = "complete";
        result["_meta"] = {{"io.modelcontextprotocol/serverInfo",
                            {{"name", info_.name}, {"title", info_.title},
                             {"version", info_.version}}}};
    }
    return core::ResultResponse{request.id, std::move(result)};
}

core::Message Server::handle_message(const core::Message& message, ToolContext* context) {
    return std::visit(
        [this, context](const auto& value) -> core::Message {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, core::Request>) {
                auto response = handle_request(value, context);
                if (context == nullptr || !context->can_request_input() ||
                    !request_protocol_version(value.params).empty()) {
                    return response;
                }
                try {
                    for (std::size_t round = 0; round <= max_mrtr_rounds_; ++round) {
                        const auto* result = std::get_if<core::ResultResponse>(&response);
                        if (result == nullptr ||
                            result->result.value("resultType", std::string{}) != "input_required") {
                            return response;
                        }
                        auto retry = value;
                        retry.params["requestState"] = result->result.at("requestState");
                        auto responses = nlohmann::json::object();
                        const auto requests = result->result.value("inputRequests", nlohmann::json::object());
                        for (auto input = requests.begin(); input != requests.end(); ++input) {
                            auto legacy_input = input.value();
                            if (legacy_input.value("method", std::string{}) == "elicitation/create" &&
                                legacy_input["params"].value("mode", std::string("form")) == "url") {
                                legacy_input["params"]["elicitationId"] = input.key();
                            }
                            responses[input.key()] = context->request_input(legacy_input);
                        }
                        retry.params["inputResponses"] = std::move(responses);
                        response = handle_request(retry, context);
                    }
                    return core::ErrorResponse{value.id,
                        core::protocol_error(core::ErrorCode::invalid_params,
                                             "TooManyRounds: legacy input continuation limit reached")};
                } catch (const std::exception&) {
                    return core::ErrorResponse{value.id,
                        core::protocol_error(core::ErrorCode::internal_error,
                                             "Legacy input request failed or timed out")};
                }
            } else if constexpr (std::is_same_v<T, core::Notification>) {
                return handle_notification(value);
            } else {
                return value;
            }
        },
        message);
}

}  // namespace cppmcp