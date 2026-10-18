#include "cppmcp/client.hpp"

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

std::size_t parse_duration(std::string_view value) {
    std::size_t duration = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), duration);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        throw std::invalid_argument("--duration-ms must be a non-negative integer");
    }
    return duration;
}

cppmcp::NegotiationPolicy parse_mode(std::string_view value) {
    if (value == "modern") {
        return cppmcp::NegotiationPolicy::modern_only;
    }
    if (value == "legacy") {
        return cppmcp::NegotiationPolicy::legacy_only;
    }
    if (value == "auto") {
        return cppmcp::NegotiationPolicy::prefer_modern;
    }
    throw std::invalid_argument("--mode must be modern, legacy, or auto");
}

std::string result_text(const nlohmann::json& result) {
    if (!result.contains("content") || !result["content"].is_array() ||
        result["content"].empty() || !result["content"][0].contains("text")) {
        throw std::runtime_error("tools/call add returned no text content");
    }
    return result["content"][0]["text"].get<std::string>();
}

}  // namespace

int main(int argc, char** argv) {
    try {
        cppmcp::HttpClientOptions options;
        std::string token;
        bool listen = false;
        std::size_t duration_ms = 5000;
        for (int i = 1; i < argc; ++i) {
            const std::string_view argument(argv[i]);
            if ((argument == "--url" || argument == "--mode" || argument == "--cacert" ||
                 argument == "--token" || argument == "--duration-ms") &&
                i + 1 >= argc) {
                throw std::invalid_argument("missing value for " + std::string(argument));
            }
            if (argument == "--url") {
                options.url = argv[++i];
            } else if (argument == "--mode") {
                options.policy = parse_mode(argv[++i]);
            } else if (argument == "--cacert") {
                options.ca_file = argv[++i];
            } else if (argument == "--token") {
                token = argv[++i];
            } else if (argument == "--listen") {
                listen = true;
            } else if (argument == "--duration-ms") {
                duration_ms = parse_duration(argv[++i]);
            } else {
                throw std::invalid_argument("unknown option: " + std::string(argument));
            }
        }
        if (options.url.empty()) {
            throw std::invalid_argument("--url is required");
        }
        if (!token.empty()) {
            options.auth_provider = std::make_shared<cppmcp::StaticBearerProvider>(token);
        }
        cppmcp::Client client;
        client.connect_http(std::move(options));
        std::cout << "MODE " << client.http_mode() << '\n';
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
        if (has_add) {
            const auto add_result = client.call_tool(
                "add", nlohmann::json{{"a", 1}, {"b", 2}});
            std::cout << "CALL add -> " << result_text(add_result) << '\n';
        }
        if (listen) {
            cppmcp::transport::ListenCallbacks callbacks;
            callbacks.on_stream_lost = [] { std::cout << "STREAM_LOST\n"; };
            callbacks.on_reconnected = [] { std::cout << "RECONNECTED\n"; };
            auto handle = client.listen(
                nlohmann::json{{"toolsListChanged", true}},
                [](const cppmcp::core::Notification& notification) {
                    std::cout << "NOTIFY " << notification.method << '\n';
                },
                std::move(callbacks));
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
