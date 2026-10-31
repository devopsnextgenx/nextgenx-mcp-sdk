#pragma once

// Client-side support for prompts, resources, completions and tasks.
//
// The public `cppmcp::Client` only exposes discover / list_tools / call_tool / ping, and
// its connection is private. To reach the other MCP methods without touching the SDK
// headers, this file provides:
//
//   * RawRequestTransport - an ITransport decorator. Client::call_tool("__cppmcp_raw__",
//     {{"method", M}, {"params", P}}) is rewritten *on the way out* into a plain request
//     for method M with params P. Everything the Client adds (the `_meta` block with
//     protocol version / client capabilities, ids, timeouts, correlation of the reply) is
//     kept, so the handshake and negotiation are still done by the SDK. The wrapped
//     transport sees an ordinary `prompts/list`, `resources/read`, ... request, so
//     transport-level header mirroring (HTTP) works as usual.
//     It can also tap inbound notifications (resources/updated, tasks/status, ...).
//
//   * FeatureClient - typed helpers on top of that tunnel.

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

#include "cppmcp/client.hpp"
#include "cppmcp/core/transport.hpp"

namespace cppmcp::examples {

using Json = nlohmann::json;

class RawRequestTransport final : public core::ITransport {
public:
    static constexpr const char* kSentinel = "__cppmcp_raw__";

    explicit RawRequestTransport(std::shared_ptr<core::ITransport> inner)
        : inner_(std::move(inner)) {
        inner_->on_message([this](core::Message message) {
            if (const auto* note = std::get_if<core::Notification>(&message)) {
                std::function<void(const core::Notification&)> tap;
                {
                    std::lock_guard lock(tap_mutex_);
                    tap = tap_;
                }
                if (tap) tap(*note);
            }
            if (on_message_) on_message_(std::move(message));
        });
        inner_->on_parse_error([this](core::RpcError e) { if (on_parse_error_) on_parse_error_(std::move(e)); });
        inner_->on_close([this](core::CloseReason r) { if (on_close_) on_close_(r); });
        inner_->on_error([this](std::string e) { if (on_error_) on_error_(std::move(e)); });
    }

    // Observe every server->client notification (runs on the transport's reader thread).
    void set_notification_tap(std::function<void(const core::Notification&)> tap) {
        std::lock_guard lock(tap_mutex_);
        tap_ = std::move(tap);
    }

    void start() override { inner_->start(); }
    void close() override { inner_->close(); }
    core::CancellationStrategy cancellation_strategy() const noexcept override {
        return inner_->cancellation_strategy();
    }
    void cancel_request(const core::RequestId& id) override { inner_->cancel_request(id); }

    void send(const core::Message& message) override { inner_->send(rewrite(message)); }

private:
    static core::Message rewrite(const core::Message& message) {
        const auto* request = std::get_if<core::Request>(&message);
        if (!request || request->method != "tools/call" || !request->params.is_object() ||
            request->params.value("name", std::string{}) != kSentinel) {
            return message;
        }
        const Json arguments = request->params.value("arguments", Json::object());
        core::Request out;
        out.id = request->id;
        out.method = arguments.value("method", std::string{});
        out.params = arguments.value("params", Json::object());
        if (!out.params.is_object()) out.params = Json::object();
        // Preserve the SDK-managed `_meta` (protocol version, capabilities, progress token...).
        if (request->params.contains("_meta") && request->params["_meta"].is_object()) {
            Json meta = request->params["_meta"];
            if (out.params.contains("_meta") && out.params["_meta"].is_object()) {
                meta.update(out.params["_meta"]);
            }
            out.params["_meta"] = std::move(meta);
        }
        return out;
    }

    std::shared_ptr<core::ITransport> inner_;
    std::mutex tap_mutex_;
    std::function<void(const core::Notification&)> tap_;
};

class FeatureClient {
public:
    explicit FeatureClient(Client& client) : client_(client) {}

    // Send any MCP request and return its `result`. Throws on JSON-RPC errors / timeouts.
    Json request(const std::string& method, Json params = Json::object(),
                 std::chrono::milliseconds timeout = std::chrono::seconds(15)) {
        Client::CallToolOptions options;
        options.timeout = timeout;
        return client_.call_tool(RawRequestTransport::kSentinel,
                                 {{"method", method}, {"params", std::move(params)}},
                                 std::move(options));
    }

    // ---- prompts ---------------------------------------------------------
    Json list_prompts(const std::string& cursor = {}) {
        return request("prompts/list", cursor_params(cursor));
    }
    Json get_prompt(const std::string& name, Json arguments = Json::object()) {
        return request("prompts/get", {{"name", name}, {"arguments", std::move(arguments)}});
    }

    // ---- resources -------------------------------------------------------
    Json list_resources(const std::string& cursor = {}) {
        return request("resources/list", cursor_params(cursor));
    }
    Json list_resource_templates(const std::string& cursor = {}) {
        return request("resources/templates/list", cursor_params(cursor));
    }
    Json read_resource(const std::string& uri) { return request("resources/read", {{"uri", uri}}); }
    Json subscribe(const std::string& uri) { return request("resources/subscribe", {{"uri", uri}}); }
    Json unsubscribe(const std::string& uri) { return request("resources/unsubscribe", {{"uri", uri}}); }

    // ---- completions -----------------------------------------------------
    Json complete_prompt(const std::string& prompt, const std::string& argument,
                         const std::string& typed, Json resolved = Json::object()) {
        return complete({{"type", "ref/prompt"}, {"name", prompt}}, argument, typed, std::move(resolved));
    }
    Json complete_resource(const std::string& uri_template, const std::string& variable,
                           const std::string& typed, Json resolved = Json::object()) {
        return complete({{"type", "ref/resource"}, {"uri", uri_template}}, variable, typed,
                        std::move(resolved));
    }

    // ---- tasks -----------------------------------------------------------
    // Runs `tools/call` as a task. Returns the `task` object (taskId, status, ...).
    Json create_task(const std::string& tool, Json arguments, std::int64_t ttl_ms = 60'000) {
        auto result = request("tools/call", {{"name", tool},
                                             {"arguments", std::move(arguments)},
                                             {"task", {{"ttl", ttl_ms}}}});
        if (!result.contains("task")) throw std::runtime_error("server did not create a task: " + result.dump());
        return result["task"];
    }
    Json get_task(const std::string& id) { return request("tasks/get", {{"taskId", id}}); }
    Json list_tasks(const std::string& cursor = {}) { return request("tasks/list", cursor_params(cursor)); }
    Json cancel_task(const std::string& id) { return request("tasks/cancel", {{"taskId", id}}); }
    // Blocks server-side until the task is terminal, then returns the tool's own result.
    Json task_result(const std::string& id, std::chrono::milliseconds timeout = std::chrono::seconds(70)) {
        return request("tasks/result", {{"taskId", id}}, timeout);
    }
    // Polls tasks/get honouring `pollInterval`; calls `on_status` on every observed change.
    Json wait_for_task(const std::string& id, std::chrono::milliseconds max_wait = std::chrono::seconds(30),
                       const std::function<void(const Json&)>& on_status = {}) {
        const auto deadline = std::chrono::steady_clock::now() + max_wait;
        std::string last;
        for (;;) {
            Json task = get_task(id);
            const auto status = task.value("status", std::string{});
            if (status != last) {
                last = status;
                if (on_status) on_status(task);
            }
            if (status == "completed" || status == "failed" || status == "cancelled") return task;
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("timed out waiting for task " + id);
            std::this_thread::sleep_for(std::chrono::milliseconds(
                std::max<std::int64_t>(task.value("pollInterval", 200), 50)));
        }
    }

private:
    static Json cursor_params(const std::string& cursor) {
        Json params = Json::object();
        if (!cursor.empty()) params["cursor"] = cursor;
        return params;
    }
    Json complete(Json ref, const std::string& argument, const std::string& typed, Json resolved) {
        Json params = {{"ref", std::move(ref)}, {"argument", {{"name", argument}, {"value", typed}}}};
        if (!resolved.empty()) params["context"] = {{"arguments", std::move(resolved)}};
        return request("completion/complete", std::move(params));
    }

    Client& client_;
};

}  // namespace cppmcp::examples
