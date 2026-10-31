// HTTP MCP client. Original behaviour (list tools, --call, --listen, elicitation, progress, logs)
// is unchanged; new options exercise prompts, resources, completions and tasks:
//
//   --demo                       run the full tool + prompt/completion/resource/task tour
//   --rpc METHOD [--rpc-params JSON]   send any MCP request (e.g. prompts/list, resources/read)
//   --task TOOL [--args JSON]    run a tool as a task, poll it, print tasks/result
//   --subscribe URI              with --listen: subscribe to resources/updated for URI
//
// The feature options connect through RawRequestTransport(HttpClientTransport); without them the
// client uses Client::connect_http exactly as before.

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include "cppmcp/client.hpp"

#include "../common/client_tour.hpp"

namespace {

using Json = nlohmann::json;

std::size_t parse_duration(std::string_view value) {
    std::size_t duration = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), duration);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        throw std::invalid_argument("--duration-ms must be a non-negative integer");
    }
    return duration;
}

cppmcp::NegotiationPolicy parse_mode(std::string_view value) {
    if (value == "modern") return cppmcp::NegotiationPolicy::modern_only;
    if (value == "legacy") return cppmcp::NegotiationPolicy::legacy_only;
    if (value == "auto") return cppmcp::NegotiationPolicy::prefer_modern;
    throw std::invalid_argument("--mode must be modern, legacy, or auto");
}

std::string result_text(const Json& result) {
    if (!result.contains("content") || !result["content"].is_array() || result["content"].empty() ||
        !result["content"][0].contains("text")) {
        throw std::runtime_error("tools/call add returned no text content");
    }
    return result["content"][0]["text"].get<std::string>();
}

Json parse_object(const char* flag, const char* text) {
    auto value = Json::parse(text);
    if (!value.is_object()) throw std::invalid_argument(std::string(flag) + " must be a JSON object");
    return value;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        cppmcp::HttpClientOptions options;
        std::string token, log_level, elicit_action, rpc_method, task_tool, subscribe_uri;
        std::optional<std::string> requested_call;
        Json call_args = Json::object(), elicit_content = Json::object(), rpc_params = Json::object();
        bool progress = false, listen = false, demo = false;
        std::size_t duration_ms = 5000;
        for (int i = 1; i < argc; ++i) {
            const std::string_view argument(argv[i]);
            const bool takes_value =
                argument == "--url" || argument == "--mode" || argument == "--cacert" || argument == "--token" ||
                argument == "--duration-ms" || argument == "--call" || argument == "--args" ||
                argument == "--log-level" || argument == "--elicit" || argument == "--elicit-content" ||
                argument == "--rpc" || argument == "--rpc-params" || argument == "--task" ||
                argument == "--subscribe";
            if (takes_value && i + 1 >= argc) throw std::invalid_argument("missing value for " + std::string(argument));
            if (argument == "--url") options.url = argv[++i];
            else if (argument == "--mode") options.policy = parse_mode(argv[++i]);
            else if (argument == "--cacert") options.ca_file = argv[++i];
            else if (argument == "--token") token = argv[++i];
            else if (argument == "--listen") listen = true;
            else if (argument == "--duration-ms") duration_ms = parse_duration(argv[++i]);
            else if (argument == "--call") requested_call = argv[++i];
            else if (argument == "--args") call_args = parse_object("--args", argv[++i]);
            else if (argument == "--progress") progress = true;
            else if (argument == "--log-level") log_level = argv[++i];
            else if (argument == "--elicit") {
                elicit_action = argv[++i];
                if (elicit_action != "accept" && elicit_action != "decline" && elicit_action != "cancel") {
                    throw std::invalid_argument("--elicit must be accept, decline, or cancel");
                }
            } else if (argument == "--elicit-content") elicit_content = parse_object("--elicit-content", argv[++i]);
            else if (argument == "--demo") demo = true;
            else if (argument == "--rpc") rpc_method = argv[++i];
            else if (argument == "--rpc-params") rpc_params = parse_object("--rpc-params", argv[++i]);
            else if (argument == "--task") task_tool = argv[++i];
            else if (argument == "--subscribe") subscribe_uri = argv[++i];
            else throw std::invalid_argument("unknown option: " + std::string(argument));
        }
        if (options.url.empty()) throw std::invalid_argument("--url is required");
        if (!token.empty()) options.auth_provider = std::make_shared<cppmcp::StaticBearerProvider>(token);
        if (!elicit_action.empty() || demo) options.client_capabilities["elicitation"] = {{"form", Json::object()}};

        const bool use_ext = demo || !rpc_method.empty() || !task_tool.empty() || !subscribe_uri.empty();

        cppmcp::Client client;
        std::shared_ptr<cppmcp::transport::HttpClientTransport> http;
        std::shared_ptr<cppmcp::examples::RawRequestTransport> tap;
        if (use_ext) {
            http = std::make_shared<cppmcp::transport::HttpClientTransport>(options);
            tap = std::make_shared<cppmcp::examples::RawRequestTransport>(http);
            client.connect(tap);
            std::cout << "MODE " << (http->is_modern() ? "modern" : "legacy") << '\n';
        } else {
            client.connect_http(std::move(options));
            std::cout << "MODE " << client.http_mode() << '\n';
        }

        const auto tools_result = client.list_tools();
        if (!tools_result.contains("tools") || !tools_result["tools"].is_array()) {
            throw std::runtime_error("tools/list returned no tools array");
        }
        bool has_add = false;
        for (const auto& tool : tools_result["tools"]) {
            const auto name = tool.at("name").get<std::string>();
            std::cout << "TOOL " << name << '\n';
            has_add = has_add || name == "add";
        }

        if (requested_call) {
            cppmcp::Client::CallToolOptions call_options;
            call_options.log_level = log_level;
            if (progress) {
                call_options.on_progress = [](double current, std::optional<double> total, std::optional<std::string>) {
                    std::cout << "PROGRESS " << current << '/';
                    if (total) std::cout << *total;
                    std::cout << '\n';
                };
            }
            if (!log_level.empty()) {
                call_options.on_log = [](const std::string& level, const std::string&, const Json& data) {
                    std::cout << "LOG " << level << ' ' << (data.is_string() ? data.get<std::string>() : data.dump()) << '\n';
                };
            }
            if (!elicit_action.empty()) {
                call_options.on_elicit = [&](const std::string&, const Json& request) {
                    std::cout << "ELICIT " << request.value("params", Json::object()).value("message", std::string{}) << '\n';
                    Json response = {{"action", elicit_action}};
                    if (elicit_action == "accept") response["content"] = elicit_content;
                    return response;
                };
            }
            const auto result = client.call_tool(*requested_call, call_args, std::move(call_options));
            std::cout << "RESULT " << result.dump() << '\n';
            if (result.value("resultType", std::string{}) == "input_required" && result.contains("inputRequests") &&
                result["inputRequests"].is_object()) {
                for (auto item = result["inputRequests"].begin(); item != result["inputRequests"].end(); ++item) {
                    std::cout << "ELICIT " << item.value().value("params", Json::object()).value("message", std::string{}) << '\n';
                }
            }
        } else if (has_add && !use_ext) {
            std::cout << "CALL add -> " << result_text(client.call_tool("add", Json{{"a", 1}, {"b", 2}})) << '\n';
        }

        if (use_ext) {
            cppmcp::examples::FeatureClient features(client);
            if (demo) {
                cppmcp::examples::run_tool_tour(client);
                cppmcp::examples::run_feature_tour(features);
            }
            if (!rpc_method.empty()) {
                cppmcp::examples::step("RPC " + rpc_method, [&] { return features.request(rpc_method, rpc_params); });
            }
            if (!task_tool.empty()) {
                try {
                    const auto task = features.create_task(task_tool, call_args);
                    const auto id = task.at("taskId").get<std::string>();
                    std::cout << "TASK " << task.dump() << '\n';
                    features.wait_for_task(id, std::chrono::seconds(30), [](const Json& t) {
                        std::cout << "TASK_STATUS " << t.value("status", std::string{}) << '\n';
                    });
                    std::cout << "TASK_RESULT " << features.task_result(id).dump() << '\n';
                } catch (const std::exception& e) {
                    std::cout << "TASK !! " << e.what() << '\n';
                }
            }
            if (!subscribe_uri.empty()) {
                cppmcp::examples::step("resources/subscribe " + subscribe_uri,
                                       [&] { return features.subscribe(subscribe_uri); });
            }
        }

        if (listen) {
            cppmcp::transport::ListenCallbacks callbacks;
            callbacks.on_stream_lost = [] { std::cout << "STREAM_LOST\n"; };
            callbacks.on_reconnected = [] { std::cout << "RECONNECTED\n"; };
            // Filter keys beyond toolsListChanged follow SubscriptionFilter's field names (camelCased).
            Json filter = {{"toolsListChanged", true}};
            if (use_ext) {
                filter["promptsListChanged"] = true;
                filter["resourcesListChanged"] = true;
                if (!subscribe_uri.empty()) filter["resourceSubscriptions"] = Json::array({subscribe_uri});
            }
            auto on_note = [](const cppmcp::core::Notification& n) {
                std::cout << "NOTIFY " << n.method;
                if (n.method == "notifications/resources/updated") std::cout << ' ' << n.params.value("uri", std::string{});
                std::cout << '\n';
            };
            // The decorator hides the transport from Client::listen, so use the transport's own.
            auto handle = use_ext ? http->listen(filter, on_note, std::move(callbacks))
                                  : client.listen(filter, on_note, std::move(callbacks));
            std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
            handle->close();
        }
        client.close();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "http_client: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
