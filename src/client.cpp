#include "cppmcp/client.hpp"

#include <exception>
#include <stdexcept>

namespace cppmcp {

Client::Client(ClientInfo info, NegotiationPolicy policy)
    : info_(std::move(info)), policy_(policy) {}

void Client::connect(std::shared_ptr<cppmcp::core::ITransport> transport) {
    transport_ = std::move(transport);
    connection_ = std::make_shared<cppmcp::core::Connection>(transport_);
    connection_->set_notification_handler(
        [this](const core::Notification& notification) {
            handle_server_notification(notification);
        });
    connection_->start();
}

void Client::close() {
    for (const auto& close_listen : listen_closers_) {
        close_listen();
    }
    listen_closers_.clear();
    if (connection_) {
        connection_->close();
    }
}

void Client::cancel(const cppmcp::core::RequestId& request_id) {
    if (!connection_) {
        throw std::runtime_error("client is not connected");
    }
    connection_->cancel(request_id);
}

void Client::handle_server_notification(const core::Notification& notification) {
    if (notification.method == "notifications/progress") {
        const auto token = notification.params.find("progressToken");
        if (token == notification.params.end() || !token->is_string()) {
            return;
        }
        std::function<void(double, std::optional<double>, std::optional<std::string>)> callback;
        {
            std::lock_guard<std::mutex> lock(tool_callbacks_mutex_);
            const auto active = active_tool_calls_.find(token->get<std::string>());
            if (active != active_tool_calls_.end()) {
                callback = active->second.on_progress;
            }
        }
        if (callback) {
            const auto progress = notification.params.value("progress", 0.0);
            std::optional<double> total;
            if (notification.params.contains("total") &&
                notification.params["total"].is_number()) {
                total = notification.params["total"].get<double>();
            }
            std::optional<std::string> message;
            if (notification.params.contains("message") &&
                notification.params["message"].is_string()) {
                message = notification.params["message"].get<std::string>();
            }
            callback(progress, total, message);
        }
        return;
    }

    if (notification.method == "notifications/message") {
        const auto level = notification.params.value("level", std::string{});
        const auto logger = notification.params.value("logger", std::string{});
        const auto data = notification.params.value("data", nlohmann::json{});
        std::vector<std::function<void(const std::string&, const std::string&,
                                       const nlohmann::json&)>> callbacks;
        {
            std::lock_guard<std::mutex> lock(tool_callbacks_mutex_);
            for (const auto& [_, active] : active_tool_calls_) {
                if (!active.log_level.empty() && active.on_log) {
                    callbacks.push_back(active.on_log);
                }
            }
        }
        for (const auto& callback : callbacks) {
            callback(level, logger, data);
        }
    }
}

nlohmann::json Client::discover() {
    if (!connection_) {
        throw std::runtime_error("client is not connected");
    }
    auto future = connection_->send_request("server/discover", {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}, {"io.modelcontextprotocol/clientCapabilities", nlohmann::json::object()}, {"io.modelcontextprotocol/clientInfo", {{"name", info_.name}, {"title", info_.title}, {"version", info_.version}}}}}});
    return future.get();
}

nlohmann::json Client::list_tools() {
    if (!connection_) {
        throw std::runtime_error("client is not connected");
    }
    auto future = connection_->send_request("tools/list", {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}, {"io.modelcontextprotocol/clientCapabilities", nlohmann::json::object()}, {"io.modelcontextprotocol/clientInfo", {{"name", info_.name}, {"title", info_.title}, {"version", info_.version}}}}}});
    return future.get();
}

nlohmann::json Client::call_tool(const std::string& name, const nlohmann::json& args) {
    return call_tool(name, args, CallToolOptions{});
}

nlohmann::json Client::call_tool(const std::string& name, const nlohmann::json& args,
                                 CallToolOptions options) {
    if (!connection_) {
        throw std::runtime_error("client is not connected");
    }
    const auto call_id = "call-" + std::to_string(
        next_progress_token_.fetch_add(1, std::memory_order_relaxed));
    if (!options.client_capabilities.is_object() || options.timeout.count() <= 0) {
        throw std::invalid_argument("Tool call requires object capabilities and a positive timeout");
    }
    auto client_capabilities = options.client_capabilities;
    if (options.on_elicit) {
        client_capabilities["elicitation"] = {{"form", nlohmann::json::object()}};
        if (options.allow_url_elicitation) {
            client_capabilities["elicitation"]["url"] = nlohmann::json::object();
        }
    }
    nlohmann::json meta = {
        {"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
        {"io.modelcontextprotocol/clientCapabilities", std::move(client_capabilities)},
        {"io.modelcontextprotocol/clientInfo",
         {{"name", info_.name}, {"title", info_.title}, {"version", info_.version}}},
    };
    if (!options.log_level.empty()) {
        meta["io.modelcontextprotocol/logLevel"] = options.log_level;
    }
    if (options.on_progress) {
        meta["progressToken"] = call_id;
    }
    {
        std::lock_guard<std::mutex> lock(tool_callbacks_mutex_);
        active_tool_calls_.emplace(call_id, options);
    }
    try {
        nlohmann::json request_params = {
            {"name", name}, {"arguments", args}, {"_meta", std::move(meta)}};
        const auto original_params = request_params;
        const auto deadline = std::chrono::steady_clock::now() + options.timeout;
        for (std::size_t round = 0;; ++round) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                throw std::runtime_error("Tool call timed out during input continuation");
            }
            auto future = connection_->send_request("tools/call", request_params,
                                                    remaining);
            auto result = future.get();
            if (result.value("resultType", std::string("complete")) != "input_required") {
                std::lock_guard<std::mutex> lock(tool_callbacks_mutex_);
                active_tool_calls_.erase(call_id);
                return result;
            }
            const auto requests = result.value("inputRequests", nlohmann::json::object());
            if (!requests.is_object() ||
                (!result.contains("inputRequests") && !result.contains("requestState")) ||
                (result.contains("requestState") && !result["requestState"].is_string())) {
                throw std::runtime_error("Invalid input-required result");
            }
            if (!requests.empty() && !options.on_elicit && !options.on_input) {
                std::lock_guard<std::mutex> lock(tool_callbacks_mutex_);
                active_tool_calls_.erase(call_id);
                return result;
            }
            if (round >= options.max_rounds) {
                throw std::runtime_error("TooManyRounds: tool call exceeded its elicitation round limit");
            }

            nlohmann::json responses = nlohmann::json::object();
            for (auto item = requests.begin(); item != requests.end(); ++item) {
                if (options.on_input) {
                    responses[item.key()] = options.on_input(item.key(), item.value());
                } else if (item.value().value("method", std::string{}) == "elicitation/create") {
                    const auto& params = item.value().at("params");
                    const auto mode = params.value("mode", std::string("form"));
                    if (mode == "form") {
                        core::validate_elicitation_form_schema(params.at("requestedSchema"));
                    } else if (mode != "url" || !options.allow_url_elicitation) {
                        throw std::runtime_error("Unsupported elicitation mode");
                    }
                    responses[item.key()] = options.on_elicit(item.key(), item.value());
                } else {
                    throw std::runtime_error("No handler for input request method");
                }
            }
            request_params = original_params;
            if (!responses.empty()) {
                request_params["inputResponses"] = std::move(responses);
            }
            if (result.contains("requestState")) {
                request_params["requestState"] = result["requestState"];
            }
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(tool_callbacks_mutex_);
        active_tool_calls_.erase(call_id);
        throw;
    }
}

nlohmann::json Client::ping() {
    if (!connection_) {
        throw std::runtime_error("client is not connected");
    }
    auto future = connection_->send_request("ping", {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}, {"io.modelcontextprotocol/clientCapabilities", nlohmann::json::object()}, {"io.modelcontextprotocol/clientInfo", {{"name", info_.name}, {"title", info_.title}, {"version", info_.version}}}}}});
    return future.get();
}

}  // namespace cppmcp
