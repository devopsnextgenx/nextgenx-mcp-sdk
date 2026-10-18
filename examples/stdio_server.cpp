#include <condition_variable>
#include <memory>
#include <mutex>

#include "cppmcp/core/log.hpp"
#include "cppmcp/server.hpp"
#include "cppmcp/transport/stdio.hpp"

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
        .input_schema = nlohmann::json::object({{"type", "object"}}),
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
        .input_schema = nlohmann::json::object({{"type", "object"}}),
        .output_schema = nlohmann::json::object(),
        .annotations = cppmcp::ToolAnnotation{},
        .meta = nlohmann::json::object(),
        .handler = [](const nlohmann::json& args, const nlohmann::json&) {
            const auto a = args.value("a", 0);
            const auto b = args.value("b", 0);
            return a + b;
        },
    });

    auto transport = std::make_shared<cppmcp::transport::StdioServerTransport>();
    transport->on_message([&](const cppmcp::core::Message& message) {
        auto response = server.handle_message(message);
        if (std::holds_alternative<cppmcp::core::ResultResponse>(response) ||
            std::holds_alternative<cppmcp::core::ErrorResponse>(response)) {
            transport->send(response);
        }
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

    transport->start();
    CPPMCP_LOG_INFO("ready; serving " << server.tools().size() << " tool(s) over stdio");

    {
        std::unique_lock<std::mutex> lock(done_mutex);
        done_cv.wait(lock, [&] { return done; });
    }

    transport->close();  // main thread: safe to join the reader here
    CPPMCP_LOG_INFO("bye");
    return 0;
}