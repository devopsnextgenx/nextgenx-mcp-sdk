#pragma once

#include <chrono>
#include <thread>

#include "cppmcp/server.hpp"

namespace cppmcp::examples {

inline void add_phase4_tools(Server& server) {
    using Json = nlohmann::json;

    server.add_tool({
        .name = "structured_demo",
        .title = "Structured output demo",
        .description = "Return a structured city report.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"city", {{"type", "string"}}}}},
                         {"required", {"city"}},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties", {{"city", {{"type", "string"}}},
                                          {"temp_c", {{"type", "number"}}},
                                          {"conditions", {{"type", "string"}}}}},
                          {"required", {"city", "temp_c", "conditions"}},
                          {"additionalProperties", false}},
        .annotations = ToolAnnotation{.read_only_hint = true},
        .meta = Json::object(),
        .handler = [](const Json& args, const Json&) {
            return ToolResult::structured(
                {{"city", args.value("city", std::string{})},
                 {"temp_c", 21},
                 {"conditions", "clear"}}).to_json();
        },
    });

    server.add_tool({
        .name = "bad_output",
        .title = "Invalid output demo",
        .description = "Return an output that violates its declared schema.",
        .input_schema = {{"type", "object"}, {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties", {{"ok", {{"type", "boolean"}}}}},
                          {"required", {"ok"}},
                          {"additionalProperties", false}},
                .annotations = ToolAnnotation{},
                .meta = Json::object(),
        .handler = [](const Json&, const Json&) { return Json{{"ok", "nope"}}; },
    });

    server.add_tool({
        .name = "progress_demo",
        .title = "Progress demo",
        .description = "Emit progress updates while completing a short task.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"steps", {{"type", "integer"},
                                                      {"minimum", 1},
                                                      {"maximum", 20}}},
                                         {"delay_ms", {{"type", "integer"},
                                                        {"minimum", 0}}}}},
                         {"required", {"steps"}},
                         {"additionalProperties", false}},
                .annotations = ToolAnnotation{},
                .meta = Json::object(),
        .handler = {},
    }, [](const Json& args, const Json& params, ToolContext& context) {
        const auto steps = args.value("steps", 1);
        const auto delay = std::chrono::milliseconds(args.value("delay_ms", 10));
        (void)params;
        for (int step = 1; step <= steps; ++step) {
            if (context.cancelled()) {
                break;
            }
            context.report_progress(step, steps);
            std::this_thread::sleep_for(delay);
        }
        return Json{{"steps", steps}};
    });

    server.add_tool({
        .name = "log_demo",
        .title = "Logging demo",
        .description = "Emit one message at each supported logging level.",
        .input_schema = {{"type", "object"}, {"additionalProperties", false}},
        .annotations = ToolAnnotation{},
        .meta = Json::object(),
        .handler = {},
    }, [](const Json&, const Json&, ToolContext& context) {
        for (const auto* level : {"debug", "info", "warning", "error"}) {
            context.emit({{"jsonrpc", "2.0"},
                          {"method", "notifications/message"},
                          {"params", {{"level", level},
                                       {"logger", "cppmcp.example"},
                                       {"data", std::string("log at ") + level}}}});
        }
        return Json{{"logged", true}};
    });

    server.add_tool({
        .name = "delete_project",
        .title = "Delete project",
        .description = "Ask for confirmation before deleting a project.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"project", {{"type", "string"}}}}},
                         {"required", {"project"}},
                         {"additionalProperties", false}},
        .annotations = ToolAnnotation{.destructive_hint = true},
                .meta = Json::object(),
        .handler = [](const Json& args, const Json& params) {
            const auto responses = params.value("inputResponses", Json::object());
            const auto response = responses.find("confirm_delete");
            if (response == responses.end()) {
                return Json{{"resultType", "input_required"},
                            {"inputRequests",
                             {{"confirm_delete",
                               {{"method", "elicitation/create"},
                                {"params", {{"mode", "form"},
                                             {"message", "Confirm project deletion"},
                                             {"requestedSchema",
                                              {{"type", "object"},
                                               {"properties", {{"confirm", {{"type", "boolean"}}}}},
                                               {"required", {"confirm"}}}}}}}}}}};
            }
            const auto accepted = response->is_object() &&
                                 response->value("action", std::string{}) == "accept" &&
                                 response->value("content", Json::object())
                                         .value("confirm", false);
            const auto project = args.value("project", std::string{});
            return ToolResult::text(accepted ? "deleted " + project : "not deleted").to_json();
        },
    });

    server.add_tool({
        .name = "loop_input",
        .title = "Repeated input demo",
        .description = "Always request another input round.",
        .input_schema = {{"type", "object"}, {"additionalProperties", false}},
        .annotations = ToolAnnotation{},
        .meta = Json::object(),
        .handler = [](const Json&, const Json&) {
            return Json{{"resultType", "input_required"},
                        {"inputRequests",
                         {{"repeat",
                           {{"method", "elicitation/create"},
                            {"params", {{"mode", "form"},
                                         {"message", "Continue?"},
                                         {"requestedSchema",
                                          {{"type", "object"},
                                           {"properties", {{"confirm", {{"type", "boolean"}}}}},
                                           {"required", {"confirm"}}}}}}}}}}};
        },
    });
}

}  // namespace cppmcp::examples