#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace cppmcp::core {
using Json = nlohmann::json;

namespace meta {

[[nodiscard]] inline std::optional<Json> get_optional(const Json& params,
                                                    std::string_view key) {
    if (!params.is_object()) {
        return std::nullopt;
    }
    const auto it = params.find(key.data());
    if (it == params.end()) {
        return std::nullopt;
    }
    return it.value();
}

[[nodiscard]] inline Json get(const Json& params, std::string_view key,
                              const Json& fallback = Json()) {
    if (!params.is_object()) {
        return fallback;
    }
    const auto it = params.find(key.data());
    if (it == params.end()) {
        return fallback;
    }
    return it.value();
}

[[nodiscard]] inline std::string get_string(const Json& params, std::string_view key,
                                            std::string_view fallback = "") {
    const Json value = get(params, key, Json());
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return std::string(fallback);
}

[[nodiscard]] inline Json get_meta(const Json& params) {
    if (!params.is_object()) {
        return Json::object();
    }
    const auto it = params.find("_meta");
    if (it == params.end() || !it.value().is_object()) {
        return Json::object();
    }
    return it.value();
}

[[nodiscard]] inline Json get_meta_value(const Json& params, std::string_view key,
                                        const Json& fallback = Json()) {
    const Json meta = get_meta(params);
    const auto it = meta.find(key.data());
    if (it == meta.end()) {
        return fallback;
    }
    return it.value();
}

[[nodiscard]] inline std::optional<Json> get_meta_optional(const Json& params,
                                                          std::string_view key) {
    const Json meta = get_meta(params);
    const auto it = meta.find(key.data());
    if (it == meta.end()) {
        return std::nullopt;
    }
    return it.value();
}

[[nodiscard]] inline std::string protocol_version(const Json& params,
                                                  std::string_view fallback = "") {
    const Json value = get_meta_value(params, "io.modelcontextprotocol/protocolVersion",
                                      Json());
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return std::string(fallback);
}

[[nodiscard]] inline Json client_capabilities(const Json& params) {
    return get_meta_value(params, "io.modelcontextprotocol/clientCapabilities", Json::object());
}

[[nodiscard]] inline Json client_info(const Json& params) {
    return get_meta_value(params, "io.modelcontextprotocol/clientInfo", Json::object());
}

[[nodiscard]] inline Json log_level(const Json& params) {
    return get_meta_value(params, "io.modelcontextprotocol/logLevel", Json());
}

[[nodiscard]] inline Json server_info(const Json& params) {
    return get_meta_value(params, "io.modelcontextprotocol/serverInfo", Json::object());
}

[[nodiscard]] inline std::string progress_token(const Json& params,
                                               std::string_view fallback = "") {
    const Json value = get_meta_value(params, "io.modelcontextprotocol/progressToken", Json());
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return std::string(fallback);
}

[[nodiscard]] inline std::string traceparent(const Json& params,
                                            std::string_view fallback = "") {
    const Json value = get_meta_value(params, "traceparent", Json());
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return std::string(fallback);
}

[[nodiscard]] inline std::string tracestate(const Json& params,
                                            std::string_view fallback = "") {
    const Json value = get_meta_value(params, "tracestate", Json());
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return std::string(fallback);
}

[[nodiscard]] inline std::string baggage(const Json& params,
                                         std::string_view fallback = "") {
    const Json value = get_meta_value(params, "baggage", Json());
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return std::string(fallback);
}

}  // namespace meta
}  // namespace cppmcp::core
