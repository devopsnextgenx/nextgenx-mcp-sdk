#include "cppmcp/server.hpp"

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
    if (!tool.output_schema.is_null()) {
        item["outputSchema"] = tool.output_schema;
    }
    if (!tool.meta.is_null()) {
        item["_meta"] = tool.meta;
    }
    return item;
}

}  // namespace

Server::Server(ServerInfo info) : info_(std::move(info)) {}

void Server::add_tool(ToolDefinition tool) {
    std::lock_guard<std::mutex> lock(mutex_);
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
    return {{"protocolVersions", {"2026-07-28", "2025-11-25"}},
            {"capabilities", {"tools", {"listChanged", false}}},
            {"serverInfo", {"name", info_.name}, {"title", info_.title}, {"version", info_.version}}};
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
    if (notification.method == "notifications/initialized") {
        return core::Notification{"notifications/initialized", {}};
    }
    return core::ErrorResponse{std::nullopt,
                              core::protocol_error(core::ErrorCode::method_not_found,
                                                   "Notification method not implemented");
    };
}

core::Message Server::handle_request(const core::Request& request) {
    const auto method = request.method;
    const auto version = request_protocol_version(request.params);

    if (method == "server/discover") {
        return core::ResultResponse{request.id, discover()};
    }

    if (method == "initialize") {
        return core::ResultResponse{request.id,
                                    {{"protocolVersion", "2025-11-25"},
                                     {"capabilities", {{"tools", {{"listChanged", false}}}}},
                                     {"serverInfo",
                                      {{"name", info_.name},
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
        const auto name = request.params.value("name", "");
        if (name.empty()) {
            return core::ErrorResponse{request.id,
                                      core::protocol_error(core::ErrorCode::invalid_params,
                                                           "A tool name is required")};
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tools_.find(name.get<std::string>());
        if (it == tools_.end()) {
            return core::ErrorResponse{request.id,
                                      core::protocol_error(core::ErrorCode::method_not_found,
                                                           "Unknown tool")};
        }
        const auto result = it->second.handler(request.params.value("arguments", nlohmann::json::object()),
                                              request.params);
        nlohmann::json payload = {{"content", nlohmann::json::array({{ {"type", "text"}, {"text", result.dump()} }})}};
        if (!result.is_object() || !result.contains("isError")) {
            payload["structuredContent"] = result;
        }
        if (!version.empty()) {
            payload["resultType"] = "complete";
            payload["_meta"] = {{"io.modelcontextprotocol/serverInfo",
                                 {{"name", info_.name}, {"title", info_.title}, {"version", info_.version}}}};
        }
        return core::ResultResponse{request.id, std::move(payload)};
    }

    if (method == "notifications/cancelled") {
        return core::ErrorResponse{request.id,
                                  core::protocol_error(core::ErrorCode::method_not_found,
                                                       "Cancellation notification requires a live request context")};
    }

    return core::ErrorResponse{request.id,
                              core::protocol_error(core::ErrorCode::method_not_found,
                                                   "Unknown method")};
}

core::Message Server::handle_message(const core::Message& message) {
    return std::visit(
        [this](const auto& value) -> core::Message {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, core::Request>) {
                return handle_request(value);
            } else if constexpr (std::is_same_v<T, core::Notification>) {
                return handle_notification(value);
            } else {
                return value;
            }
        },
        message);
}

}  // namespace cppmcp
