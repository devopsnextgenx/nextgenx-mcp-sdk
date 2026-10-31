#pragma once

// Shared demo tours used by the stdio and HTTP example clients.

#include <iostream>
#include <string>

#include "cppmcp/client.hpp"
#include "mcp_client_ext.hpp"

namespace cppmcp::examples {

// Runs `fn`, printing either its result or the error (a failure must not stop the tour).
template <typename Fn>
inline void step(const std::string& label, Fn&& fn) {
    try {
        std::cout << label << " -> " << fn().dump() << '\n';
    } catch (const std::exception& e) {
        std::cout << label << " !! " << e.what() << '\n';
    }
}

// Phase-4 tools: structured output, output-schema validation, progress, logging, elicitation.
inline void run_tool_tour(Client& client) {
    step("tools/call echo", [&] { return client.call_tool("echo", {{"value", "hello"}}); });
    step("tools/call add", [&] { return client.call_tool("add", {{"a", 15}, {"b", 7}}); });
    step("tools/call structured_demo", [&] { return client.call_tool("structured_demo", {{"city", "Detroit"}}); });
    step("tools/call bad_output (expect schema error)", [&] { return client.call_tool("bad_output"); });

    step("tools/call progress_demo", [&] {
        Client::CallToolOptions options;
        options.on_progress = [](double current, std::optional<double> total, std::optional<std::string>) {
            std::cout << "  PROGRESS " << current << '/' << total.value_or(0) << '\n';
        };
        return client.call_tool("progress_demo", {{"steps", 3}, {"delay_ms", 20}}, std::move(options));
    });

    step("tools/call log_demo", [&] {
        Client::CallToolOptions options;
        options.log_level = "debug";
        options.on_log = [](const std::string& level, const std::string&, const Json& data) {
            std::cout << "  LOG " << level << ' ' << (data.is_string() ? data.get<std::string>() : data.dump()) << '\n';
        };
        return client.call_tool("log_demo", Json::object(), std::move(options));
    });

    step("tools/call delete_project (elicit: accept)", [&] {
        Client::CallToolOptions options;
        options.client_capabilities["elicitation"] = {{"form", Json::object()}};
        options.on_elicit = [](const std::string&, const Json& request) {
            std::cout << "  ELICIT " << request.value("params", Json::object()).value("message", std::string{}) << '\n';
            return Json{{"action", "accept"}, {"content", {{"confirm", true}}}};
        };
        return client.call_tool("delete_project", {{"project", "demo"}}, std::move(options));
    });
}

inline void run_prompt_tour(FeatureClient& fc) {
    step("prompts/list", [&] { return fc.list_prompts(); });
    step("prompts/get code_review", [&] {
        return fc.get_prompt("code_review", {{"code", "int main() {}"}, {"language", "cpp"}, {"focus", "style"}});
    });
    step("prompts/get city_briefing (embedded resource)", [&] { return fc.get_prompt("city_briefing", {{"city", "Tokyo"}}); });
    step("prompts/get code_review (missing arg -> error)", [&] { return fc.get_prompt("code_review"); });
}

inline void run_completion_tour(FeatureClient& fc) {
    step("completion prompt code_review.language 'ru'", [&] { return fc.complete_prompt("code_review", "language", "ru"); });
    step("completion prompt city_briefing.city 'n'", [&] { return fc.complete_prompt("city_briefing", "city", "n"); });
    step("completion resource demo://cities/{city} 'l'", [&] {
        return fc.complete_resource("demo://cities/{city}", "city", "l");
    });
    step("completion resource demo://notes/{id} ''", [&] { return fc.complete_resource("demo://notes/{id}", "id", ""); });
}

inline void run_resource_tour(FeatureClient& fc) {
    step("resources/list", [&] { return fc.list_resources(); });
    step("resources/templates/list", [&] { return fc.list_resource_templates(); });
    step("resources/read demo://readme", [&] { return fc.read_resource("demo://readme"); });
    step("resources/read demo://pixel.png (blob)", [&] { return fc.read_resource("demo://pixel.png"); });
    step("resources/read demo://cities/Lisbon (template)", [&] { return fc.read_resource("demo://cities/Lisbon"); });
    step("resources/read demo://notes/9 (expect not found)", [&] { return fc.read_resource("demo://notes/9"); });
}

inline void run_task_tour(FeatureClient& fc) {
    try {
        const auto task = fc.create_task("progress_demo", {{"steps", 5}, {"delay_ms", 100}});
        const std::string id = task.at("taskId").get<std::string>();
        std::cout << "tasks/create -> " << task.dump() << '\n';
        step("tasks/list", [&] { return fc.list_tasks(); });
        fc.wait_for_task(id, std::chrono::seconds(30), [](const Json& t) {
            std::cout << "  TASK " << t.value("status", std::string{}) << '\n';
        });
        step("tasks/result", [&] { return fc.task_result(id); });
    } catch (const std::exception& e) {
        std::cout << "task tour !! " << e.what() << '\n';
    }
    // Cancellation: start a long task and cancel it.
    try {
        const auto task = fc.create_task("progress_demo", {{"steps", 20}, {"delay_ms", 500}});
        const std::string id = task.at("taskId").get<std::string>();
        step("tasks/cancel", [&] { return fc.cancel_task(id); });
        step("tasks/get after cancel", [&] { return fc.get_task(id); });
        step("tasks/result after cancel (expect error)", [&] { return fc.task_result(id); });
    } catch (const std::exception& e) {
        std::cout << "cancel tour !! " << e.what() << '\n';
    }
}

inline void run_feature_tour(FeatureClient& fc) {
    run_prompt_tour(fc);
    run_completion_tour(fc);
    run_resource_tour(fc);
    run_task_tour(fc);
}

}  // namespace cppmcp::examples
