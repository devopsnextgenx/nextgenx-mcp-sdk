#pragma once

// Client-side support for prompts / resources / completions / tasks.
//
// cppmcp::Client only knows tools. Rather than touch client.hpp, `SideChannel` decorates
// an ITransport: everything the stock Client sends and receives passes straight through,
// while `FeatureClient` uses the same channel to issue its own requests (ids prefixed
// "ext-", which the Client's Connection never sees) and tap server notifications.
//
//   auto stdio = std::make_shared<StdioClientTransport>(spec);
//   auto side  = std::make_shared<SideChannel>(stdio);
//   Client client;  client.connect(side);          // tools, progress, elicitation ...
//   FeatureClient features(side, meta);            // prompts, resources, tasks ...

#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "cppmcp/core/json_rpc.hpp"
#include "cppmcp/core/transport.hpp"
#include "cppmcp/core/version.hpp"

namespace cppmcp::examples {

using Json = nlohmann::json;

class RpcFailure : public std::runtime_error {
public:
    RpcFailure(std::int32_t code, const std::string& message, Json data = Json())
        : std::runtime_error("rpc error " + std::to_string(code) + ": " + message),
          code_(code), data_(std::move(data)) {}
    [[nodiscard]] std::int32_t code() const noexcept { return code_; }
    [[nodiscard]] const Json& data() const noexcept { return data_; }

private:
    std::int32_t code_;
    Json data_;
};

class SideChannel final : public core::ITransport {
public:
    explicit SideChannel(std::shared_ptr<core::ITransport> inner) : inner_(std::move(inner)) {
        // Hook the inner transport now, before anyone calls start().
        inner_->on_message([this](core::Message message) { dispatch(std::move(message)); });
        inner_->on_parse_error([this](core::RpcError error) {
            if (on_parse_error_) on_parse_error_(std::move(error));
        });
        inner_->on_error([this](std::string error) {
            if (on_error_) on_error_(std::move(error));
        });
        inner_->on_close([this](core::CloseReason reason) {
            fail_all("transport closed");
            if (on_close_) on_close_(reason);
        });
    }

    void start() override { inner_->start(); }
    void send(const core::Message& message) override { inner_->send(message); }
    void close() override {
        fail_all("transport closed");
        inner_->close();
    }
    core::CancellationStrategy cancellation_strategy() const noexcept override {
        return inner_->cancellation_strategy();
    }
    void cancel_request(const core::RequestId& id) override { inner_->cancel_request(id); }

    [[nodiscard]] const std::shared_ptr<core::ITransport>& inner() const noexcept { return inner_; }

    // Observe every server notification (they are still forwarded to the Client).
    void on_notification(std::function<void(const core::Notification&)> callback) {
        std::lock_guard lock(mutex_);
        notification_tap_ = std::move(callback);
    }

    // Send a request on the shared transport and wait for its response.
    Json call(const std::string& method, Json params = Json::object(),
              std::chrono::milliseconds timeout = std::chrono::seconds(15)) {
        const std::string id = "ext-" + std::to_string(next_id_.fetch_add(1));
        auto promise = std::make_shared<std::promise<Json>>();
        auto future = promise->get_future();
        {
            std::lock_guard lock(mutex_);
            pending_[id] = promise;  // register *before* sending: responses can be synchronous
        }
        try {
            send(core::Request{id, method, std::move(params)});
        } catch (...) {
            std::lock_guard lock(mutex_);
            pending_.erase(id);
            throw;
        }
        if (future.wait_for(timeout) != std::future_status::ready) {
            std::lock_guard lock(mutex_);
            pending_.erase(id);
            throw std::runtime_error("timed out waiting for " + method);
        }
        return future.get();  // rethrows RpcFailure
    }

private:
    static const std::string* ext_id(const core::RequestId& id) {
        const auto* text = std::get_if<std::string>(&id);
        return (text && text->rfind("ext-", 0) == 0) ? text : nullptr;
    }

    std::shared_ptr<std::promise<Json>> take(const std::string& id) {
        std::lock_guard lock(mutex_);
        const auto it = pending_.find(id);
        if (it == pending_.end()) return nullptr;
        auto promise = std::move(it->second);
        pending_.erase(it);
        return promise;
    }

    void fail_all(const std::string& reason) {
        std::map<std::string, std::shared_ptr<std::promise<Json>>> pending;
        {
            std::lock_guard lock(mutex_);
            pending.swap(pending_);
        }
        for (auto& [id, promise] : pending) {
            promise->set_exception(std::make_exception_ptr(std::runtime_error(reason)));
        }
    }

    void dispatch(core::Message message) {
        if (const auto* result = std::get_if<core::ResultResponse>(&message)) {
            if (const auto* id = ext_id(result->id)) {
                if (auto promise = take(*id)) promise->set_value(result->result);
                return;
            }
        } else if (const auto* error = std::get_if<core::ErrorResponse>(&message)) {
            if (error->id) {
                if (const auto* id = ext_id(*error->id)) {
                    if (auto promise = take(*id)) {
                        promise->set_exception(std::make_exception_ptr(RpcFailure(
                            error->error.code, error->error.message, error->error.data.value_or(Json()))));
                    }
                    return;
                }
            }
        } else if (const auto* notification = std::get_if<core::Notification>(&message)) {
            std::function<void(const core::Notification&)> tap;
            {
                std::lock_guard lock(mutex_);
                tap = notification_tap_;
            }
            if (tap) tap(*notification);
        }
        if (on_message_) on_message_(std::move(message));
    }

    std::shared_ptr<core::ITransport> inner_;
    std::atomic<std::uint64_t> next_id_{1};
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<std::promise<Json>>> pending_;
    std::function<void(const core::Notification&)> notification_tap_;
};

// `_meta` fields a modern-era request carries (see core/meta.hpp).
inline Json make_modern_meta(const Json& client_info, const Json& client_capabilities) {
    std::string version;
    for (const auto& known : core::known_protocol_versions()) {
        if (known.era == core::ProtocolEra::modern && known.value > version) version = known.value;
    }
    return {{"io.modelcontextprotocol/protocolVersion", version},
            {"io.modelcontextprotocol/clientInfo", client_info},
            {"io.modelcontextprotocol/clientCapabilities", client_capabilities}};
}

class FeatureClient {
public:
    explicit FeatureClient(std::shared_ptr<SideChannel> channel, Json meta = Json::object(),
                           std::chrono::milliseconds timeout = std::chrono::seconds(15))
        : channel_(std::move(channel)), meta_(std::move(meta)), timeout_(timeout) {}

    // Only for legacy-era connections that were not initialised by cppmcp::Client
    // (i.e. a standalone second HTTP connection).
    void legacy_initialize(const Json& client_info, const Json& capabilities) {
        std::string version;
        for (const auto& known : core::known_protocol_versions()) {
            if (known.era == core::ProtocolEra::legacy && known.value > version) version = known.value;
        }
        channel_->call("initialize", {{"protocolVersion", version},
                                      {"capabilities", capabilities},
                                      {"clientInfo", client_info}},
                       timeout_);
        channel_->send(core::Notification{"notifications/initialized", Json::object()});
    }

    // prompts
    Json list_prompts(const std::string& cursor = "") { return paged("prompts/list", cursor); }
    Json get_prompt(const std::string& name, Json arguments = Json::object()) {
        return call("prompts/get", {{"name", name}, {"arguments", std::move(arguments)}});
    }

    // resources
    Json list_resources(const std::string& cursor = "") { return paged("resources/list", cursor); }
    Json list_resource_templates(const std::string& cursor = "") {
        return paged("resources/templates/list", cursor);
    }
    Json read_resource(const std::string& uri) { return call("resources/read", {{"uri", uri}}); }
    // Legacy-style subscription (modern servers use subscriptions/listen via Client::listen).
    Json subscribe(const std::string& uri) { return call("resources/subscribe", {{"uri", uri}}); }
    Json unsubscribe(const std::string& uri) { return call("resources/unsubscribe", {{"uri", uri}}); }

    // completions
    Json complete_prompt(const std::string& prompt, const std::string& argument,
                         const std::string& value, Json resolved = Json::object()) {
        return complete({{"type", "ref/prompt"}, {"name", prompt}}, argument, value, std::move(resolved));
    }
    Json complete_resource(const std::string& uri_template, const std::string& variable,
                           const std::string& value, Json resolved = Json::object()) {
        return complete({{"type", "ref/resource"}, {"uri", uri_template}}, variable, value,
                        std::move(resolved));
    }
    Json complete(Json ref, const std::string& argument, const std::string& value,
                  Json resolved = Json::object()) {
        Json params = {{"ref", std::move(ref)}, {"argument", {{"name", argument}, {"value", value}}}};
        if (!resolved.empty()) params["context"] = {{"arguments", std::move(resolved)}};
        return call("completion/complete", std::move(params));
    }

    // tasks
    Json call_tool_as_task(const std::string& name, Json arguments = Json::object(),
                           std::optional<std::int64_t> ttl_ms = std::nullopt) {
        Json task = Json::object();
        if (ttl_ms) task["ttl"] = *ttl_ms;
        return call("tools/call", {{"name", name}, {"arguments", std::move(arguments)}, {"task", task}});
    }
    Json get_task(const std::string& id) { return call("tasks/get", {{"taskId", id}}); }
    Json list_tasks(const std::string& cursor = "") { return paged("tasks/list", cursor); }
    Json cancel_task(const std::string& id) { return call("tasks/cancel", {{"taskId", id}}); }
    // Blocks server-side until the task is terminal.
    Json task_result(const std::string& id, std::chrono::milliseconds wait = std::chrono::seconds(70)) {
        return call("tasks/result", {{"taskId", id}}, wait);
    }
    // Polls tasks/get at the server's suggested interval until terminal or timeout.
    Json wait_for_task(const std::string& id,
                       std::chrono::milliseconds timeout = std::chrono::seconds(30),
                       const std::function<void(const Json&)>& on_status = {}) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::string last_status;
        for (;;) {
            Json task = get_task(id);
            const auto status = task.value("status", std::string{});
            if (on_status && status != last_status) on_status(task);
            last_status = status;
            if (status == "completed" || status == "failed" || status == "cancelled") return task;
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("timed out waiting for task " + id);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(task.value("pollInterval", 200)));
        }
    }

    // Follow nextCursor and concatenate `key` arrays.
    Json list_all(const std::string& method, const std::string& key) {
        Json all = Json::array();
        std::string cursor;
        do {
            const Json page = paged(method, cursor);
            for (const auto& item : page.value(key, Json::array())) all.push_back(item);
            cursor = page.value("nextCursor", std::string{});
        } while (!cursor.empty());
        return all;
    }

    Json call(const std::string& method, Json params,
              std::optional<std::chrono::milliseconds> timeout = std::nullopt) {
        if (!meta_.empty() && !params.contains("_meta")) params["_meta"] = meta_;
        return channel_->call(method, std::move(params), timeout.value_or(timeout_));
    }

private:
    Json paged(const std::string& method, const std::string& cursor) {
        Json params = Json::object();
        if (!cursor.empty()) params["cursor"] = cursor;
        return call(method, std::move(params));
    }

    std::shared_ptr<SideChannel> channel_;
    Json meta_;
    std::chrono::milliseconds timeout_;
};

}  // namespace cppmcp::examples
