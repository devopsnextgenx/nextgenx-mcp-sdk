#include "cppmcp/core/json_rpc.hpp"

#include <limits>
#include <exception>
#include <type_traits>
#include <utility>

namespace cppmcp::core {
namespace {

RpcError invalid_request(std::string message) {
    return protocol_error(ErrorCode::invalid_request, std::move(message));
}

bool valid_id(const Json& id) {
    if (id.is_string()) {
        return true;
    }
    if (id.is_number_integer()) {
        return true;
    }
    return id.is_number_unsigned() &&
           id.get<std::uint64_t>() <=
               static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
}

bool valid_error_code(const Json& code) {
    if (!code.is_number_integer()) {
        return false;
    }
    if (code.is_number_unsigned()) {
        return code.get<std::uint64_t>() <=
               static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
    }
    const auto value = code.get<std::int64_t>();
    return value >= std::numeric_limits<std::int32_t>::min() &&
           value <= std::numeric_limits<std::int32_t>::max();
}

RequestId request_id(const Json& id) {
    if (id.is_string()) {
        return id.get<std::string>();
    }
    if (id.is_number_unsigned()) {
        return static_cast<std::int64_t>(id.get<std::uint64_t>());
    }
    return id.get<std::int64_t>();
}

Json id_json(const RequestId& id) {
    return std::visit([](const auto& value) -> Json { return value; }, id);
}

Json error_json(const RpcError& error) {
    Json result = {{"code", error.code}, {"message", error.message}};
    if (error.data.has_value()) {
        result["data"] = *error.data;
    }
    return result;
}

}  // namespace

RpcError protocol_error(ErrorCode code, std::string message, std::optional<Json> data) {
    return RpcError{static_cast<std::int32_t>(code), std::move(message), std::move(data)};
}

ParseOutcome parse_message(std::string_view input) noexcept {
    try {
        const Json value = Json::parse(input);
        if (!value.is_object()) {
            return invalid_request("A JSON-RPC message must be a JSON object.");
        }

        const auto version = value.find("jsonrpc");
        if (version == value.end() || !version->is_string() || *version != "2.0") {
            return invalid_request("The jsonrpc member must be the string \"2.0\".");
        }

        const auto method = value.find("method");
        if (method != value.end()) {
            if (!method->is_string()) {
                return invalid_request("The method member must be a string.");
            }

            Json params = Json::object();
            const auto params_member = value.find("params");
            if (params_member != value.end()) {
                if (!params_member->is_object()) {
                    return invalid_request("MCP params must be a JSON object.");
                }
                params = *params_member;
            }

            const auto id = value.find("id");
            if (id == value.end()) {
                return Message{Notification{method->get<std::string>(), std::move(params)}};
            }
            if (!valid_id(*id)) {
                return invalid_request("A request id must be a string or integer, not null.");
            }
            return Message{Request{request_id(*id), method->get<std::string>(), std::move(params)}};
        }

        const auto id = value.find("id");
        const auto result = value.find("result");
        const auto error = value.find("error");
        if ((result == value.end()) == (error == value.end())) {
            return invalid_request("A response must contain exactly one of result or error.");
        }
        if (result != value.end()) {
            if (id == value.end() || !valid_id(*id)) {
                return invalid_request("A result response must have a string or integer id.");
            }
            if (!result->is_object()) {
                return invalid_request("An MCP result must be a JSON object.");
            }
            return Message{ResultResponse{request_id(*id), *result}};
        }

        if (id != value.end() && !valid_id(*id)) {
            return invalid_request("An error response id must be a string or integer.");
        }
        if (!error->is_object()) {
            return invalid_request("The error member must be a JSON object.");
        }
        const auto code = error->find("code");
        const auto message_member = error->find("message");
        if (code == error->end() || !valid_error_code(*code) ||
            message_member == error->end() || !message_member->is_string()) {
            return invalid_request("An error must contain an integer code and string message.");
        }

        std::optional<Json> data;
        const auto data_member = error->find("data");
        if (data_member != error->end()) {
            data = *data_member;
        }
        return Message{ErrorResponse{
            id == value.end() ? std::nullopt
                              : std::optional<RequestId>{request_id(*id)},
            RpcError{code->get<std::int32_t>(), message_member->get<std::string>(), std::move(data)}}};
    } catch (const Json::parse_error&) {
        return protocol_error(ErrorCode::parse_error, "Invalid JSON.");
    } catch (const Json::exception&) {
        return invalid_request("The JSON-RPC message contains an invalid member value.");
    } catch (const std::exception&) {
        return protocol_error(ErrorCode::internal_error, "Unable to parse the JSON-RPC message.");
    }
}

Json to_json(const Message& message) {
    return std::visit(
        [](const auto& value) -> Json {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, Request>) {
                return Json{{"jsonrpc", "2.0"},
                            {"id", id_json(value.id)},
                            {"method", value.method},
                            {"params", value.params}};
            } else if constexpr (std::is_same_v<T, Notification>) {
                return Json{{"jsonrpc", "2.0"},
                            {"method", value.method},
                            {"params", value.params}};
            } else if constexpr (std::is_same_v<T, ResultResponse>) {
                return Json{{"jsonrpc", "2.0"},
                            {"id", id_json(value.id)},
                            {"result", value.result}};
            } else {
                Json result{{"jsonrpc", "2.0"}, {"error", error_json(value.error)}};
                if (value.id.has_value()) {
                    result["id"] = id_json(*value.id);
                }
                return result;
            }
        },
        message);
}

std::string serialize_message(const Message& message) {
    return to_json(message).dump();
}

Json error_response(const RpcError& error, std::optional<RequestId> id) {
    Json result{{"jsonrpc", "2.0"}, {"error", error_json(error)}};
    if (id.has_value()) {
        result["id"] = id_json(*id);
    }
    return result;
}

}  // namespace cppmcp::core
