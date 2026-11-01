// HTTP MCP server: tools (incl. phase-4 tools) + prompts, resources, completions, tasks.
//
// Default: examples' FrontDoor listener -> FeatureRegistry -> (fallthrough) McpHttpHandler.
// --sdk-listener: the SDK's own Server::run_http (TLS etc.), tools only.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

#include "cppmcp/server.hpp"
#include "cppmcp/transport/http.hpp"
#include "cppmcp/transport/http_server.hpp"

#include "../common/demo_features.hpp"
#include "../common/phase4_tools.hpp"
#include "augmenting_exchange.hpp"
#include "front_door.hpp"

namespace {

using Json = nlohmann::json;

void add_tools(cppmcp::Server& server) {
    server.add_tool({
        .name = "echo",
        .title = "Echo",
        .description = "Return the supplied value.",
        .input_schema = Json{{"type", "object"},
                             {"properties", {{"value", {{"description", "The value to return unchanged."}}}}},
                             {"required", Json::array({"value"})},
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
        .input_schema = Json{{"type", "object"},
                             {"properties", {{"a", {{"type", "number"}, {"description", "First addend."}}},
                                             {"b", {{"type", "number"}, {"description", "Second addend."}}}}},
                             {"required", Json::array({"a", "b"})},
                             {"additionalProperties", false}},
        .output_schema = Json::object(),
        .annotations = cppmcp::ToolAnnotation{.read_only_hint = true},
        .meta = Json::object(),
        .handler = [](const Json& args, const Json&) { return Json(args.value("a", 0.0) + args.value("b", 0.0)); },
    });
    server.add_tool({
        .name = "slow_count",
        .title = "Slow count",
        .description = "Wait between counting steps.",
        .input_schema = Json{{"type", "object"},
                             {"properties", {{"count", {{"type", "integer"}, {"minimum", 0},
                                                        {"description", "Number of steps (default: 5)."}}},
                                             {"delay_ms", {{"type", "integer"}, {"minimum", 0},
                                                           {"description", "Delay between steps in ms (default: 10)."}}}}},
                             {"additionalProperties", false}},
        .output_schema = Json::object(),
        .annotations = cppmcp::ToolAnnotation{},
        .meta = Json::object(),
        .handler = {},
    }, [](const Json& args, const Json&, cppmcp::ToolContext& context) {
        const auto count = args.value("count", 5);
        const auto delay = args.value("delay_ms", 10);
        for (int i = 1; i <= count; ++i) {
            const auto step_delay = std::chrono::milliseconds(10);
            for (auto elapsed = std::chrono::milliseconds(0); elapsed < std::chrono::milliseconds(delay);
                 elapsed += step_delay) {
                if (context.cancelled()) return Json{{"count", i - 1}, {"cancelled", true}};
                std::this_thread::sleep_for(std::min(step_delay, std::chrono::milliseconds(delay) - elapsed));
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
        .input_schema = Json{{"type", "object"},
                             {"properties", {{"count", {{"type", "integer"}, {"minimum", 0},
                                                        {"description", "Number of progress events (default: 3)."}}}}},
                             {"additionalProperties", false}},
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
        .input_schema = Json{{"type", "object"},
                             {"properties", {{"city", {{"type", "string"}, {"description", "City for the report."}}}}},
                             {"required", Json::array({"city"})},
                             {"additionalProperties", false}},
        .output_schema = Json::object(),
        .annotations = cppmcp::ToolAnnotation{},
        .meta = Json::object(),
        .handler = [](const Json& args, const Json&) {
            return Json{{"city", args.value("city", std::string("unknown"))},
                        {"conditions", "clear"}, {"temperature_c", 21}};
        },
    }.header_param("city"));
}

unsigned short parse_port(const std::string& value) {
    std::size_t consumed = 0;
    const auto parsed = std::stoul(value, &consumed);
    if (consumed != value.size() || parsed > 65535) throw std::invalid_argument("port must be an integer from 0 to 65535");
    return static_cast<unsigned short>(parsed);
}

cppmcp::transport::HttpMode parse_mode(const std::string& value) {
    if (value == "modern") return cppmcp::transport::HttpMode::modern;
    if (value == "legacy") return cppmcp::transport::HttpMode::legacy;
    if (value == "dual") return cppmcp::transport::HttpMode::dual;
    throw std::invalid_argument("mode must be one of: modern, legacy, dual");
}

std::chrono::milliseconds parse_ms(const std::string& value) {
    std::size_t consumed = 0;
    const auto parsed = std::stoul(value, &consumed);
    if (consumed != value.size() || parsed > static_cast<unsigned long>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("interval must be an integer millisecond value");
    }
    return std::chrono::milliseconds(static_cast<int>(parsed));
}

std::size_t parse_size(const std::string& value, const char* option) {
    if (value.empty() || value.front() == '-') {
        throw std::invalid_argument(std::string(option) + " must be a non-negative integer");
    }
    std::size_t consumed = 0;
    const auto parsed = std::stoull(value, &consumed);
    if (consumed != value.size() || parsed > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument(std::string(option) + " is out of range");
    }
    return static_cast<std::size_t>(parsed);
}

std::string parse_request_state_key(const std::string& value) {
    if (value.size() != 64 || !std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; })) {
        throw std::invalid_argument("request-state key must contain exactly 64 hexadecimal characters");
    }
    return value;
}

std::string read_request_state_key_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("unable to open request-state key file");
    std::string value((std::istreambuf_iterator<char>(input)),
                      std::istreambuf_iterator<char>());
    if (input.bad()) throw std::runtime_error("unable to read request-state key file");
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return parse_request_state_key(value);
}

bool parse_validate_output(const std::string& value) {
    if (value == "on") return true;
    if (value == "off") return false;
    throw std::invalid_argument("--validate-output must be on or off");
}

std::optional<cppmcp::core::RpcError> validate_feature_headers(
    const cppmcp::core::Request& request, const cppmcp::transport::HttpRequest& http_request) {
    const auto meta = request.params.is_object() ? request.params.find("_meta")
                                                  : cppmcp::core::Json::const_iterator{};
    if (meta == request.params.end() || !meta->is_object() ||
        meta->value("io.modelcontextprotocol/protocolVersion", std::string{}) != "2026-07-28") {
        return std::nullopt;
    }

    auto mismatch = [](std::string header) {
        return cppmcp::core::RpcError{
            static_cast<std::int32_t>(cppmcp::core::ErrorCode::header_mismatch),
            "Mismatched " + header + " header", std::nullopt};
    };
    if (cppmcp::examples::header_value(http_request.headers, "Mcp-Method") != request.method) {
        return mismatch("Mcp-Method");
    }

    const bool needs_name = request.method == "prompts/get" || request.method == "resources/read";
    if (!needs_name) return std::nullopt;
    const char* field = request.method == "prompts/get" ? "name" : "uri";
    const auto target = request.params.find(field);
    const auto header = cppmcp::examples::header_value(http_request.headers, "Mcp-Name");
    std::string decoded;
    if (target == request.params.end() || !target->is_string() ||
        !cppmcp::transport::decode_mirrored_header_value(header, decoded) ||
        decoded != target->get<std::string>()) {
        return mismatch("Mcp-Name");
    }
    return std::nullopt;
}

std::atomic<bool> g_stop{false};
extern "C" void on_signal(int) { g_stop = true; }

}  // namespace

int main(int argc, char** argv) {
    try {
        cppmcp::transport::HttpOptions options;
        cppmcp::examples::FeatureOptions feature_options;
        std::string tls_cert, tls_key;
        std::string request_state_key_file;
        std::chrono::milliseconds tick_interval{0};
        std::chrono::milliseconds churn_interval{0};
        bool sdk_listener = false;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--port" && i + 1 < argc) options.port = parse_port(argv[++i]);
            else if (argument == "--resource-root" && i + 1 < argc) feature_options.resource_root = argv[++i];
            else if (argument == "--max-resource-bytes" && i + 1 < argc)
                feature_options.max_resource_bytes = parse_size(argv[++i], "--max-resource-bytes");
            else if (argument == "--page-size" && i + 1 < argc)
                feature_options.page_size = parse_size(argv[++i], "--page-size");
            else if (argument == "--tick-ms" && i + 1 < argc) tick_interval = parse_ms(argv[++i]);
            else if (argument == "--churn-ms" && i + 1 < argc) churn_interval = parse_ms(argv[++i]);
            else if (argument == "--bind" && i + 1 < argc) options.bind_address = argv[++i];
            else if (argument == "--mode" && i + 1 < argc) { options.mode = parse_mode(argv[++i]); options.handler.mode = options.mode; }
            else if (argument == "--ping-interval-ms" && i + 1 < argc) options.handler.ping_interval = parse_ms(argv[++i]);
            else if (argument == "--notify-interval-ms" && i + 1 < argc) options.notify_interval = parse_ms(argv[++i]);
            else if (argument == "--allow-non-loopback") { options.allow_non_loopback = true; options.allow_insecure_remote = true; }
            else if (argument == "--allow-insecure-remote") options.allow_insecure_remote = true;
            else if (argument == "--json-only") options.handler.response_mode = cppmcp::transport::HttpResponseMode::json_only;
            else if (argument == "--sse-only") options.handler.response_mode = cppmcp::transport::HttpResponseMode::sse_only;
            else if (argument == "--token" && i + 1 < argc) options.bearer_token = argv[++i];
            else if (argument == "--tls-cert" && i + 1 < argc) tls_cert = argv[++i];
            else if (argument == "--tls-key" && i + 1 < argc) tls_key = argv[++i];
            else if (argument == "--request-state-key" && i + 1 < argc) options.request_state_key = parse_request_state_key(argv[++i]);
            else if (argument == "--request-state-key-file" && i + 1 < argc) request_state_key_file = argv[++i];
            else if (argument == "--validate-output" && i + 1 < argc) options.validate_output = parse_validate_output(argv[++i]);
            else if (argument == "--sdk-listener") sdk_listener = true;
            else throw std::invalid_argument("unknown or incomplete option: " + argument);
        }
        if (options.request_state_key.empty()) {
            if (!request_state_key_file.empty()) {
                options.request_state_key = read_request_state_key_file(request_state_key_file);
            } else if (const char* key = std::getenv("CPPMCP_REQUEST_STATE_KEY")) {
                options.request_state_key = parse_request_state_key(key);
            }
        }
        if (tls_cert.empty() != tls_key.empty()) throw std::invalid_argument("--tls-cert and --tls-key must be supplied together");
        if (!tls_cert.empty()) {
            if (!sdk_listener) throw std::invalid_argument("TLS requires --sdk-listener (the example front door is plain HTTP)");
            options.tls = cppmcp::transport::HttpTlsOptions{.cert_chain_file = std::move(tls_cert), .private_key_file = std::move(tls_key)};
        }

        const cppmcp::ServerInfo info{"cppmcp-http-server", "cppmcp HTTP server", "0.1.0"};
        cppmcp::ServerOptions server_options;
        if (!options.request_state_key.empty() && !sdk_listener) {
            server_options.request_state_keys.push_back({"example", options.request_state_key});
        }
        cppmcp::Server server(info, server_options);
        add_tools(server);
        cppmcp::examples::add_phase4_tools(server);
        server.set_validate_output(options.validate_output);

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
        // HTTP has no connection to push on: resource updates go through the SDK's subscription
        // registry (delivered to `listen` streams); task status is polled via tasks/get.
        registry.set_notifier([&server](const std::string& method, const Json& params) {
            if (method == "notifications/resources/updated") server.notify_resource_updated(params.value("uri", std::string{}));
        });
        registry.set_list_changed_notifier([&server](const std::string& kind) {
            if (kind == "prompts") server.notify_prompts_list_changed();
            else if (kind == "resources") server.notify_resources_list_changed();
        });

        std::jthread notifier;
        if (options.notify_interval.count() > 0) {
            notifier = std::jthread([&](std::stop_token stop) {
                while (!stop.stop_requested()) {
                    const auto until = std::chrono::steady_clock::now() + options.notify_interval;
                    while (!stop.stop_requested() && std::chrono::steady_clock::now() < until) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                    if (stop.stop_requested()) break;
                    server.notify_tools_list_changed();
                    registry.resource_updated("demo://clock");
                }
            });
        }
        std::jthread ticker;
        if (tick_interval.count() > 0) {
            ticker = std::jthread([&, next_counter = true](std::stop_token stop) mutable {
                while (!stop.stop_requested()) {
                    const auto until = std::chrono::steady_clock::now() + tick_interval;
                    while (!stop.stop_requested() && std::chrono::steady_clock::now() < until) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    if (stop.stop_requested()) break;
                    const std::string uri = next_counter ? "demo://counter" : "demo://other";
                    if (next_counter) {
                        cppmcp::examples::demo::counter_value.fetch_add(
                            1, std::memory_order_relaxed);
                    } else {
                        cppmcp::examples::demo::other_value.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    next_counter = !next_counter;
                    server.notify_resource_updated(uri);
                }
            });
        }
        std::jthread churn;
        if (churn_interval.count() > 0) {
            churn = std::jthread([&](std::stop_token stop) {
                bool registered = false;
                while (!stop.stop_requested()) {
                    const auto until = std::chrono::steady_clock::now() + churn_interval;
                    while (!stop.stop_requested() && std::chrono::steady_clock::now() < until) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    if (stop.stop_requested()) break;
                    if (registered) {
                        registry.remove_prompt("dynamic_prompt");
                        registry.remove_resource("demo://dynamic");
                    } else {
                        registry.add_prompt({
                            .name = "dynamic_prompt",
                            .title = "Dynamic prompt",
                            .description = "A prompt registered at runtime.",
                            .arguments = {},
                            .render = [](const Json&) {
                                return Json{{"messages",
                                             Json::array({{{"role", "user"},
                                                           {"content", {{"type", "text"},
                                                                        {"text", "dynamic"}}}}})}};
                            },
                        });
                        registry.add_resource({
                            .uri = "demo://dynamic",
                            .name = "dynamic",
                            .title = "Dynamic resource",
                            .description = "A resource registered at runtime.",
                            .mime_type = "text/plain",
                            .read = [] {
                                return cppmcp::examples::ResourceContent{
                                    "dynamic", std::nullopt, std::nullopt};
                            },
                        });
                    }
                    registered = !registered;
                }
            });
        }

        if (sdk_listener) {
            std::cerr << "http_server: using SDK listener (tools only; no prompts/resources/tasks)\n";
            server.run_http(options);
            notifier.request_stop();
            ticker.request_stop();
            churn.request_stop();
            return EXIT_SUCCESS;
        }

        if (!options.bearer_token.empty()) {
            const std::string expected = "Bearer " + options.bearer_token;
            options.handler.authenticator = [expected](const cppmcp::transport::HttpRequest& r) {
                return cppmcp::examples::header_value(r.headers, "Authorization") == expected;
            };
        }
        const bool loopback = options.bind_address == "127.0.0.1" || options.bind_address == "localhost";
        if (!loopback && !options.allow_non_loopback) {
            throw std::invalid_argument("refusing to bind " + options.bind_address + " without --allow-non-loopback");
        }

        cppmcp::transport::McpHttpHandler sdk_handler(server, options.handler);
        const std::string path = options.handler.path;

        cppmcp::examples::FrontDoor door(
            options.bind_address, options.port,
            [&](const std::shared_ptr<cppmcp::transport::IHttpExchange>& exchange) {
                const auto& request = exchange->request();
                const auto target = request.target.substr(0, request.target.find('?'));
                if (target == path && request.method == "POST") {
                    if (!options.handler.authenticator || options.handler.authenticator(request)) {
                        const auto parsed = cppmcp::core::parse_message(request.body);
                        if (const auto* message = std::get_if<cppmcp::core::Message>(&parsed)) {
                            if (const auto* rpc = std::get_if<cppmcp::core::Request>(message)) {
                                if (const auto error = validate_feature_headers(*rpc, request)) {
                                    exchange->respond(
                                        400, {{"Content-Type", "application/json"}},
                                        cppmcp::core::serialize_message(
                                            cppmcp::core::ErrorResponse{rpc->id, *error}));
                                    return;
                                }
                                if (rpc->method != "prompts/get" &&
                                    rpc->method != "resources/read" &&
                                    rpc->method != "resources/subscribe" &&
                                    rpc->method != "resources/unsubscribe") {
                                    if (auto handled = registry.try_handle(*rpc)) {
                                    exchange->respond(200, {{"Content-Type", "application/json"}},
                                                      cppmcp::core::serialize_message(*handled));
                                    return;
                                    }
                                }
                            }
                        }
                    }
                }
                // Everything else (initialize/discover, tools/*, GET/DELETE, auth failures...) is the SDK's.
                sdk_handler.handle(std::make_shared<cppmcp::examples::AugmentingExchange>(exchange, registry));
            });

        std::cout << "LISTENING " << door.port() << std::endl;
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));

        notifier.request_stop();
        ticker.request_stop();
        churn.request_stop();
        door.stop();
        if (notifier.joinable()) notifier.join();
        if (ticker.joinable()) ticker.join();
        if (churn.joinable()) churn.join();
        registry.shutdown();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "http_server: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
