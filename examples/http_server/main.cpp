#include "cppmcp/server.hpp"
#include "cppmcp/transport/http.hpp"
#include "cppmcp/transport/http_server.hpp"

#include <algorithm>
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
        .handler = {},
    }, [](const Json& args, const Json&, cppmcp::ToolContext& context) {
        const auto count = args.value("count", 5);
        const auto delay = args.value("delay_ms", 10);
        for (int i = 1; i <= count; ++i) {
            const auto step_delay = std::chrono::milliseconds(10);
            for (auto elapsed = std::chrono::milliseconds(0);
                 elapsed < std::chrono::milliseconds(delay); elapsed += step_delay) {
                if (context.cancelled()) {
                    return Json{{"count", i - 1}, {"cancelled", true}};
                }
                std::this_thread::sleep_for(std::min(
                    step_delay, std::chrono::milliseconds(delay) - elapsed));
            }
            if (context.cancellation_token().stop_possible()) {
                context.emit(Json{{"type", "progress"}, {"count", i}, {"total", count}});
            }
        }
        return Json{{"count", count}};
    });
    server.add_tool({
        .name = "emit_demo",
        .title = "Emission demo",
        .description = "Return a simple result for the SSE demonstration.",
        .input_schema = Json{{"type", "object"}},
        .output_schema = Json::object(),
        .annotations = cppmcp::ToolAnnotation{},
        .meta = Json::object(),
        .handler = {},
    }, [](const Json& args, const Json&, cppmcp::ToolContext& context) {
        const auto count = args.value("count", 3);
        for (int i = 1; i <= count; ++i) {
            if (context.cancellation_token().stop_possible()) {
                context.emit(Json{{"type", "progress"}, {"count", i}, {"total", count}});
            }
        }
        return Json{{"count", count}};
    });
    server.add_tool(cppmcp::ToolDefinition{
        .name = "weather_like",
        .title = "Weather-like report",
        .description = "Return a deterministic example forecast.",
        .input_schema = Json{
            {"type", "object"},
            {"properties", {{"city", {{"type", "string"},
                                      {"description", "City for the report."}}}}}},
        .output_schema = Json::object(),
        .annotations = cppmcp::ToolAnnotation{},
        .meta = Json::object(),
        .handler = [](const Json& args, const Json&) {
            return Json{{"city", args.value("city", std::string("unknown"))},
                        {"conditions", "clear"},
                        {"temperature_c", 21}};
        },
    }.header_param("city"));
}

unsigned short parse_port(const std::string& value) {
    std::size_t consumed = 0;
    const auto parsed = std::stoul(value, &consumed);
    if (consumed != value.size() || parsed > 65535) {
        throw std::invalid_argument("port must be an integer from 0 to 65535");
    }
    return static_cast<unsigned short>(parsed);
}

cppmcp::transport::HttpMode parse_mode(const std::string& value) {
    if (value == "modern") {
        return cppmcp::transport::HttpMode::modern;
    }
    if (value == "legacy") {
        return cppmcp::transport::HttpMode::legacy;
    }
    if (value == "dual") {
        return cppmcp::transport::HttpMode::dual;
    }
    throw std::invalid_argument("mode must be one of: modern, legacy, dual");
}

std::chrono::milliseconds parse_ping_interval_ms(const std::string& value) {
    std::size_t consumed = 0;
    const auto parsed = std::stoul(value, &consumed);
    if (consumed != value.size() || parsed > std::numeric_limits<int>::max()) {
        throw std::invalid_argument("ping interval must be an integer millisecond value");
    }
    return std::chrono::milliseconds(static_cast<int>(parsed));
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
            } else if (argument == "--mode" && i + 1 < argc) {
                options.mode = parse_mode(argv[++i]);
                options.handler.mode = options.mode;
            } else if (argument == "--ping-interval-ms" && i + 1 < argc) {
                options.handler.ping_interval = parse_ping_interval_ms(argv[++i]);
            } else if (argument == "--allow-non-loopback") {
                options.allow_non_loopback = true;
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
