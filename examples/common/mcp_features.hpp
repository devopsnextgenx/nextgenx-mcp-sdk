#pragma once

// Prompts, Resources, Resource Templates, Completions and Tasks for cppmcp servers.
//
// The public cppmcp headers expose only `Server::add_tool`; everything else a server
// speaks goes through `Server::handle_message`. This registry therefore sits *in front*
// of the Server: a transport calls `FeatureRegistry::try_handle(request)` first and only
// falls through to `Server::handle_message` when it returns std::nullopt.
//
// Wire shapes follow MCP 2025-11-25 (prompts/*, resources/*, completion/complete,
// tasks/* and task-augmented tools/call).

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "cppmcp/core/json_rpc.hpp"
#include "cppmcp/server.hpp"

namespace cppmcp::examples {

using Json = nlohmann::json;

// ---------------------------------------------------------------------------
// Definitions
// ---------------------------------------------------------------------------

struct PromptArgument {
    std::string name;
    std::string description;
    bool required = false;
};

struct PromptDefinition {
    std::string name;
    std::string title;
    std::string description;
    std::vector<PromptArgument> arguments;
    // Returns {"description": "...", "messages": [...]}.
    std::function<Json(const Json& arguments)> render;
};

struct ResourceContent {
    std::optional<std::string> text;
    std::optional<std::string> blob_base64;
    std::optional<std::string> mime_type;  // overrides the definition's mime type
};

struct ResourceDefinition {
    std::string uri;
    std::string name;
    std::string title;
    std::string description;
    std::string mime_type;
    std::function<ResourceContent()> read;
};

struct ResourceTemplateDefinition {
    std::string uri_template;  // e.g. "demo://cities/{city}"
    std::string name;
    std::string title;
    std::string description;
    std::string mime_type;
    // Return std::nullopt when the variables do not identify an existing resource.
    std::function<std::optional<ResourceContent>(const std::map<std::string, std::string>&)> read;
};

// (value typed so far, arguments already resolved by the client) -> candidate values.
using CompletionFn =
    std::function<std::vector<std::string>(const std::string& value, const Json& context)>;

struct FeatureOptions {
    std::size_t page_size = 50;
    std::size_t max_tasks = 32;
    std::int64_t default_task_ttl_ms = 60'000;
    std::int64_t max_task_ttl_ms = 3'600'000;
    std::int64_t task_poll_interval_ms = 200;
    std::chrono::milliseconds task_result_wait{60'000};
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace detail {

inline std::string iso8601(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

inline std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

inline std::string percent_decode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() && std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(std::string(in.substr(i + 1, 2)), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

// Matches RFC 6570 level-1 templates ("scheme://x/{a}/y/{b}"). A variable matches one
// non-empty path segment.
inline std::optional<std::map<std::string, std::string>> match_uri_template(
    std::string_view tmpl, std::string_view uri) {
    std::map<std::string, std::string> vars;
    std::size_t t = 0, u = 0;
    while (t < tmpl.size()) {
        if (tmpl[t] == '{') {
            const auto close = tmpl.find('}', t);
            if (close == std::string_view::npos) return std::nullopt;
            const std::string name(tmpl.substr(t + 1, close - t - 1));
            t = close + 1;
            const auto next_brace = tmpl.find('{', t);
            const auto literal = tmpl.substr(
                t, next_brace == std::string_view::npos ? std::string_view::npos : next_brace - t);
            std::size_t end = uri.size();
            if (!literal.empty()) {
                end = uri.find(literal, u);
                if (end == std::string_view::npos) return std::nullopt;
            }
            const auto value = uri.substr(u, end - u);
            if (value.empty() || value.find('/') != std::string_view::npos) return std::nullopt;
            vars[name] = percent_decode(value);
            u = end;
        } else {
            if (u >= uri.size() || uri[u] != tmpl[t]) return std::nullopt;
            ++t;
            ++u;
        }
    }
    if (u != uri.size()) return std::nullopt;
    return vars;
}

inline core::Message ok(const core::Request& request, Json result) {
    return core::ResultResponse{request.id, std::move(result)};
}

inline core::Message fail(const core::Request& request, std::int32_t code, std::string message,
                          std::optional<Json> data = std::nullopt) {
    return core::ErrorResponse{request.id, core::RpcError{code, std::move(message), std::move(data)}};
}

inline core::Message invalid_params(const core::Request& request, std::string message) {
    return fail(request, static_cast<std::int32_t>(core::ErrorCode::invalid_params),
                std::move(message));
}

inline core::Message internal_error(const core::Request& request, std::string message) {
    return fail(request, static_cast<std::int32_t>(core::ErrorCode::internal_error),
                std::move(message));
}

// Cursor == decimal offset. Returns std::nullopt for a malformed cursor.
inline std::optional<std::size_t> parse_cursor(const Json& params) {
    if (!params.is_object() || !params.contains("cursor") || params["cursor"].is_null()) return 0;
    if (!params["cursor"].is_string()) return std::nullopt;
    const auto text = params["cursor"].get<std::string>();
    std::size_t offset = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), offset);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return std::nullopt;
    return offset;
}

inline Json page(const std::vector<Json>& items, std::size_t offset, std::size_t page_size,
                 const char* key) {
    Json result = Json::object();
    Json slice = Json::array();
    const auto begin = std::min(offset, items.size());
    const auto end = std::min(items.size(), begin + page_size);
    for (auto i = begin; i < end; ++i) slice.push_back(items[i]);
    result[key] = std::move(slice);
    if (end < items.size()) result["nextCursor"] = std::to_string(end);
    return result;
}

inline void put_if(Json& target, const char* key, const std::string& value) {
    if (!value.empty()) target[key] = value;
}

inline bool is_terminal(const std::string& status) {
    return status == "completed" || status == "failed" || status == "cancelled";
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

class FeatureRegistry {
public:
    // Executes a (task-less) tools/call. Typically `make_tool_executor(server)`.
    using ToolExecutor = std::function<core::Message(const core::Request&, std::stop_token)>;
    // Pushes a server->client notification on the current connection (stdio).
    using Notifier = std::function<void(const std::string& method, const Json& params)>;

    explicit FeatureRegistry(FeatureOptions options = {}) : options_(options) {}
    ~FeatureRegistry() { shutdown(); }
    FeatureRegistry(const FeatureRegistry&) = delete;
    FeatureRegistry& operator=(const FeatureRegistry&) = delete;

    // -- registration ------------------------------------------------------
    void add_prompt(PromptDefinition prompt) {
        std::lock_guard lock(mutex_);
        prompts_.push_back(std::move(prompt));
    }
    void add_resource(ResourceDefinition resource) {
        std::lock_guard lock(mutex_);
        resources_.push_back(std::move(resource));
    }
    void add_resource_template(ResourceTemplateDefinition tmpl) {
        std::lock_guard lock(mutex_);
        templates_.push_back(std::move(tmpl));
    }
    void add_prompt_completion(std::string prompt, std::string argument, CompletionFn fn) {
        std::lock_guard lock(mutex_);
        prompt_completions_[{std::move(prompt), std::move(argument)}] = std::move(fn);
    }
    void add_resource_completion(std::string uri_template, std::string variable, CompletionFn fn) {
        std::lock_guard lock(mutex_);
        resource_completions_[{std::move(uri_template), std::move(variable)}] = std::move(fn);
    }
    void set_tool_executor(ToolExecutor executor) {
        std::lock_guard lock(mutex_);
        executor_ = std::move(executor);
    }
    void set_notifier(Notifier notifier) {
        std::lock_guard lock(mutex_);
        notifier_ = std::move(notifier);
    }

    // Call before tearing down whatever the notifier / executor refer to.
    void shutdown() {
        std::map<std::string, std::shared_ptr<TaskRecord>> tasks;
        {
            std::lock_guard lock(mutex_);
            notifier_ = nullptr;
            tasks.swap(tasks_);
        }
        tasks.clear();  // ~TaskRecord requests stop and joins the worker
    }

    // -- capability advertisement -----------------------------------------
    // Adds prompts/resources/completions/tasks to a discover/initialize result when it
    // looks like one (has "capabilities" and "serverInfo"). Existing entries are kept.
    void augment_discover(Json& result) const {
        if (!result.is_object() || !result.contains("capabilities") ||
            !result["capabilities"].is_object() || !result.contains("serverInfo")) {
            return;
        }
        auto& caps = result["capabilities"];
        std::lock_guard lock(mutex_);
        if (!prompts_.empty() && !caps.contains("prompts")) caps["prompts"] = {{"listChanged", true}};
        if ((!resources_.empty() || !templates_.empty()) && !caps.contains("resources")) {
            caps["resources"] = {{"subscribe", true}, {"listChanged", true}};
        }
        if (!caps.contains("completions")) caps["completions"] = Json::object();
        if (!caps.contains("tasks")) {
            caps["tasks"] = {{"list", Json::object()},
                             {"cancel", Json::object()},
                             {"requests", {{"tools", {{"call", Json::object()}}}}}};
        }
    }

    // -- server-side change notifications (stdio) -------------------------
    void resource_updated(const std::string& uri) {
        Notifier notifier;
        {
            std::lock_guard lock(mutex_);
            if (!subscriptions_.count(uri)) return;
            notifier = notifier_;
        }
        if (notifier) notifier("notifications/resources/updated", {{"uri", uri}});
    }

    // -- dispatch ----------------------------------------------------------
    std::optional<core::Message> try_handle(const core::Request& request) {
        const auto& m = request.method;
        try {
            if (m == "prompts/list") return prompts_list(request);
            if (m == "prompts/get") return prompts_get(request);
            if (m == "resources/list") return resources_list(request);
            if (m == "resources/templates/list") return templates_list(request);
            if (m == "resources/read") return resources_read(request);
            if (m == "resources/subscribe") return subscribe(request, true);
            if (m == "resources/unsubscribe") return subscribe(request, false);
            if (m == "completion/complete") return complete(request);
            if (m == "tasks/get") return tasks_get(request);
            if (m == "tasks/result") return tasks_result(request);
            if (m == "tasks/list") return tasks_list(request);
            if (m == "tasks/cancel") return tasks_cancel(request);
            if (m == "tools/call" && request.params.is_object() && request.params.contains("task") &&
                request.params["task"].is_object()) {
                return create_task(request);
            }
        } catch (const std::exception& error) {
            return detail::internal_error(request, error.what());
        }
        return std::nullopt;
    }

private:
    struct TaskRecord {
        std::string id;
        std::string status = "working";
        std::string status_message;
        std::chrono::system_clock::time_point created;
        std::chrono::system_clock::time_point updated;
        std::int64_t ttl_ms = 0;
        std::int64_t poll_interval_ms = 200;
        std::stop_source stop;
        std::mutex mutex;
        std::condition_variable cv;
        std::optional<core::Message> outcome;
        std::jthread worker;  // declared last: joined first on destruction

        ~TaskRecord() { stop.request_stop(); }

        Json to_json() const {  // caller holds `mutex`
            Json task = {{"taskId", id},
                         {"status", status},
                         {"createdAt", detail::iso8601(created)},
                         {"lastUpdatedAt", detail::iso8601(updated)},
                         {"ttl", ttl_ms},
                         {"pollInterval", poll_interval_ms}};
            detail::put_if(task, "statusMessage", status_message);
            return task;
        }
    };

    // ---- prompts ---------------------------------------------------------
    core::Message prompts_list(const core::Request& request) {
        const auto offset = detail::parse_cursor(request.params);
        if (!offset) return detail::invalid_params(request, "Invalid cursor");
        std::vector<Json> items;
        {
            std::lock_guard lock(mutex_);
            for (const auto& prompt : prompts_) {
                Json entry = {{"name", prompt.name}};
                detail::put_if(entry, "title", prompt.title);
                detail::put_if(entry, "description", prompt.description);
                Json args = Json::array();
                for (const auto& a : prompt.arguments) {
                    Json arg = {{"name", a.name}, {"required", a.required}};
                    detail::put_if(arg, "description", a.description);
                    args.push_back(std::move(arg));
                }
                entry["arguments"] = std::move(args);
                items.push_back(std::move(entry));
            }
        }
        return detail::ok(request, detail::page(items, *offset, options_.page_size, "prompts"));
    }

    core::Message prompts_get(const core::Request& request) {
        const auto name = request.params.value("name", std::string{});
        PromptDefinition prompt;
        {
            std::lock_guard lock(mutex_);
            const auto it = std::find_if(prompts_.begin(), prompts_.end(),
                                         [&](const auto& p) { return p.name == name; });
            if (it == prompts_.end()) {
                return detail::invalid_params(request, "Unknown prompt: " + name);
            }
            prompt = *it;
        }
        Json arguments = request.params.value("arguments", Json::object());
        if (!arguments.is_object()) return detail::invalid_params(request, "arguments must be an object");
        for (const auto& a : prompt.arguments) {
            if (a.required && !arguments.contains(a.name)) {
                return detail::invalid_params(request, "Missing required argument: " + a.name);
            }
        }
        Json rendered = prompt.render ? prompt.render(arguments) : Json::object();
        if (!rendered.contains("messages")) rendered["messages"] = Json::array();
        if (!rendered.contains("description") && !prompt.description.empty()) {
            rendered["description"] = prompt.description;
        }
        return detail::ok(request, std::move(rendered));
    }

    // ---- resources -------------------------------------------------------
    static Json content_json(const std::string& uri, const std::string& default_mime,
                             const ResourceContent& content) {
        Json out = {{"uri", uri}};
        const auto mime = content.mime_type ? *content.mime_type : default_mime;
        detail::put_if(out, "mimeType", mime);
        if (content.blob_base64) out["blob"] = *content.blob_base64;
        else out["text"] = content.text.value_or("");
        return out;
    }

    core::Message resources_list(const core::Request& request) {
        const auto offset = detail::parse_cursor(request.params);
        if (!offset) return detail::invalid_params(request, "Invalid cursor");
        std::vector<Json> items;
        {
            std::lock_guard lock(mutex_);
            for (const auto& r : resources_) {
                Json entry = {{"uri", r.uri}, {"name", r.name}};
                detail::put_if(entry, "title", r.title);
                detail::put_if(entry, "description", r.description);
                detail::put_if(entry, "mimeType", r.mime_type);
                items.push_back(std::move(entry));
            }
        }
        return detail::ok(request, detail::page(items, *offset, options_.page_size, "resources"));
    }

    core::Message templates_list(const core::Request& request) {
        const auto offset = detail::parse_cursor(request.params);
        if (!offset) return detail::invalid_params(request, "Invalid cursor");
        std::vector<Json> items;
        {
            std::lock_guard lock(mutex_);
            for (const auto& t : templates_) {
                Json entry = {{"uriTemplate", t.uri_template}, {"name", t.name}};
                detail::put_if(entry, "title", t.title);
                detail::put_if(entry, "description", t.description);
                detail::put_if(entry, "mimeType", t.mime_type);
                items.push_back(std::move(entry));
            }
        }
        return detail::ok(request,
                          detail::page(items, *offset, options_.page_size, "resourceTemplates"));
    }

    core::Message resources_read(const core::Request& request) {
        const auto uri = request.params.value("uri", std::string{});
        if (uri.empty()) return detail::invalid_params(request, "uri is required");
        std::function<ResourceContent()> reader;
        std::string mime;
        std::function<std::optional<ResourceContent>()> template_reader;
        {
            std::lock_guard lock(mutex_);
            for (const auto& r : resources_) {
                if (r.uri == uri) {
                    reader = r.read;
                    mime = r.mime_type;
                    break;
                }
            }
            if (!reader) {
                for (const auto& t : templates_) {
                    if (auto vars = detail::match_uri_template(t.uri_template, uri)) {
                        mime = t.mime_type;
                        template_reader = [fn = t.read, v = std::move(*vars)] { return fn(v); };
                        break;
                    }
                }
            }
        }
        std::optional<ResourceContent> content;
        if (reader) content = reader();
        else if (template_reader) content = template_reader();
        if (!content) {
            return detail::fail(request, -32002, "Resource not found", Json{{"uri", uri}});
        }
        return detail::ok(request, {{"contents", Json::array({content_json(uri, mime, *content)})}});
    }

    core::Message subscribe(const core::Request& request, bool on) {
        const auto uri = request.params.value("uri", std::string{});
        if (uri.empty()) return detail::invalid_params(request, "uri is required");
        std::lock_guard lock(mutex_);
        if (on) subscriptions_.insert(uri);
        else subscriptions_.erase(uri);
        return detail::ok(request, Json::object());
    }

    // ---- completion ------------------------------------------------------
    core::Message complete(const core::Request& request) {
        const auto& p = request.params;
        if (!p.contains("ref") || !p["ref"].is_object() || !p.contains("argument") ||
            !p["argument"].is_object()) {
            return detail::invalid_params(request, "ref and argument are required");
        }
        const auto ref_type = p["ref"].value("type", std::string{});
        const auto arg_name = p["argument"].value("name", std::string{});
        const auto value = p["argument"].value("value", std::string{});
        const Json context = p.contains("context") && p["context"].is_object()
                                 ? p["context"].value("arguments", Json::object())
                                 : Json::object();
        CompletionFn fn;
        {
            std::lock_guard lock(mutex_);
            if (ref_type == "ref/prompt") {
                const auto name = p["ref"].value("name", std::string{});
                const bool known = std::any_of(prompts_.begin(), prompts_.end(),
                                               [&](const auto& pr) { return pr.name == name; });
                if (!known) return detail::invalid_params(request, "Unknown prompt: " + name);
                if (auto it = prompt_completions_.find({name, arg_name}); it != prompt_completions_.end())
                    fn = it->second;
            } else if (ref_type == "ref/resource") {
                const auto uri = p["ref"].value("uri", std::string{});
                const bool known = std::any_of(templates_.begin(), templates_.end(), [&](const auto& t) {
                    return t.uri_template == uri;
                });
                if (!known) return detail::invalid_params(request, "Unknown resource template: " + uri);
                if (auto it = resource_completions_.find({uri, arg_name}); it != resource_completions_.end())
                    fn = it->second;
            } else {
                return detail::invalid_params(request, "Unsupported ref type: " + ref_type);
            }
        }
        std::vector<std::string> values = fn ? fn(value, context) : std::vector<std::string>{};
        constexpr std::size_t kMax = 100;  // MCP limit per response
        const auto total = values.size();
        if (values.size() > kMax) values.resize(kMax);
        return detail::ok(request, {{"completion",
                                     {{"values", values},
                                      {"total", total},
                                      {"hasMore", total > kMax}}}});
    }

    // ---- tasks -----------------------------------------------------------
    static std::string new_task_id() {
        static std::mutex m;
        static std::mt19937_64 rng{std::random_device{}()};
        std::lock_guard lock(m);
        char buffer[33];
        std::snprintf(buffer, sizeof buffer, "%016llx%016llx",
                      static_cast<unsigned long long>(rng()), static_cast<unsigned long long>(rng()));
        return buffer;
    }

    // Expired records are moved to `graveyard` instead of destroyed: ~TaskRecord joins the
    // worker, which may itself be waiting for `mutex_`. Callers declare `graveyard`
    // *before* taking the lock so it is destroyed after the lock is released.
    void purge_expired_locked(std::vector<std::shared_ptr<TaskRecord>>& graveyard) {
        const auto now = std::chrono::system_clock::now();
        for (auto it = tasks_.begin(); it != tasks_.end();) {
            bool drop = false;
            {
                std::lock_guard task_lock(it->second->mutex);
                drop = detail::is_terminal(it->second->status) &&
                       now > it->second->created + std::chrono::milliseconds(it->second->ttl_ms);
            }
            if (drop) {
                graveyard.push_back(std::move(it->second));
                it = tasks_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::shared_ptr<TaskRecord> find_task(const std::string& id) {
        std::vector<std::shared_ptr<TaskRecord>> graveyard;
        std::lock_guard lock(mutex_);
        purge_expired_locked(graveyard);
        const auto it = tasks_.find(id);
        return it == tasks_.end() ? nullptr : it->second;
    }

    void emit_status(const Json& task) {
        Notifier notifier;
        {
            std::lock_guard lock(mutex_);
            notifier = notifier_;
        }
        if (notifier) notifier("notifications/tasks/status", task);
    }

    core::Message create_task(const core::Request& request) {
        const auto& task_params = request.params["task"];
        std::int64_t ttl = options_.default_task_ttl_ms;
        if (task_params.contains("ttl")) {
            if (!task_params["ttl"].is_number_integer() || task_params["ttl"].get<std::int64_t>() < 0) {
                return detail::invalid_params(request, "task.ttl must be a non-negative integer");
            }
            ttl = std::min(task_params["ttl"].get<std::int64_t>(), options_.max_task_ttl_ms);
        }

        ToolExecutor executor;
        std::shared_ptr<TaskRecord> record;
        std::vector<std::shared_ptr<TaskRecord>> graveyard;
        {
            std::lock_guard lock(mutex_);
            if (!executor_) return detail::internal_error(request, "Task execution is not configured");
            executor = executor_;
            purge_expired_locked(graveyard);
            std::size_t active = 0;
            for (auto& [id, t] : tasks_) {
                std::lock_guard task_lock(t->mutex);
                if (!detail::is_terminal(t->status)) ++active;
            }
            if (active >= options_.max_tasks) {
                return detail::internal_error(request, "Too many active tasks");
            }
            record = std::make_shared<TaskRecord>();
            record->id = new_task_id();
            record->created = record->updated = std::chrono::system_clock::now();
            record->ttl_ms = ttl;
            record->poll_interval_ms = options_.task_poll_interval_ms;
            tasks_[record->id] = record;
        }

        Json tool_params = request.params;
        tool_params.erase("task");
        core::Request tool_request{request.id, "tools/call", std::move(tool_params)};
        Json created;
        {
            std::lock_guard task_lock(record->mutex);
            created = record->to_json();
        }
        // The worker only touches `*record`, which outlives it (see ~TaskRecord).
        record->worker = std::jthread(
            [this, rec = record.get(), executor = std::move(executor),
             tool_request = std::move(tool_request), token = record->stop.get_token()] {
                run_task(*rec, executor, tool_request, token);
            });
        return detail::ok(request, {{"task", std::move(created)}});
    }

    void run_task(TaskRecord& record, const ToolExecutor& executor, const core::Request& request,
                  std::stop_token token) {
        core::Message outcome;
        try {
            outcome = executor(request, token);
        } catch (const std::exception& error) {
            outcome = detail::internal_error(request, error.what());
        }
        Json final_task;
        {
            std::lock_guard lock(record.mutex);
            if (record.status == "working") {
                if (const auto* error = std::get_if<core::ErrorResponse>(&outcome)) {
                    record.status = "failed";
                    record.status_message = error->error.message;
                } else if (const auto* result = std::get_if<core::ResultResponse>(&outcome);
                           result && result->result.is_object() &&
                           result->result.value("isError", false)) {
                    record.status = "failed";
                    record.status_message = "Tool reported an error";
                } else {
                    record.status = "completed";
                }
                record.outcome = std::move(outcome);
                record.updated = std::chrono::system_clock::now();
                final_task = record.to_json();
            }
        }
        record.cv.notify_all();
        if (!final_task.is_null()) emit_status(final_task);
    }

    core::Message tasks_get(const core::Request& request) {
        const auto task = find_task(request.params.value("taskId", std::string{}));
        if (!task) return detail::invalid_params(request, "Task not found");
        std::lock_guard lock(task->mutex);
        return detail::ok(request, task->to_json());
    }

    core::Message tasks_list(const core::Request& request) {
        const auto offset = detail::parse_cursor(request.params);
        if (!offset) return detail::invalid_params(request, "Invalid cursor");
        std::vector<std::shared_ptr<TaskRecord>> records;
        std::vector<std::shared_ptr<TaskRecord>> graveyard;
        {
            std::lock_guard lock(mutex_);
            purge_expired_locked(graveyard);
            for (auto& [id, t] : tasks_) records.push_back(t);
        }
        std::sort(records.begin(), records.end(),
                  [](const auto& a, const auto& b) { return a->created < b->created; });
        std::vector<Json> items;
        for (const auto& t : records) {
            std::lock_guard lock(t->mutex);
            items.push_back(t->to_json());
        }
        return detail::ok(request, detail::page(items, *offset, options_.page_size, "tasks"));
    }

    core::Message tasks_cancel(const core::Request& request) {
        const auto task = find_task(request.params.value("taskId", std::string{}));
        if (!task) return detail::invalid_params(request, "Task not found");
        Json snapshot;
        {
            std::lock_guard lock(task->mutex);
            if (detail::is_terminal(task->status)) {
                return detail::invalid_params(
                    request, "Cannot cancel task: already in terminal status '" + task->status + "'");
            }
            task->status = "cancelled";
            task->status_message = "Cancelled by client request";
            task->updated = std::chrono::system_clock::now();
            snapshot = task->to_json();
        }
        task->stop.request_stop();
        task->cv.notify_all();
        emit_status(snapshot);
        return detail::ok(request, std::move(snapshot));
    }

    core::Message tasks_result(const core::Request& request) {
        const auto task = find_task(request.params.value("taskId", std::string{}));
        if (!task) return detail::invalid_params(request, "Task not found");
        std::unique_lock lock(task->mutex);
        if (!task->cv.wait_for(lock, options_.task_result_wait,
                               [&] { return detail::is_terminal(task->status); })) {
            return detail::internal_error(request, "Timed out waiting for task to finish");
        }
        if (task->status == "cancelled" || !task->outcome) {
            return detail::fail(request, static_cast<std::int32_t>(core::ErrorCode::invalid_request),
                                "Task was cancelled");
        }
        const Json related = {{"io.modelcontextprotocol/related-task", {{"taskId", task->id}}}};
        if (const auto* result = std::get_if<core::ResultResponse>(&*task->outcome)) {
            Json payload = result->result;
            if (payload.is_object()) {
                auto& meta = payload["_meta"];
                if (!meta.is_object()) meta = Json::object();
                meta.update(related);
            }
            return detail::ok(request, std::move(payload));
        }
        const auto& error = std::get<core::ErrorResponse>(*task->outcome).error;
        return core::ErrorResponse{request.id, error};
    }

    FeatureOptions options_;
    mutable std::mutex mutex_;
    std::vector<PromptDefinition> prompts_;
    std::vector<ResourceDefinition> resources_;
    std::vector<ResourceTemplateDefinition> templates_;
    std::map<std::pair<std::string, std::string>, CompletionFn> prompt_completions_;
    std::map<std::pair<std::string, std::string>, CompletionFn> resource_completions_;
    std::set<std::string> subscriptions_;
    std::map<std::string, std::shared_ptr<TaskRecord>> tasks_;
    ToolExecutor executor_;
    Notifier notifier_;
};

// Runs tools/call through the real Server. Used for task-augmented calls.
inline FeatureRegistry::ToolExecutor make_tool_executor(Server& server) {
    return [&server](const core::Request& request, std::stop_token token) -> core::Message {
        ToolContext context(token, [](Json) {}, request.params);
        auto response = server.handle_message(request, &context);
        context.finish();
        return response;
    };
}

}  // namespace cppmcp::examples
