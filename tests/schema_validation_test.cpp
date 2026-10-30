#include "cppmcp/core/json_schema.hpp"
#include "cppmcp/server.hpp"
#include "test.hpp"

#include <stdexcept>

namespace {

template <typename Fn>
bool throws_invalid_argument(Fn&& fn) {
    try {
        fn();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

}  // namespace

void test_schema_register() {
    auto validator = cppmcp::core::make_json_schema_validator();
    const nlohmann::json schema = {
        {"type", "object"},
        {"properties", {{"a", {{"type", "number"}}}, {"b", {{"type", "number"}}}}},
        {"required", {"a", "b"}},
    };
    const auto compiled = validator->compile(schema);
    CPPMCP_CHECK(!compiled->validate({{"a", 1}, {"b", 2}}));
    const auto error = compiled->validate({{"a", "bad"}, {"b", 2}});
    CPPMCP_CHECK(error.has_value());
    if (error) {
        CPPMCP_CHECK(error->instance_path == "/a");
        CPPMCP_CHECK(!error->message.empty());
    }

    cppmcp::Server server;
    cppmcp::ToolDefinition invalid_tool;
    invalid_tool.name = "invalid_schema_tool";
    invalid_tool.input_schema = {{"properties", {{"value", {{"type", "string"}}}}}};
    CPPMCP_CHECK(throws_invalid_argument([&] { server.add_tool(invalid_tool); }));

    cppmcp::ToolDefinition unsupported_dialect;
    unsupported_dialect.name = "unsupported_dialect_tool";
    unsupported_dialect.input_schema = {
        {"$schema", "https://example.com/unknown-schema"}, {"type", "object"}};
    bool named_error = false;
    try {
        server.add_tool(unsupported_dialect);
    } catch (const std::invalid_argument& exception) {
        named_error = std::string(exception.what()).find("unsupported_dialect_tool") !=
                      std::string::npos;
    }
    CPPMCP_CHECK(named_error);
}

void test_schema_limits() {
    auto validator = cppmcp::core::make_json_schema_validator();
    const auto remote_ref = nlohmann::json{
        {"type", "object"},
        {"properties", {{"value", {{"$ref", "https://example.com/schema.json"}}}}},
    };
    CPPMCP_CHECK(throws_invalid_argument([&] { validator->compile(remote_ref); }));

    const auto cyclic_ref = nlohmann::json{
        {"type", "object"},
        {"$defs", {{"recursive", {{"$ref", "#/$defs/recursive"}}}}},
    };
    CPPMCP_CHECK(throws_invalid_argument([&] { validator->compile(cyclic_ref); }));

    const nlohmann::json schema = {{"type", "object"}};
    cppmcp::core::SchemaValidationLimits small_schema;
    small_schema.max_schema_bytes = 4;
    CPPMCP_CHECK(throws_invalid_argument([&] { validator->compile(schema, small_schema); }));

    const auto string_schema = validator->compile({{"type", "string"}});
    cppmcp::core::SchemaValidationLimits small_instance;
    small_instance.max_instance_depth = 1;
    CPPMCP_CHECK(string_schema->validate(nlohmann::json{{"nested", {"value"}}}).has_value());
}

void test_output_schema() {
    cppmcp::Server server;
    cppmcp::ToolDefinition typed_result;
    typed_result.name = "typed_result";
    typed_result.input_schema = {{"type", "object"}};
    typed_result.output_schema = {{"type", "string"}};
    typed_result.handler = [](const nlohmann::json&, const nlohmann::json&) {
        return cppmcp::ToolResult::structured("ready");
    };
    server.add_tool(std::move(typed_result));

    cppmcp::ToolDefinition bad_result;
    bad_result.name = "bad_result";
    bad_result.input_schema = {{"type", "object"}, {"additionalProperties", false}};
    bad_result.output_schema = {{"type", "object"},
                                {"properties", {{"ok", {{"type", "boolean"}}}}},
                                {"required", {"ok"}}};
    bad_result.handler = [](const nlohmann::json&, const nlohmann::json&) {
        return nlohmann::json{{"ok", "nope"}};
    };
    server.add_tool(std::move(bad_result));

    auto response = server.handle_request({1, "tools/call",
                                           {{"name", "typed_result"},
                                            {"arguments", nlohmann::json::object()}}});
    const auto* valid_result = std::get_if<cppmcp::core::ResultResponse>(&response);
    CPPMCP_CHECK(valid_result != nullptr);
    if (valid_result != nullptr) {
        CPPMCP_CHECK(valid_result->result["structuredContent"] == "ready");
        CPPMCP_CHECK(valid_result->result["content"][0]["text"] == "\"ready\"");
    }

    response = server.handle_request({2, "tools/call",
                                      {{"name", "bad_result"},
                                       {"arguments", nlohmann::json::object()}}});
    const auto* output_error = std::get_if<cppmcp::core::ErrorResponse>(&response);
    CPPMCP_CHECK(output_error != nullptr);
    if (output_error != nullptr) {
        CPPMCP_CHECK(output_error->error.code ==
                     static_cast<std::int32_t>(cppmcp::core::ErrorCode::internal_error));
    }

    response = server.handle_request({3, "tools/call",
                                      {{"name", "bad_result"},
                                       {"arguments", {{"unexpected", true}}}}});
    const auto* invalid_arguments = std::get_if<cppmcp::core::ResultResponse>(&response);
    CPPMCP_CHECK(invalid_arguments != nullptr);
    if (invalid_arguments != nullptr) {
        CPPMCP_CHECK(invalid_arguments->result["isError"] == true);
    }

    response = server.handle_request({4, "tools/call",
                                      {{"name", "missing"},
                                       {"arguments", nlohmann::json::object()}}});
    const auto* unknown_tool = std::get_if<cppmcp::core::ErrorResponse>(&response);
    CPPMCP_CHECK(unknown_tool != nullptr);
    if (unknown_tool != nullptr) {
        CPPMCP_CHECK(unknown_tool->error.code ==
                     static_cast<std::int32_t>(cppmcp::core::ErrorCode::invalid_params));
    }
}