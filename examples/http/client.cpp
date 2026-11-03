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
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "cppmcp/client.hpp"
#include "cppmcp/client/oauth_provider.hpp"

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

std::size_t parse_count(std::string_view flag, std::string_view value) {
    std::size_t count = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), count);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || count == 0) {
        throw std::invalid_argument(std::string(flag) + " must be a positive integer");
    }
    return count;
}

std::string read_secret_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("unable to read OAuth client secret file");
    std::string secret((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
    while (!secret.empty() && (secret.back() == '\n' || secret.back() == '\r')) {
        secret.pop_back();
    }
    if (secret.empty()) throw std::runtime_error("OAuth client secret file is empty");
    return secret;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        cppmcp::HttpClientOptions options;
        std::string token, log_level, elicit_action, rpc_method, request_method, task_tool;
        std::string oauth_secret_file, oauth_store_file, oauth_launcher = "xdg-open";
        std::string oauth_cimd_url, oauth_client_id, oauth_scopes, oauth_grant = "authorization_code";
        unsigned short oauth_redirect_port = 0;
        std::vector<std::string> subscribe_uris;
        std::optional<std::string> requested_call;
        Json call_args = Json::object(), elicit_content = Json::object(), rpc_params = Json::object();
        bool progress = false, listen = false, demo = false;
        bool oauth = false, oauth_dcr = false, oauth_allow_insecure_loopback = false;
        std::size_t repeat = 1, repeat_delay_ms = 0, parallel = 1;
        std::size_t duration_ms = 5000;
        for (int i = 1; i < argc; ++i) {
            const std::string_view argument(argv[i]);
            const bool takes_value =
                argument == "--url" || argument == "--mode" || argument == "--cacert" || argument == "--token" ||
                argument == "--duration-ms" || argument == "--call" || argument == "--args" ||
                argument == "--log-level" || argument == "--elicit" || argument == "--elicit-content" ||
                argument == "--rpc" || argument == "--rpc-params" || argument == "--request" ||
                argument == "--params" || argument == "--task" ||
                argument == "--subscribe" || argument == "--subscribe-resource" ||
                argument == "--oauth-client-id" || argument == "--oauth-client-secret-file" ||
                argument == "--oauth-cimd-url" || argument == "--oauth-grant" ||
                argument == "--oauth-scopes" || argument == "--oauth-launcher" ||
                argument == "--oauth-redirect-port" || argument == "--oauth-store-file" ||
                argument == "--repeat" || argument == "--repeat-delay-ms" ||
                argument == "--parallel";
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
            else if (argument == "--request") request_method = argv[++i];
            else if (argument == "--params") rpc_params = parse_object("--params", argv[++i]);
            else if (argument == "--task") task_tool = argv[++i];
            else if (argument == "--subscribe" || argument == "--subscribe-resource") {
                subscribe_uris.emplace_back(argv[++i]);
            }
            else if (argument == "--oauth") oauth = true;
            else if (argument == "--oauth-client-id") oauth_client_id = argv[++i];
            else if (argument == "--oauth-client-secret-file") oauth_secret_file = argv[++i];
            else if (argument == "--oauth-cimd-url") oauth_cimd_url = argv[++i];
            else if (argument == "--oauth-dcr") oauth_dcr = true;
            else if (argument == "--oauth-grant") oauth_grant = argv[++i];
            else if (argument == "--oauth-scopes") oauth_scopes = argv[++i];
            else if (argument == "--oauth-launcher") oauth_launcher = argv[++i];
            else if (argument == "--oauth-redirect-port") {
                const auto port = parse_count("--oauth-redirect-port", argv[++i]);
                if (port > 65535) throw std::invalid_argument("--oauth-redirect-port is out of range");
                oauth_redirect_port = static_cast<unsigned short>(port);
            }
            else if (argument == "--oauth-store-file") oauth_store_file = argv[++i];
            else if (argument == "--oauth-allow-insecure-loopback") oauth_allow_insecure_loopback = true;
            else if (argument == "--repeat") repeat = parse_count("--repeat", argv[++i]);
            else if (argument == "--repeat-delay-ms") repeat_delay_ms = parse_duration(argv[++i]);
            else if (argument == "--parallel") parallel = parse_count("--parallel", argv[++i]);
            else throw std::invalid_argument("unknown option: " + std::string(argument));
        }
        if (options.url.empty()) throw std::invalid_argument("--url is required");
        if (oauth) {
            if (!token.empty()) throw std::invalid_argument("--oauth and --token cannot be used together");
            if (oauth_launcher != "http-follow" && oauth_launcher != "print" &&
                oauth_launcher != "xdg-open" && oauth_launcher != "open" &&
                oauth_launcher != "start") {
                throw std::invalid_argument("unsupported --oauth-launcher");
            }
            if (oauth_grant != "authorization_code" && oauth_grant != "client_credentials") {
                throw std::invalid_argument("--oauth-grant must be authorization_code or client_credentials");
            }
            cppmcp::client::OAuthProviderOptions oauth_options;
            oauth_options.resource_url = options.url;
            oauth_options.client_id = oauth_client_id;
            oauth_options.client_secret =
                oauth_secret_file.empty() ? std::string{} : read_secret_file(oauth_secret_file);
            oauth_options.cimd_url = oauth_cimd_url;
            oauth_options.grant_type = oauth_grant;
            oauth_options.launcher = oauth_launcher;
            oauth_options.redirect_port = oauth_redirect_port;
            oauth_options.enable_dcr = oauth_dcr;
            oauth_options.allow_insecure_loopback = oauth_allow_insecure_loopback;
            oauth_options.requested_scopes.clear();
            std::istringstream scope_stream(oauth_scopes);
            for (std::string scope; scope_stream >> scope;) {
                oauth_options.requested_scopes.insert(std::move(scope));
            }
            if (!oauth_store_file.empty()) {
                oauth_options.token_store =
                    std::make_shared<cppmcp::client::FileTokenStore>(oauth_store_file);
            }
            oauth_options.event_handler = [](std::string_view event) {
                static std::mutex output_mutex;
                std::lock_guard lock(output_mutex);
                std::cout << event << '\n';
            };
            options.auth_provider =
                std::make_shared<cppmcp::client::OAuthProvider>(std::move(oauth_options));
        }
        if (!token.empty()) options.auth_provider = std::make_shared<cppmcp::StaticBearerProvider>(token);
        if (!elicit_action.empty() || demo) options.client_capabilities["elicitation"] = {{"form", Json::object()}};

        const bool use_ext = demo || !rpc_method.empty() || !request_method.empty() ||
                             !task_tool.empty() || !subscribe_uris.empty();

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
            const auto execute_call = [&] {
                const auto result = client.call_tool(*requested_call, call_args, call_options);
                {
                    static std::mutex output_mutex;
                    std::lock_guard lock(output_mutex);
                    if (result.contains("content") && result["content"].is_array() &&
                        !result["content"].empty() && result["content"][0].contains("text") &&
                        result["content"][0]["text"].is_string()) {
                        std::cout << "CALL " << *requested_call << " -> "
                                  << result["content"][0]["text"].get<std::string>() << '\n';
                    }
                    std::cout << "RESULT " << result.dump() << '\n';
                }
                if (result.value("resultType", std::string{}) == "input_required" &&
                    result.contains("inputRequests") && result["inputRequests"].is_object()) {
                    for (auto item = result["inputRequests"].begin();
                         item != result["inputRequests"].end(); ++item) {
                        std::cout << "ELICIT "
                                  << item.value().value("params", Json::object())
                                         .value("message", std::string{}) << '\n';
                    }
                }
            };
            if (parallel > 1) {
                std::vector<std::thread> workers;
                std::vector<std::exception_ptr> errors(parallel);
                workers.reserve(parallel);
                for (std::size_t i = 0; i < parallel; ++i) {
                    workers.emplace_back([&, i] {
                        try {
                            execute_call();
                        } catch (...) {
                            errors[i] = std::current_exception();
                        }
                    });
                }
                for (auto& worker : workers) worker.join();
                for (const auto& error : errors) {
                    if (error) std::rethrow_exception(error);
                }
            } else {
                for (std::size_t i = 0; i < repeat; ++i) {
                    if (i != 0 && repeat_delay_ms != 0) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(repeat_delay_ms));
                    }
                    execute_call();
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
            if (!request_method.empty()) {
                std::function<Json(const std::string&, const Json&)> callback;
                if (!elicit_action.empty()) {
                    callback = [&](const std::string&, const Json& request) {
                        std::cout << "ELICIT "
                                  << request.value("params", Json::object())
                                         .value("message", std::string{})
                                  << '\n';
                        Json response = {{"action", elicit_action}};
                        if (elicit_action == "accept") response["content"] = elicit_content;
                        return response;
                    };
                }
                const auto result = features.request(request_method, rpc_params,
                                                     std::chrono::seconds(30),
                                                     std::move(callback));
                std::cout << "RESULT " << result.dump() << '\n';
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
            if (!subscribe_uris.empty() &&
                (!listen || !http->is_modern())) {
                for (const auto& uri : subscribe_uris) {
                    cppmcp::examples::step("resources/subscribe " + uri,
                                           [&] { return features.subscribe(uri); });
                }
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
                if (!subscribe_uris.empty()) filter["resourceSubscriptions"] = subscribe_uris;
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
