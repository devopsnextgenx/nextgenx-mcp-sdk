#include "cppmcp/server.hpp"

#include <exception>
#include <stdexcept>

#include "cppmcp/core/log.hpp"

namespace cppmcp {
namespace {

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

Server::Server(ServerInfo info) : info_(std::move(info)) {}

ToolContext::ToolContext(std::stop_token cancellation,
                         std::function<void(nlohmann::json)> emitter)
    : cancellation_(cancellation), emitter_(std::move(emitter)) {}

void ToolContext::emit(nlohmann::json event) const {
    if (!emitter_) {
        throw std::logic_error("request context does not support emissions");
    }
    emitter_(std::move(event));
}

bool ToolContext::cancelled() const noexcept {
    return cancellation_.stop_requested();
}

std::stop_token ToolContext::cancellation_token() const noexcept {
    return cancellation_;
}

void Server::add_tool(ToolDefinition tool) {
    std::lock_guard<std::mutex> lock(mutex_);
    CPPMCP_LOG_DEBUG("registering tool '" << tool.name << "'");
    context_handlers_.erase(tool.name);
    tools_[tool.name] = std::move(tool);
}

void Server::add_tool(ToolDefinition tool, ContextToolHandler context_handler) {
    if (!context_handler) {
        throw std::invalid_argument("context tool handler must not be empty");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    CPPMCP_LOG_DEBUG("registering context tool '" << tool.name << "'");
    context_handlers_[tool.name] = std::move(context_handler);
    tools_[tool.name] = std::move(tool);
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

nlohmann::json Server::discover() const {
    return {{"protocolVersions", nlohmann::json::array({"2026-07-28", "2025-11-25"})},
            {"capabilities", {{"tools", {{"listChanged", false}}}}},
            {"serverInfo", {{"name", info_.name}, {"title", info_.title}, {"version", info_.version}}}};
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

    if (method == "server/discover") {
        return core::ResultResponse{request.id, discover()};
    }

    if (method == "initialize") {
        return core::ResultResponse{request.id,
                                    {{"protocolVersion", "2025-11-25"},
                                     {"capabilities", {{"tools", {{"listChanged", false}}}}},
                                     {"serverInfo", {{"name", info_.name},
                                                     {"title", info_.title},
                                                     {"version", info_.version}}}}};
    }

    if (method == "ping") {
        return core::ResultResponse{request.id, {}};
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
            }
        }
        if (!found) {
            CPPMCP_LOG_WARN("tools/call for unknown tool '" << name << "'");
            return core::ErrorResponse{request.id,
                                      core::protocol_error(core::ErrorCode::method_not_found,
                                                           "Unknown tool")};
        }

        auto finalize = [&](nlohmann::json payload) -> core::Message {
            if (!version.empty()) {
                payload["resultType"] = "complete";
                payload["_meta"] = {{"io.modelcontextprotocol/serverInfo",
                                     {{"name", info_.name}, {"title", info_.title}, {"version", info_.version}}}};
            }
            return core::ResultResponse{request.id, std::move(payload)};
        };

        CPPMCP_LOG_DEBUG("calling tool '" << name << "'");
        nlohmann::json result;
        try {
            const auto arguments = request.params.value("arguments", nlohmann::json::object());
            if (context_handler) {
                ToolContext fallback;
                result = context_handler(arguments, request.params,
                                         context == nullptr ? fallback : *context);
            } else {
                result = handler(arguments, request.params);
            }
        } catch (const std::exception& error) {
            CPPMCP_LOG_ERROR("tool '" << name << "' threw: " << error.what());
            nlohmann::json content = nlohmann::json::array();
            content.push_back({{"type", "text"},
                               {"text", std::string("Tool execution failed: ") + error.what()}});
            return finalize({{"content", std::move(content)}, {"isError", true}});
        }

        nlohmann::json content = nlohmann::json::array();
        content.push_back({{"type", "text"}, {"text", result.dump()}});
        nlohmann::json payload = {{"content", std::move(content)}};
        if (result.is_object()) {
            if (!result.contains("isError")) {
                payload["structuredContent"] = result;
            }
        } else {
            // structuredContent must be a JSON object, so wrap scalars/arrays/null.
            nlohmann::json wrapped = nlohmann::json::object();
            wrapped["result"] = result;
            payload["structuredContent"] = std::move(wrapped);
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

core::Message Server::handle_message(const core::Message& message, ToolContext* context) {
    return std::visit(
        [this, context](const auto& value) -> core::Message {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, core::Request>) {
                return handle_request(value, context);
            } else if constexpr (std::is_same_v<T, core::Notification>) {
                return handle_notification(value);
            } else {
                return value;
            }
        },
        message);
}

}  // namespace cppmcp