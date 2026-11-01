#pragma once

// Demo prompts / resources / templates / completions shared by the stdio and HTTP servers.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include "mcp_features.hpp"

namespace cppmcp::examples {

namespace demo {

inline std::atomic<int> counter_value{0};
inline std::atomic<int> other_value{0};

struct City {
    const char* name;
    int temp_c;
    const char* conditions;
};

// Same shape as the `structured_demo` tool in phase4_tools.hpp.
inline constexpr std::array<City, 4> kCities{{
    {"Detroit", 12, "overcast"},
    {"Lisbon", 22, "sunny"},
    {"Nairobi", 24, "partly cloudy"},
    {"Tokyo", 18, "clear"},
}};

inline const City* find_city(const std::string& name) {
    const auto wanted = detail::lower(name);
    for (const auto& city : kCities) {
        if (detail::lower(city.name) == wanted) return &city;
    }
    return nullptr;
}

inline Json city_json(const City& city) {
    return {{"city", city.name}, {"temp_c", city.temp_c}, {"conditions", city.conditions}};
}

inline std::vector<std::string> prefix_filter(std::vector<std::string> candidates,
                                              const std::string& typed) {
    const auto needle = detail::lower(typed);
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [&](const std::string& c) {
                                        return detail::lower(c).rfind(needle, 0) != 0;
                                    }),
                     candidates.end());
    return candidates;
}

// 1x1 transparent PNG, to demonstrate binary (blob) resources.
inline constexpr const char* kPixelPngBase64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";

}  // namespace demo

inline void add_demo_features(FeatureRegistry& registry) {
    using demo::prefix_filter;

    // ---- prompts ---------------------------------------------------------
    registry.add_prompt({
        .name = "code_review",
        .title = "Code review",
        .description = "Ask for a focused review of a code snippet.",
        .arguments = {{"language", "Programming language of the snippet.", true},
                      {"framework", "Framework used by the snippet.", false}},
        .render = [](const Json& args) {
            const auto language = args.value("language", std::string{});
            const auto framework = args.value("framework", std::string{});
            return Json{
                {"description", "Code review request"},
                {"messages",
                 Json::array({{{"role", "user"},
                               {"content",
                                {{"type", "text"},
                                 {"text", "Please review the supplied " + language +
                                              (framework.empty() ? std::string{} :
                                               " code using " + framework) + "."}}}}})}};
        },
    });

    registry.add_prompt({
        .name = "summarize",
        .title = "Summarize",
        .description = "Summarize the supplied text.",
        .arguments = {{"text", "Text to summarize.", true},
                      {"style", "Summary style.", false}},
        .render = [](const Json& args) {
            const auto style = args.value("style", std::string("brief"));
            return Json{{"description", "Summary in " + style + " style"},
                        {"messages", Json::array({{{"role", "user"},
                                                   {"content", {{"type", "text"},
                                                                {"text", "Summarize this text (" +
                                                                             style + "):\n\n" +
                                                                             args.value("text", std::string{})}}}}})}};
        },
    });

    registry.add_prompt({
        .name = "ask_name",
        .title = "Ask for a name",
        .description = "Greet a person by name.",
        .arguments = {},
        .render = [](const Json&) {
            return Json{{"description", "A greeting"},
                        {"messages", Json::array({{{"role", "user"},
                                                   {"content", {{"type", "text"},
                                                                {"text", "Hello!"}}}}})}};
        },
        .context_render = [](const Json&, ToolContext& context) {
            if (!context.input_responses().contains("name")) {
                return Json{{"resultType", "input_required"},
                            {"inputRequests",
                             {{"name",
                               {{"method", "elicitation/create"},
                                {"params",
                                 {{"mode", "form"},
                                  {"message", "What is your name?"},
                                  {"requestedSchema",
                                   {{"type", "object"},
                                    {"properties", {{"name", {{"type", "string"}}}}},
                                    {"required", {"name"}},
                                    {"additionalProperties", false}}}}}}}}},
                            {"privateState", Json::object()}};
            }
            const auto response = context.input_responses().value("name", Json::object());
            const auto name = response.value("action", std::string{}) == "accept"
                                  ? response.value("content", Json::object())
                                        .value("name", std::string{})
                                  : std::string{};
            const auto greeting = name.empty() ? std::string("Hello!")
                                                : "Hello " + name + "!";
            return Json{{"description", "A greeting"},
                        {"messages", Json::array({{{"role", "user"},
                                                   {"content", {{"type", "text"},
                                                                {"text", greeting}}}}})}};
        },
    });

    registry.add_prompt({
        .name = "city_briefing",
        .title = "City briefing",
        .description = "Write a short briefing using the embedded city report resource.",
        .arguments = {{"city", "City to brief on.", true}},
        .render = [](const Json& args) {
            const auto name = args.value("city", std::string{});
            const auto* city = demo::find_city(name);
            Json messages = Json::array({{{"role", "user"},
                                          {"content", {{"type", "text"},
                                                       {"text", "Write a two-sentence travel briefing for " +
                                                                    name + "."}}}}});
            if (city) {
                messages.push_back(
                    {{"role", "user"},
                     {"content", {{"type", "resource"},
                                  {"resource", {{"uri", std::string("demo://cities/") + city->name},
                                                {"mimeType", "application/json"},
                                                {"text", demo::city_json(*city).dump()}}}}}});
            }
            return Json{{"description", "Briefing for " + name}, {"messages", std::move(messages)}};
        },
    });

    registry.add_prompt_completion("summarize", "style", [](const std::string& typed, const Json&) {
        return prefix_filter({"brief", "detailed", "bullets"}, typed);
    });
    registry.add_prompt_completion(
        "code_review", "framework", [](const std::string& typed, const Json& context) {
            const auto language = context.value("language", std::string{});
            if (language == "python") return prefix_filter({"django", "flask"}, typed);
            if (language == "rust") return prefix_filter({"axum", "actix"}, typed);
            return std::vector<std::string>{};
        });
    registry.add_prompt_completion("city_briefing", "city", [](const std::string& typed, const Json&) {
        std::vector<std::string> names;
        for (const auto& c : demo::kCities) names.emplace_back(c.name);
        return prefix_filter(std::move(names), typed);
    });

    // ---- static resources ------------------------------------------------
    registry.add_resource({
        .uri = "demo://readme",
        .name = "readme",
        .title = "Server README",
        .description = "What this demo server offers.",
        .mime_type = "text/markdown",
        .read = [] {
            return ResourceContent{
                "# cppmcp demo server\n\nTools, prompts, resources, completions and tasks.\n",
                std::nullopt, std::nullopt};
        },
    });
    registry.add_resource({
        .uri = "demo://image",
        .name = "image",
        .title = "1x1 PNG",
        .description = "Binary resource returned as a base64 blob.",
        .mime_type = "image/png",
        .read = [] {
            return ResourceContent{std::nullopt, std::string(demo::kPixelPngBase64), std::nullopt};
        },
    });
    registry.add_resource({
        .uri = "demo://counter",
        .name = "counter",
        .title = "Counter",
        .description = "A counter resource.",
        .mime_type = "text/plain",
        .read = [] {
            return ResourceContent{
                "counter=" + std::to_string(demo::counter_value.load(std::memory_order_relaxed)),
                std::nullopt, std::nullopt};
        },
    });
    registry.add_resource({
        .uri = "demo://other",
        .name = "other",
        .title = "Other counter",
        .description = "An alternate changing resource.",
        .mime_type = "text/plain",
        .read = [] {
            return ResourceContent{
                "other=" + std::to_string(demo::other_value.load(std::memory_order_relaxed)),
                std::nullopt, std::nullopt};
        },
    });
    registry.add_resource({
        .uri = "demo://secret/needs_confirm",
        .name = "confirmed_secret",
        .title = "Confirmed secret",
        .description = "Requires explicit confirmation before returning the value.",
        .mime_type = "text/plain",
        .context_read = [](const std::string& uri, ToolContext& context) {
            if (!context.input_responses().contains("confirm")) {
                return Json{{"resultType", "input_required"},
                            {"inputRequests",
                             {{"confirm",
                               {{"method", "elicitation/create"},
                                {"params",
                                 {{"mode", "form"},
                                  {"message", "Confirm access to the secret resource."},
                                  {"requestedSchema",
                                   {{"type", "object"},
                                    {"properties", {{"confirm", {{"type", "boolean"}}}}},
                                    {"required", {"confirm"}},
                                    {"additionalProperties", false}}}}}}}}},
                            {"privateState", Json::object()}};
            }
            const auto response = context.input_responses().value("confirm", Json::object());
            const bool approved = response.value("action", std::string{}) == "accept" &&
                                  response.value("content", Json::object())
                                      .value("confirm", false);
            return Json{{"contents",
                         Json::array({{{"uri", uri},
                                       {"mimeType", "text/plain"},
                                       {"text", approved ? "secret-value" : "access denied"}}})}};
        },
    });
    registry.add_resource({
        .uri = "demo://config",
        .name = "config",
        .title = "Server configuration",
        .description = "Static JSON configuration.",
        .mime_type = "application/json",
        .read = [] {
            return ResourceContent{Json{{"name", "cppmcp-demo"}, {"features", {"tools", "prompts", "resources", "completions", "tasks"}}}.dump(),
                                   std::nullopt, std::nullopt};
        },
    });
    registry.add_resource({
        .uri = "demo://clock",
        .name = "clock",
        .title = "Server clock",
        .description = "Current UTC time; changes on every read.",
        .mime_type = "text/plain",
        .read = [] {
            return ResourceContent{detail::iso8601(std::chrono::system_clock::now()),
                                   std::nullopt, std::nullopt};
        },
    });
    registry.add_resource({
        .uri = "demo://pixel.png",
        .name = "pixel",
        .title = "1x1 PNG",
        .description = "Binary resource returned as a base64 blob.",
        .mime_type = "image/png",
        .read = [] {
            return ResourceContent{std::nullopt, std::string(demo::kPixelPngBase64), std::nullopt};
        },
    });

    // ---- templates -------------------------------------------------------
    registry.add_resource_template({
        .uri_template = "demo://cities/{city}",
        .name = "city_report",
        .title = "City report",
        .description = "Weather report for a known city.",
        .mime_type = "application/json",
        .read = [](const core::UriTemplate::Variables& vars) -> std::optional<ResourceContent> {
            const auto* city = demo::find_city(vars.at("city").get<std::string>());
            if (!city) return std::nullopt;
            return ResourceContent{demo::city_json(*city).dump(), std::nullopt, std::nullopt};
        },
    });
    registry.add_resource_template({
        .uri_template = "demo://users/{id}/profile{?format}",
        .name = "user_profile",
        .title = "User profile",
        .description = "A profile for a user ID, optionally formatted as JSON.",
        .mime_type = "text/plain",
        .read = [](const core::UriTemplate::Variables& vars)
            -> std::optional<ResourceContent> {
            const auto id = vars.at("id").get<std::string>();
            if (const auto format = vars.find("format");
                format != vars.end() && format->second == "json") {
                return ResourceContent{Json{{"id", id}, {"name", "User " + id}}.dump(),
                                       std::nullopt, "application/json"};
            }
            return ResourceContent{"Profile for user " + id, std::nullopt, std::nullopt};
        },
    });
    registry.add_resource_completion(
        "demo://users/{id}/profile{?format}", "id",
        [](const std::string& typed, const Json&) {
            return prefix_filter({"1", "2", "42", "420", "7"}, typed);
        });
    registry.add_resource_template({
        .uri_template = "demo://notes/{id}",
        .name = "note",
        .title = "Note",
        .description = "Numbered notes 1-3.",
        .mime_type = "text/plain",
        .read = [](const core::UriTemplate::Variables& vars) -> std::optional<ResourceContent> {
            static const std::array<const char*, 3> notes{"First note.", "Second note.", "Third note."};
            const auto id = vars.at("id").get<std::string>();
            if (id.size() != 1 || id[0] < '1' || id[0] > '3') return std::nullopt;
            return ResourceContent{std::string(notes[static_cast<std::size_t>(id[0] - '1')]),
                                   std::nullopt, std::nullopt};
        },
    });
    registry.add_resource_completion("demo://cities/{city}", "city", [](const std::string& typed, const Json&) {
        std::vector<std::string> names;
        for (const auto& c : demo::kCities) names.emplace_back(c.name);
        return prefix_filter(std::move(names), typed);
    });
    registry.add_resource_completion("demo://notes/{id}", "id", [](const std::string& typed, const Json&) {
        return prefix_filter({"1", "2", "3"}, typed);
    });

    const auto add_operator_resource = [&registry](std::string uri_template) {
        registry.add_resource_template({
            .uri_template = std::move(uri_template),
            .name = "uri_template_match",
            .title = "URI template match",
            .description = "Return the variables matched by an RFC 6570 URI template.",
            .mime_type = "application/json",
            .read = [](const core::UriTemplate::Variables& variables) {
                Json result = Json::object();
                for (const auto& [name, value] : variables) result[name] = value;
                return std::optional<ResourceContent>(
                    ResourceContent{result.dump(), std::nullopt, "application/json"});
            },
        });
    };
    add_operator_resource("demo://m/simple/{a}/{b}");
    add_operator_resource("demo://m/reserved/{+p}");
    add_operator_resource("demo://m/frag{#f}");
    add_operator_resource("demo://m/label{.x}");
    add_operator_resource("demo://m/path{/segs*}");
    add_operator_resource("demo://m/semi{;k,v}");
    add_operator_resource("demo://m/query{?q,limit}");
    add_operator_resource("demo://m/cont?fixed=1{&extra}");
}

}  // namespace cppmcp::examples
