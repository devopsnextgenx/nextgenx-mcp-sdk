#include "cppmcp/server.hpp"
#include "cppmcp/transport/http.hpp"
#include "cppmcp/transport/http_server.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using Json = nlohmann::json;

void add_tools(cppmcp::Server& server) {
    server.add_tool({
        .name = "echo",
        .title = "Echo",
        .description = "Return the supplied value.",
        .input_schema = Json{{"type", "object"}},
        .output_schema = Json::object(),
        .annotations = cppmcp::ToolAnnotation{},
        .meta = Json::object(),
        .handler = [](const Json& args, const Json&) {
            return args.value("value", Json(nullptr));
        },
    });
    server.add_tool({
        .name = "add",
        .title = "Add",
        .description = "Add two numbers.",
        .input_schema = Json{{"type", "object"}},
        .output_schema = Json::object(),
        .annotations = cppmcp::ToolAnnotation{},
        .meta = Json::object(),
        .handler = [](const Json& args, const Json&) {
            const auto a = args.value("a", 0.0);
            const auto b = args.value("b", 0.0);
            return Json(a + b);
        },
    });
    server.add_tool({
        .name = "slow_count",
        .title = "Slow count",
        .description = "Wait between counting steps.",
        .input_schema = Json{{"type", "object"}},
        .output_schema = Json::object(),
        .annotations = cppmcp::ToolAnnotation{},
        .meta = Json::object(),
        .handler = [](const Json& args, const Json&) {
            const auto count = args.value("count", 5);
            const auto delay = args.value("delay_ms", 10);
            for (int i = 0; i < count; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }
            return Json{{"count", count}};
        },
    });
    server.add_tool({
        .name = "emit_demo",
        .title = "Emission demo",
        .description = "Return a simple result for the SSE demonstration.",
        .input_schema = Json{{"type", "object"}},
        .output_schema = Json::object(),
        .annotations = cppmcp::ToolAnnotation{},
        .meta = Json::object(),
        .handler = [](const Json& args, const Json&) {
            return Json{{"count", args.value("count", 3)}};
        },
    });
    server.add_tool({
        .name = "weather_like",
        .title = "Weather-like report",
        .description = "Return a deterministic example forecast.",
        .input_schema = Json{{"type", "object"}},
        .output_schema = Json::object(),
        .annotations = cppmcp::ToolAnnotation{},
        .meta = Json::object(),
        .handler = [](const Json& args, const Json&) {
            return Json{{"city", args.value("city", std::string("unknown"))},
                        {"conditions", "clear"},
                        {"temperature_c", 21}};
        },
    });
}

unsigned short parse_port(const std::string& value) {
    std::size_t consumed = 0;
    const auto parsed = std::stoul(value, &consumed);
    if (consumed != value.size() || parsed > 65535) {
        throw std::invalid_argument("port must be an integer from 0 to 65535");
    }
    return static_cast<unsigned short>(parsed);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        cppmcp::transport::HttpOptions options;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--port" && i + 1 < argc) {
                options.port = parse_port(argv[++i]);
            } else if (argument == "--bind" && i + 1 < argc) {
                options.bind_address = argv[++i];
            } else if (argument == "--json-only") {
                options.handler.response_mode = cppmcp::transport::HttpResponseMode::json_only;
            } else if (argument == "--sse-only") {
                options.handler.response_mode = cppmcp::transport::HttpResponseMode::sse_only;
            } else if (argument == "--token" && i + 1 < argc) {
                options.bearer_token = argv[++i];
            } else {
                throw std::invalid_argument("unknown or incomplete option: " + argument);
            }
        }

        cppmcp::Server server({"cppmcp-http-server", "cppmcp HTTP server", "0.1.0"});
        add_tools(server);
        server.run_http(options);
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "http_server: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
