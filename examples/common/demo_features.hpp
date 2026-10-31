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
        .arguments = {{"code", "The code to review.", true},
                      {"language", "Programming language of the snippet.", false},
                      {"focus", "What to concentrate on.", false}},
        .render = [](const Json& args) {
            const auto language = args.value("language", std::string("the given language"));
            const auto focus = args.value("focus", std::string("correctness"));
            return Json{
                {"description", "Code review request"},
                {"messages",
                 Json::array({{{"role", "user"},
                               {"content",
                                {{"type", "text"},
                                 {"text", "Please review the following " + language +
                                              " code, focusing on " + focus + ":\n\n" +
                                              args.value("code", std::string{})}}}}})}};
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
            Json messages = Json::array();
            messages.push_back({{"role", "user"},
                                {"content",
                                 {{"type", "text"},
                                  {"text", "Write a two-sentence travel briefing for " + name + "."}}}});
            if (city) {
                messages.push_back(
                    {{"role", "user"},
                     {"content",
                      {{"type", "resource"},
                       {"resource",
                        {{"uri", std::string("demo://cities/") + city->name},
                         {"mimeType", "application/json"},
                         {"text", demo::city_json(*city).dump()}}}}}});
            }
            return Json{{"description", "Briefing for " + name}, {"messages", std::move(messages)}};
        },
    });

    registry.add_prompt_completion("code_review", "language", [](const std::string& typed, const Json&) {
        return prefix_filter({"c", "cpp", "csharp", "go", "java", "javascript", "kotlin", "python",
                              "rust", "swift", "typescript"},
                             typed);
    });
    registry.add_prompt_completion("code_review", "focus", [](const std::string& typed, const Json&) {
        return prefix_filter({"correctness", "performance", "security", "style", "thread-safety"}, typed);
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
        .read = [](const std::map<std::string, std::string>& vars) -> std::optional<ResourceContent> {
            const auto* city = demo::find_city(vars.at("city"));
            if (!city) return std::nullopt;
            return ResourceContent{demo::city_json(*city).dump(), std::nullopt, std::nullopt};
        },
    });
    registry.add_resource_template({
        .uri_template = "demo://notes/{id}",
        .name = "note",
        .title = "Note",
        .description = "Numbered notes 1-3.",
        .mime_type = "text/plain",
        .read = [](const std::map<std::string, std::string>& vars) -> std::optional<ResourceContent> {
            static const std::array<const char*, 3> notes{"First note.", "Second note.", "Third note."};
            const auto& id = vars.at("id");
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
}

}  // namespace cppmcp::examples
