#include "cppmcp/server.hpp"
#include "cppmcp/server/features.hpp"
#include "cppmcp/transport/http.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

unsigned short parse_port(const std::string& value) {
    std::size_t consumed = 0;
    const auto port = std::stoul(value, &consumed);
    if (consumed != value.size() || port == 0 ||
        port > std::numeric_limits<unsigned short>::max()) {
        throw std::invalid_argument("port must be between 1 and 65535");
    }
    return static_cast<unsigned short>(port);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        cppmcp::transport::HttpOptions http_options;
        http_options.port = 8001;
        http_options.bind_address = "127.0.0.1";
        if (argc == 3 && std::string(argv[1]) == "--port") {
            http_options.port = parse_port(argv[2]);
        } else if (argc != 1) {
            throw std::invalid_argument("usage: cppmcp_conformance_server [--port PORT]");
        }

        cppmcp::Server server(
            {"cppmcp-conformance", "cppmcp conformance fixture", "1.0"});
        server.prompt("test_prompt")
            .description("A small prompt for protocol conformance checks.")
            .handler([](const cppmcp::examples::CallContext&,
                        const nlohmann::json&) {
                return cppmcp::PromptResult{{
                    {"messages", nlohmann::json::array({{
                        {"role", "user"},
                        {"content", {{"type", "text"}, {"text", "Hello from cppmcp."}}}}})}}};
            });
        server.prompt("test_simple_prompt")
            .handler([](const cppmcp::examples::CallContext&,
                        const nlohmann::json&) {
                return cppmcp::PromptResult{{
                    {"messages", nlohmann::json::array({{
                        {"role", "user"},
                        {"content", {{"type", "text"},
                                     {"text", "This is a simple prompt for testing."}}}}})}}};
            });
        server.resource("test://static-text")
            .name("static-text")
            .mime_type("text/plain")
            .handler([](const cppmcp::examples::CallContext&) {
                return cppmcp::ReadResult{{
                    {"contents", nlohmann::json::array({{
                        {"uri", "test://static-text"},
                        {"mimeType", "text/plain"},
                        {"text", "This is the content of the static text resource."}}})}}};
            });
        server.resource("test://resource/text")
            .name("text")
            .mime_type("text/plain")
            .handler([](const cppmcp::examples::CallContext&) {
                return cppmcp::ReadResult{{
                    {"contents", nlohmann::json::array({{
                        {"uri", "test://resource/text"}, {"text", "conformance resource"}}})}}};
            });
        server.resource_template("test://resource/{name}")
            .name("resource")
            .mime_type("text/plain")
            .handler([](const cppmcp::examples::CallContext&,
                        const cppmcp::core::UriTemplate::Variables& variables) {
                const auto name = variables.at("name").get<std::string>();
                return cppmcp::ReadResult{{
                    {"contents", nlohmann::json::array({{
                        {"uri", "test://resource/" + name}, {"text", name}}})}}};
            });
        server.resource_template("test://template/{id}/data")
            .name("template")
            .mime_type("application/json")
            .handler([](const cppmcp::examples::CallContext&,
                        const cppmcp::core::UriTemplate::Variables& variables) {
                const auto id = variables.at("id").get<std::string>();
                const auto data = nlohmann::json{
                    {"id", id}, {"templateTest", true}, {"data", "Data for ID: " + id}};
                return cppmcp::ReadResult{{
                    {"contents", nlohmann::json::array({{
                        {"uri", "test://template/" + id + "/data"},
                        {"mimeType", "application/json"},
                        {"text", data.dump()}}})}}};
            });
        server.add_tool({
            .name = "test_simple_text",
            .title = "Simple text",
            .description = "Return the input text.",
            .input_schema = {
                {"type", "object"},
                {"properties", {{"text", {{"type", "string"}}}}},
                {"required", nlohmann::json::array({"text"})},
                {"additionalProperties", false}},
            .output_schema = nlohmann::json::object(),
            .annotations = {},
            .meta = nlohmann::json::object(),
            .handler = [](const nlohmann::json& arguments, const nlohmann::json&) {
                return arguments.at("text");
            }});

        std::cout << "Starting cppmcp conformance fixture on "
                  << http_options.bind_address << ':' << http_options.port << '\n';
        server.run_http(http_options);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "conformance server: " << error.what() << '\n';
        return 1;
    }
}
