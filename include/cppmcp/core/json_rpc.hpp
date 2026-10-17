#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <nlohmann/json.hpp>

namespace cppmcp::core {

using Json = nlohmann::json;
using RequestId = std::variant<std::string, std::int64_t>;

struct RpcError {
    std::int32_t code;
    std::string message;
    std::optional<Json> data;
};

enum class ErrorCode : std::int32_t {
    parse_error = -32700,
    invalid_request = -32600,
    method_not_found = -32601,
    invalid_params = -32602,
    internal_error = -32603,
    header_mismatch = -32020,
    missing_required_client_capability = -32021,
    unsupported_protocol_version = -32022,
};

struct Request {
    RequestId id;
    std::string method;
    Json params = Json::object();
};

struct Notification {
    std::string method;
    Json params = Json::object();
};

struct ResultResponse {
    RequestId id;
    Json result = Json::object();
};

struct ErrorResponse {
    std::optional<RequestId> id;
    RpcError error;
};

using Message = std::variant<Request, Notification, ResultResponse, ErrorResponse>;
using ParseOutcome = std::variant<Message, RpcError>;

[[nodiscard]] ParseOutcome parse_message(std::string_view input,
                                         bool allow_batch = false) noexcept;
[[nodiscard]] Json to_json(const Message& message);
[[nodiscard]] std::string serialize_message(const Message& message);
[[nodiscard]] Json error_response(const RpcError& error,
                                  std::optional<RequestId> id = std::nullopt);
[[nodiscard]] RpcError protocol_error(ErrorCode code, std::string message,
                                      std::optional<Json> data = std::nullopt);

}  // namespace cppmcp::core
