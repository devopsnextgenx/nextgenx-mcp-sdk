// stdio MCP server: tools (incl. phase-4 tools) + prompts, resources, completions, tasks.
//
// Request path:  Connection -> FeatureRegistry::try_handle -> (fallthrough) Server::handle_message
// Prompts/resources/completions/tasks are served by the registry in examples/common because the
// public cppmcp::Server only registers tools.

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>

#include "cppmcp/core/connection.hpp"
#include "cppmcp/core/log.hpp"
#include "cppmcp/server.hpp"
#include "cppmcp/transport/stdio.hpp"

#include "../common/demo_features.hpp"
#include "../common/phase4_tools.hpp"

namespace {

using Json = nlohmann::json;

const char* reason_name(cppmcp::core::CloseReason reason) {
    using cppmcp::core::CloseReason;
    switch (reason) {
        case CloseReason::normal: return "normal";
        case CloseReason::eof: return "eof";
        case CloseReason::parse_error: return "parse_error";
        case CloseReason::timeout: return "timeout";
        case CloseReason::error: return "error";
        case CloseReason::child_exit: return "child_exit";
        case CloseReason::shutdown: return "shutdown";
    }
    return "unknown";
}

void add_basic_tools(cppmcp::Server& server) {
    server.add_tool({
        .name = "echo",
        .title = "Echo",
        .description = "Echo a value back to the client..",
        .input_schema = {{"type", "object"},
                         {"properties", {{"value", {{"type", "string"}}}}},
                         {"required", {"value"}},
                         {"additionalProperties", false}},
        .output_schema = Json::object(),
        .annotations = cppmcp::ToolAnnotation{.read_only_hint = true},
        .meta = Json::object(),
        .handler = [](const Json& args, const Json&) { return args.value("value", Json(nullptr)); },
    });
    server.add_tool({
        .name = "add",
        .title = "Add",
        .description = "Add two numbers.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"a", {{"type", "number"}}}, {"b", {{"type", "number"}}}}},
                         {"required", {"a", "b"}},
                         {"additionalProperties", false}},
        .output_schema = Json::object(),
        .annotations = cppmcp::ToolAnnotation{.read_only_hint = true},
        .meta = Json::object(),
        .handler = [](const Json& args, const Json&) { return args.value("a", 0.0) + args.value("b", 0.0); },
    });
}

}  // namespace

int main(int argc, char** argv) {
    cppmcp::examples::FeatureOptions feature_options;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--resource-root" && i + 1 < argc) {
                feature_options.resource_root = argv[++i];
            } else if (argument == "--max-resource-bytes" && i + 1 < argc) {
                const std::string value = argv[++i];
                std::size_t consumed = 0;
                if (value.empty() || value.front() == '-') {
                    throw std::invalid_argument(
                        "--max-resource-bytes must be a non-negative integer");
                }
                const auto parsed = std::stoull(value, &consumed);
                if (consumed != value.size() ||
                    parsed > std::numeric_limits<std::size_t>::max()) {
                    throw std::invalid_argument("--max-resource-bytes is out of range");
                }
                feature_options.max_resource_bytes = static_cast<std::size_t>(parsed);
            } else {
                throw std::invalid_argument("unknown or incomplete option: " + argument);
            }
        }
    } catch (const std::exception& error) {
        CPPMCP_LOG_ERROR("stdio_server: " << error.what());
        return EXIT_FAILURE;
    }
    // Logs go to stderr (or CPPMCP_LOG_FILE); stdout is reserved for protocol frames.
    cppmcp::core::log::configure(cppmcp::core::log::Level::info);
    CPPMCP_LOG_INFO("starting cppmcp-stdio-server");

    cppmcp::Server server({"cppmcp-stdio-server", "stdio server", "0.1.0"});
    add_basic_tools(server);
    cppmcp::examples::add_phase4_tools(server);

    cppmcp::examples::FeatureRegistry registry(feature_options);
    cppmcp::examples::add_demo_features(registry);
    registry.add_file_resource_template();
    registry.set_tool_executor(cppmcp::examples::make_tool_executor(server));
    server.add_request_handler("prompts/get", [&registry](const cppmcp::core::Request& request,
                                                           cppmcp::ToolContext& context) {
        return *registry.try_handle(request, &context);
    });
    server.add_request_handler("resources/read", [&registry](const cppmcp::core::Request& request,
                                                              cppmcp::ToolContext& context) {
        return *registry.try_handle(request, &context);
    });

    auto transport = std::make_shared<cppmcp::transport::StdioServerTransport>();
    // tasks/result may block while a task runs, so keep spare workers.
    auto executor = std::make_shared<cppmcp::core::ThreadPoolExecutor>(8);
    auto connection = std::make_shared<cppmcp::core::Connection>(transport);

    // Server -> client notifications from the registry (resources/updated, tasks/status).
    registry.set_notifier([connection](const std::string& method, const Json& params) {
        connection->send_notification(method, params);
    });

    connection->set_request_handler([&](const cppmcp::core::Request& request) {
        executor->post([&, request] {
            if (request.method != "prompts/get" && request.method != "resources/read") {
                if (auto handled = registry.try_handle(request)) {
                    transport->send(*handled);
                    return;
                }
            }
            std::stop_source cancellation;
            cppmcp::ToolContext context(cancellation.get_token(), [&](Json event) {
                if (!event.is_object() || !event.contains("method") || !event["method"].is_string()) return;
                connection->send_notification(event["method"].get<std::string>(),
                                              event.value("params", Json::object()));
            }, request.params);
            context.set_input_request_handler([connection](const Json& input) {
                return connection->send_request(input.at("method").get<std::string>(),
                                                input.value("params", Json::object()),
                                                std::chrono::seconds(30)).get();
            });
            auto response = server.handle_message(request, &context);
            context.finish();
            if (auto* result = std::get_if<cppmcp::core::ResultResponse>(&response)) {
                registry.augment_discover(result->result);
            }
            if (std::holds_alternative<cppmcp::core::ResultResponse>(response) ||
                std::holds_alternative<cppmcp::core::ErrorResponse>(response)) {
                transport->send(response);
            }
        });
    });
    connection->set_notification_handler([&](const cppmcp::core::Notification& notification) {
        (void)server.handle_notification(notification);
    });

    // Exit when the client goes away (stdin EOF). Callback runs on the reader thread: only signal.
    std::mutex done_mutex;
    std::condition_variable done_cv;
    bool done = false;
    transport->on_close([&](cppmcp::core::CloseReason reason) {
        CPPMCP_LOG_INFO("transport closed (reason=" << reason_name(reason) << "); shutting down");
        {
            std::lock_guard<std::mutex> lock(done_mutex);
            done = true;
        }
        done_cv.notify_all();
    });

    connection->start();
    CPPMCP_LOG_INFO("ready; serving " << server.tools().size() << " tool(s) over stdio");

    // Touch demo://clock once a second; only subscribed clients receive notifications.
    std::jthread ticker([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!stop.stop_requested()) registry.resource_updated("demo://clock");
        }
    });

    {
        std::unique_lock<std::mutex> lock(done_mutex);
        done_cv.wait(lock, [&] { return done; });
    }

    ticker.request_stop();
    ticker.join();
    registry.shutdown();  // cancels and joins running tasks before the connection goes away
    connection->close();
    executor.reset();
    CPPMCP_LOG_INFO("bye");
    return 0;
}
