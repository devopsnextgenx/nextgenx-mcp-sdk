#include <condition_variable>
#include <memory>
#include <mutex>

#include "cppmcp/core/log.hpp"
#include "cppmcp/core/connection.hpp"
#include "cppmcp/server.hpp"
#include "cppmcp/transport/stdio.hpp"
#include "phase4_tools.hpp"

namespace {

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

}  // namespace

int main() {
    // Logs go to stderr (or CPPMCP_LOG_FILE); stdout is reserved for protocol frames.
    cppmcp::core::log::configure(cppmcp::core::log::Level::info);
    CPPMCP_LOG_INFO("starting cppmcp-stdio-server");

    cppmcp::Server server({"cppmcp-stdio-server", "stdio server", "0.1.0"});
    server.add_tool({
        .name = "echo",
        .title = "Echo",
        .description = "Echo a value back to the client.",
        .input_schema = {{"type", "object"},
                 {"properties", {{"value", {{"type", "string"}}}}},
                 {"required", {"value"}},
                 {"additionalProperties", false}},
        .output_schema = nlohmann::json::object(),
        .annotations = cppmcp::ToolAnnotation{},
        .meta = nlohmann::json::object(),
        .handler = [](const nlohmann::json& args, const nlohmann::json&) {
            return args.value("value", nlohmann::json(nullptr));
        },
    });
    server.add_tool({
        .name = "add",
        .title = "Add",
        .description = "Add two numbers.",
        .input_schema = {{"type", "object"},
                 {"properties", {{"a", {{"type", "number"}}},
                         {"b", {{"type", "number"}}}}},
                 {"required", {"a", "b"}},
                 {"additionalProperties", false}},
        .output_schema = nlohmann::json::object(),
        .annotations = cppmcp::ToolAnnotation{},
        .meta = nlohmann::json::object(),
        .handler = [](const nlohmann::json& args, const nlohmann::json&) {
            const auto a = args.value("a", 0);
            const auto b = args.value("b", 0);
            return a + b;
        },
    });
    cppmcp::examples::add_phase4_tools(server);

    auto transport = std::make_shared<cppmcp::transport::StdioServerTransport>();
    auto executor = std::make_shared<cppmcp::core::ThreadPoolExecutor>(2);
    auto connection = std::make_shared<cppmcp::core::Connection>(transport);
    connection->set_request_handler([&](const cppmcp::core::Request& request) {
        executor->post([&, request] {
            std::stop_source cancellation;
            cppmcp::ToolContext context(cancellation.get_token(), [&](nlohmann::json event) {
                if (!event.is_object() || !event.contains("method") || !event["method"].is_string()) return;
                connection->send_notification(event["method"].get<std::string>(),
                    event.value("params", nlohmann::json::object()));
            }, request.params);
            context.set_input_request_handler([connection](const nlohmann::json& input) {
                return connection->send_request(input.at("method").get<std::string>(),
                    input.value("params", nlohmann::json::object()), std::chrono::seconds(30)).get();
            });
            auto response = server.handle_message(request, &context);
            context.finish();
            if (std::holds_alternative<cppmcp::core::ResultResponse>(response) ||
                std::holds_alternative<cppmcp::core::ErrorResponse>(response)) {
                transport->send(response);
            }
        });
    });
    connection->set_notification_handler([&](const cppmcp::core::Notification& notification) {
        (void)server.handle_notification(notification);
    });

    // Exit when the client goes away (stdin EOF). The callback runs on the
    // transport's reader thread, so it only signals; main does the shutdown.
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

    {
        std::unique_lock<std::mutex> lock(done_mutex);
        done_cv.wait(lock, [&] { return done; });
    }

    connection->close();
    executor.reset();
    CPPMCP_LOG_INFO("bye");
    return 0;
}