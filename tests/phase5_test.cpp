#include "test.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "demo_features.hpp"
#include "cppmcp/core/uri_template.hpp"
#include "cppmcp/transport/http.hpp"

namespace {

using cppmcp::core::Message;
using cppmcp::core::Request;
using cppmcp::examples::FeatureRegistry;
using cppmcp::examples::Json;

Message send(FeatureRegistry& registry, std::string method, Json params,
             std::int64_t id = 1, cppmcp::ToolContext* context = nullptr) {
    auto response = registry.try_handle(Request{id, std::move(method), std::move(params)}, context);
    CPPMCP_CHECK(response.has_value());
    return response.value_or(cppmcp::core::ErrorResponse{
        id, {-32603, "test dispatch failed", std::nullopt}});
}

Json result(const Message& message) {
    const auto* response = std::get_if<cppmcp::core::ResultResponse>(&message);
    CPPMCP_CHECK(response != nullptr);
    return response == nullptr ? Json::object() : response->result;
}

int error_code(const Message& message) {
    const auto* response = std::get_if<cppmcp::core::ErrorResponse>(&message);
    return response == nullptr ? 0 : response->error.code;
}

std::filesystem::path make_temp_root(std::string_view suffix) {
    auto path = std::filesystem::temp_directory_path() /
                ("cppmcp-phase5-" + std::string(suffix) + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path);
    return path;
}

void test_uri_template_vector(unsigned index) {
    const auto value = std::to_string(index);
    const auto uri = "demo://items/" + value + "/profile";
    const auto match = cppmcp::examples::detail::match_uri_template(
        "demo://items/{id}/profile", uri);
    CPPMCP_CHECK(match.has_value());
    CPPMCP_CHECK(match && match->at("id") == value);
}

}  // namespace

void test_phase5_case(std::string_view name) {
    using namespace cppmcp::examples;

    if (name.starts_with("uri_template_vector_")) {
        const auto suffix = name.substr(std::string_view("uri_template_vector_").size());
        unsigned index = 0;
        for (const char digit : suffix) {
            CPPMCP_CHECK(digit >= '0' && digit <= '9');
            index = index * 10U + static_cast<unsigned>(digit - '0');
        }
        test_uri_template_vector(index);
        return;
    }
    if (name == "uri_template_vectors") {
        const auto read_json = [](const char* filename) {
            std::ifstream input(std::filesystem::path(CPPMCP_URI_TEMPLATE_TEST_DATA) / filename);
            return Json::parse(input);
        };
        for (const auto* filename : {"spec-examples.json", "extended-tests.json"}) {
            const auto suites = read_json(filename);
            for (auto suite = suites.begin(); suite != suites.end(); ++suite) {
                const auto& variables = suite.value().at("variables");
                for (const auto& test : suite.value().at("testcases")) {
                    try {
                        const auto actual =
                            cppmcp::core::UriTemplate::parse(test[0].get<std::string>())
                                .expand(variables);
                        const auto& expected = test[1];
                        const bool found = expected.is_array()
                            ? std::any_of(expected.begin(), expected.end(),
                                          [&](const auto& option) { return option == actual; })
                            : expected == actual;
                        if (!found) {
                            std::cerr << filename << " vector " << test[0] << " expanded to "
                                      << actual << ", expected " << expected << '\n';
                        }
                        CPPMCP_CHECK(found);
                    } catch (const std::exception& error) {
                        std::cerr << filename << " vector " << test[0]
                                  << " threw: " << error.what() << '\n';
                        CPPMCP_CHECK(false);
                    }
                }
            }
        }
        const auto negative = read_json("negative-tests.json");
        const auto& negative_suite = negative.at("Failure Tests");
        const auto& negative_variables = negative_suite.at("variables");
        for (const auto& test : negative_suite.at("testcases")) {
            bool rejected = false;
            try {
                (void)cppmcp::core::UriTemplate::parse(test[0].get<std::string>())
                    .expand(negative_variables);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            if (!rejected) std::cerr << "invalid URI template accepted: " << test[0] << '\n';
            CPPMCP_CHECK(rejected);
        }
        return;
    }

    FeatureRegistry registry;
    add_demo_features(registry);

    if (name == "uri_template") {
        const auto match = detail::match_uri_template(
            "demo://users/{id}/profile{?format}", "demo://users/a%2Fb/profile?format=json");
        CPPMCP_CHECK(match.has_value());
        CPPMCP_CHECK(match && match->at("id") == "a/b");
        CPPMCP_CHECK(match && match->at("format") == "json");
        return;
    }
    if (name == "template_match") {
        const auto match = detail::match_uri_template(
            "demo://files/{+path}", "demo://files/folder/a%252eb.txt");
        CPPMCP_CHECK(match.has_value());
        CPPMCP_CHECK(match && match->at("path") == "folder/a%2eb.txt");
        CPPMCP_CHECK(!detail::match_uri_template("demo://items/{id}", "demo://items/a/b"));
        return;
    }
    if (name == "cursor") {
        const auto encoded = detail::encode_cursor(77, "resources", 3);
        const auto parsed =
            detail::parse_cursor(Json{{"cursor", encoded}}, 77, "resources");
        CPPMCP_CHECK(parsed && *parsed == 3);
        CPPMCP_CHECK(!detail::parse_cursor(
            Json{{"cursor", detail::encode_cursor(78, "resources", 3)}}, 77, "resources"));
        CPPMCP_CHECK(!detail::parse_cursor(
            Json{{"cursor", detail::encode_cursor(77, "prompts", 3)}}, 77, "resources"));
        CPPMCP_CHECK(!detail::parse_cursor(
            Json{{"cursor", encoded + "="}}, 77, "resources"));
        return;
    }
    if (name == "template_ambiguous") {
        const auto rejects = [](const auto& operation) {
            try {
                operation();
            } catch (const std::invalid_argument&) {
                return true;
            }
            return false;
        };
        CPPMCP_CHECK(rejects([] { (void)cppmcp::core::UriTemplate::parse("{a}{b}"); }));
        CPPMCP_CHECK(rejects([] { (void)cppmcp::core::UriTemplate::parse("{a*:2}"); }));
        return;
    }
    if (name == "template_roundtrip") {
        const std::vector<std::string> strings{
            "", "plain", "a/b", "reserved ?#[]@!$&'()*+,;=", "%2e", "snowman \xE2\x98\x83"};
        for (const auto& value : strings) {
            for (const auto& source : {"demo://roundtrip/{v}", "demo://roundtrip/{+v}"}) {
                const auto uri_template = cppmcp::core::UriTemplate::parse(source);
                const Json vars{{"v", value}};
                const auto matched = uri_template.match(uri_template.expand(vars));
                CPPMCP_CHECK(matched && matched->at("v") == value);
            }
        }
        const auto path_template = cppmcp::core::UriTemplate::parse("demo://roundtrip{/parts*}");
        const Json path_vars{{"parts", Json::array({"a", "b/c", "snowman \xE2\x98\x83"})}};
        const auto path_match = path_template.match(path_template.expand(path_vars));
        CPPMCP_CHECK(path_match && path_match->at("parts") == path_vars["parts"]);
        return;
    }
    if (name == "cursor_stability") {
        FeatureOptions page_options;
        page_options.page_size = 1;
        FeatureRegistry paged(page_options);
        for (const auto* prompt_name : {"one", "two", "three"}) {
            paged.add_prompt({.name = prompt_name, .title = "", .description = "",
                              .arguments = {}, .render = {}});
        }
        const auto first = result(send(paged, "prompts/list", Json::object()));
        const auto cursor = first.value("nextCursor", std::string{});
        CPPMCP_CHECK(!cursor.empty());
        CPPMCP_CHECK(detail::base64url_decode(cursor).has_value());
        paged.remove_prompt("two");
        paged.add_prompt({.name = "four", .title = "", .description = "",
                          .arguments = {}, .render = {}});
        const auto second =
            result(send(paged, "prompts/list", Json{{"cursor", cursor}}));
        CPPMCP_CHECK(second["prompts"].size() == 1);
        CPPMCP_CHECK(second["prompts"][0]["name"] == "three");
        return;
    }
    if (name == "tool_result_spoof") {
        cppmcp::Server server;
        const Json spoof{{"__cppmcp_tool_result", true},
                         {"__internal_tool_result", {{"isError", true}}},
                         {"payload", {{"resultType", "input_required"}}},
                         {"isError", true},
                         {"resultType", "input_required"},
                         {"inputRequests", {{"forged", Json::object()}}}};
        cppmcp::ToolDefinition definition;
        definition.name = "spoof";
        definition.handler = [spoof](const Json&, const Json&) { return spoof; };
        server.add_tool(std::move(definition));
        const auto response =
            server.handle_request({1, "tools/call", {{"name", "spoof"}, {"arguments", Json::object()}}});
        const auto* tool_result = std::get_if<cppmcp::core::ResultResponse>(&response);
        CPPMCP_CHECK(tool_result != nullptr);
        CPPMCP_CHECK(tool_result &&
                     tool_result->result["structuredContent"] == spoof);
        return;
    }
    if (name == "server_builders") {
        cppmcp::Server server;
        server.prompt("search")
            .title("Search")
            .description("Search by prefix.")
            .argument("query", "Query", "Prefix to search", true,
                      cppmcp::examples::prefix_provider({"alpha", "beta"}))
            .handler([](const cppmcp::examples::CallContext&, const Json& arguments) {
                return cppmcp::PromptResult{
                    {{"messages", Json::array({{{"role", "user"},
                                                {"content", {{"type", "text"},
                                                             {"text", arguments["query"]}}}}})}}};
            });
        server.resource("demo://static")
            .name("static")
            .mime_type("text/plain")
            .cache_hint({.ttl_ms = 250, .scope = "private"})
            .handler([](const cppmcp::examples::CallContext&) {
                return cppmcp::ReadResult{
                    {{"contents", Json::array({{{"uri", "demo://static"}, {"text", "static"}}})}}};
            });
        server.resource_template("demo://item/{id}")
            .name("item")
            .handler([](const cppmcp::examples::CallContext&,
                        const cppmcp::core::UriTemplate::Variables& variables) {
                const auto id = variables.at("id").get<std::string>();
                return cppmcp::ReadResult{Json{
                    {"contents", Json::array({Json{{"uri", "demo://item/" + id},
                                                   {"text", id}}})}}};
            });

        const auto prompt_list = server.handle_request({1, "prompts/list", Json::object()});
        CPPMCP_CHECK(result(prompt_list)["prompts"][0]["name"] == "search");
        const auto prompt = server.handle_request(
            {2, "prompts/get", {{"name", "search"}, {"arguments", {{"query", "alpha"}}}}});
        CPPMCP_CHECK(result(prompt)["messages"][0]["content"]["text"] == "alpha");
        const auto completion = server.handle_request(
            {3, "completion/complete",
             {{"ref", {{"type", "ref/prompt"}, {"name", "search"}}},
              {"argument", {{"name", "query"}, {"value", "al"}}}}});
        CPPMCP_CHECK(result(completion)["completion"]["values"] == Json::array({"alpha"}));
        const auto resource = server.handle_request(
            {4, "resources/read", {{"uri", "demo://static"}}});
        CPPMCP_CHECK(result(resource)["contents"][0]["text"] == "static");
        const auto templated = server.handle_request(
            {5, "resources/read", {{"uri", "demo://item/42"}}});
        CPPMCP_CHECK(result(templated)["contents"][0]["text"] == "42");
        return;
    }
    if (name == "file_sandbox") {
        const auto root = make_temp_root(name);
        {
            std::ofstream file(root / "readme.txt", std::ios::binary);
            file << "sandbox";
        }
        FileResourceProvider provider(root.string(), 32);
#ifdef _WIN32
        CPPMCP_CHECK(!provider.read("readme.txt"));
        FileResourceProvider unsafe_provider(root.string(), 32, true);
        CPPMCP_CHECK(unsafe_provider.read("readme.txt").has_value());
#else
        const auto content = provider.read("readme.txt");
        CPPMCP_CHECK(content && content->text == "sandbox");
        CPPMCP_CHECK(!provider.read("../outside.txt"));
        CPPMCP_CHECK(!provider.read("%252e%252e/secret"));
        CPPMCP_CHECK(!provider.read("C:/Windows/win.ini"));
        CPPMCP_CHECK(!provider.read("hidden/.private"));
#endif
        std::filesystem::remove_all(root);
        return;
    }
    if (name == "symlink") {
#ifndef _WIN32
        const auto root = make_temp_root(name);
        {
            std::ofstream file(root / "target.txt", std::ios::binary);
            file << "target";
        }
        std::filesystem::create_symlink(root / "target.txt", root / "link.txt");
        FileResourceProvider provider(root.string(), 32);
        CPPMCP_CHECK(!provider.read("link.txt"));
        std::filesystem::remove_all(root);
#else
        CPPMCP_CHECK(true);
#endif
        return;
    }
    if (name == "completion") {
        const auto response = result(send(
            registry, "completion/complete",
            {{"ref", {{"type", "ref/prompt"}, {"name", "code_review"}}},
             {"argument", {{"name", "framework"}, {"value", "d"}}},
             {"context", {{"arguments", {{"language", "python"}}}}}}));
        CPPMCP_CHECK(response["completion"]["values"] == Json::array({"django"}));
        CPPMCP_CHECK(error_code(send(
            registry, "completion/complete",
            {{"ref", {{"type", "ref/prompt"}, {"name", "missing"}}},
             {"argument", {{"name", "x"}, {"value", ""}}}})) == -32602);
        return;
    }
    if (name == "cache_hint") {
        const Request modern{1, "prompts/list",
                             {{"_meta", {{"io.modelcontextprotocol/protocolVersion",
                                          "2026-07-28"}}}}};
        const Request legacy{1, "prompts/list", Json::object()};
        const auto modern_result = detail::complete_result(modern, Json::object(), 0);
        const auto legacy_result = detail::complete_result(legacy, Json::object(), 0);
        CPPMCP_CHECK(modern_result["resultType"] == "complete");
        CPPMCP_CHECK(modern_result["ttlMs"] == 0);
        CPPMCP_CHECK(modern_result["cacheScope"] == "public");
        CPPMCP_CHECK(!legacy_result.contains("ttlMs"));
        CPPMCP_CHECK(!legacy_result.contains("cacheScope"));
        return;
    }
    if (name == "registry_churn") {
        std::atomic<bool> failed{false};
        std::atomic<std::size_t> notifications{0};
        registry.set_notifier([&](const std::string&, const Json&) {
            notifications.fetch_add(1, std::memory_order_relaxed);
        });
        registry.set_list_changed_notifier([&](const std::string&) {
            notifications.fetch_add(1, std::memory_order_relaxed);
        });
        std::vector<std::thread> workers;
        workers.reserve(100);
        for (int worker = 0; worker < 100; ++worker) {
            workers.emplace_back([&, worker] {
                for (int i = 0; i < 2; ++i) {
                    const auto prompt_name = "churn-" + std::to_string(worker);
                    const auto resource_uri = "demo://churn/" + std::to_string(worker);
                    PromptDefinition prompt;
                    prompt.name = prompt_name;
                    registry.add_prompt(std::move(prompt));
                    ResourceDefinition resource;
                    resource.uri = resource_uri;
                    resource.mime_type = "text/plain";
                    resource.read = [] {
                        return ResourceContent{"churn", std::nullopt, std::nullopt};
                    };
                    registry.add_resource(std::move(resource));
                    const auto prompt_list =
                        registry.try_handle(Request{i + 1, "prompts/list", Json::object()});
                    const auto resource_read = registry.try_handle(
                        Request{i + 1, "resources/read", {{"uri", resource_uri}}});
                    const auto subscribe = registry.try_handle(
                        Request{i + 1, "resources/subscribe", {{"uri", resource_uri}}});
                    registry.resource_updated(resource_uri);
                    const auto unsubscribe = registry.try_handle(
                        Request{i + 1, "resources/unsubscribe", {{"uri", resource_uri}}});
                    if (!prompt_list || !resource_read || !subscribe || !unsubscribe ||
                        !std::holds_alternative<cppmcp::core::ResultResponse>(*prompt_list) ||
                        !std::holds_alternative<cppmcp::core::ResultResponse>(*resource_read) ||
                        !std::holds_alternative<cppmcp::core::ResultResponse>(*subscribe) ||
                        !std::holds_alternative<cppmcp::core::ResultResponse>(*unsubscribe)) {
                        failed.store(true, std::memory_order_relaxed);
                    }
                    registry.remove_prompt(prompt_name);
                    registry.remove_resource(resource_uri);
                }
            });
        }
        for (auto& worker : workers) worker.join();
        CPPMCP_CHECK(!failed.load(std::memory_order_relaxed));
        CPPMCP_CHECK(notifications.load(std::memory_order_relaxed) >= 100);
        return;
    }
    if (name == "subscription_limit") {
        cppmcp::transport::SubscriptionRegistry subscriptions(1, 1, 1024);
        const auto first = subscriptions.subscribe("peer", "listen-1", {});
        CPPMCP_CHECK(first != 0);
        bool rejected = false;
        try {
            (void)subscriptions.subscribe("peer", "listen-2", {});
        } catch (const cppmcp::transport::SubscriptionLimitError&) {
            rejected = true;
        }
        CPPMCP_CHECK(rejected);
        subscriptions.unsubscribe(first);
        CPPMCP_CHECK(subscriptions.size() == 0);
        auto invalid_filter = cppmcp::transport::SubscriptionFilter{};
        invalid_filter.resource_subscriptions.emplace_back("invalid-\xff", 9);
        bool invalid_rejected = false;
        try {
            (void)subscriptions.subscribe("peer", "listen-3", std::move(invalid_filter));
        } catch (const std::invalid_argument&) {
            invalid_rejected = true;
        }
        CPPMCP_CHECK(invalid_rejected);
        return;
    }
    if (name == "list_changed") {
        std::vector<std::string> events;
        registry.set_list_changed_notifier(
            [&](const std::string& kind) { events.push_back(kind); });
        PromptDefinition prompt;
        prompt.name = "dynamic";
        registry.add_prompt(std::move(prompt));
        ResourceDefinition resource;
        resource.uri = "demo://dynamic";
        registry.add_resource(std::move(resource));
        ResourceTemplateDefinition resource_template;
        resource_template.uri_template = "demo://dynamic/{id}";
        registry.add_resource_template(std::move(resource_template));
        CPPMCP_CHECK(events == std::vector<std::string>({"prompts", "resources", "resources"}));
        return;
    }
    if (name == "mrtr_prompt") {
        cppmcp::ToolContext pending_context;
        const auto prompt = send(registry, "prompts/get", {{"name", "ask_name"}},
                                 1, &pending_context);
        CPPMCP_CHECK(result(prompt).value("resultType", std::string{}) == "input_required");
        cppmcp::ToolContext context;
        context.set_input_responses({{"name", {{"action", "accept"}, {"content", {{"name", "Ada"}}}}}});
        const auto completed = result(send(registry, "prompts/get", {{"name", "ask_name"}},
                                           2, &context));
        CPPMCP_CHECK(completed["messages"][0]["content"]["text"] == "Hello Ada!");
        return;
    }
    if (name == "mrtr_resource") {
        const auto pending = result(send(
            registry, "resources/read", {{"uri", "demo://secret/needs_confirm"}}));
        CPPMCP_CHECK(pending.value("resultType", std::string{}) == "input_required");
        cppmcp::ToolContext context;
        context.set_input_responses(
            {{"confirm", {{"action", "accept"}, {"content", {{"confirm", true}}}}}});
        const auto completed = result(send(
            registry, "resources/read", {{"uri", "demo://secret/needs_confirm"}}, 2, &context));
        CPPMCP_CHECK(completed["contents"][0]["text"] == "secret-value");
        return;
    }
    if (name == "mcp_name_uri_encoding") {
        const std::vector<std::string> values{
            "demo://resource/a b", "demo://resource/caf\xc3\xa9",
            "demo://resource/a%2Fb", "demo://resource/a?b={c}"};
        for (const auto& value : values) {
            std::string decoded;
            CPPMCP_CHECK(cppmcp::transport::decode_mirrored_header_value(
                cppmcp::transport::encode_mirrored_header_value(value), decoded));
            CPPMCP_CHECK(decoded == value);
        }
        return;
    }
    CPPMCP_CHECK(false);
}
