#include "cppmcp/core/json_rpc.hpp"
#include "test.hpp"

using cppmcp::core::ErrorCode;
using cppmcp::core::ErrorResponse;
using cppmcp::core::Message;
using cppmcp::core::Notification;
using cppmcp::core::Request;
using cppmcp::core::ResultResponse;

void test_json_rpc() {
    using namespace cppmcp::core;

    const auto request = parse_message(
        R"({"jsonrpc":"2.0","id":7,"method":"tools/list","params":{"cursor":"x"}})");
    CPPMCP_CHECK(std::holds_alternative<Message>(request));
    CPPMCP_CHECK(std::holds_alternative<Request>(std::get<Message>(request)));
    CPPMCP_CHECK(serialize_message(std::get<Message>(request)) ==
                 R"({"id":7,"jsonrpc":"2.0","method":"tools/list","params":{"cursor":"x"}})");

    const auto notification =
        parse_message(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    CPPMCP_CHECK(std::holds_alternative<Message>(notification));
    CPPMCP_CHECK(std::holds_alternative<Notification>(std::get<Message>(notification)));

    const auto string_id =
        parse_message(R"({"jsonrpc":"2.0","id":"req-1","result":{"resultType":"complete"}})");
    CPPMCP_CHECK(std::holds_alternative<Message>(string_id));
    CPPMCP_CHECK(std::holds_alternative<ResultResponse>(std::get<Message>(string_id)));

    const auto idless_error =
        parse_message(R"({"jsonrpc":"2.0","error":{"code":-32600,"message":"invalid"}})");
    CPPMCP_CHECK(std::holds_alternative<Message>(idless_error));
    CPPMCP_CHECK(std::holds_alternative<ErrorResponse>(std::get<Message>(idless_error)));
    CPPMCP_CHECK(!std::get<ErrorResponse>(std::get<Message>(idless_error)).id.has_value());

    const auto malformed = parse_message("{");
    CPPMCP_CHECK(std::holds_alternative<RpcError>(malformed));
    CPPMCP_CHECK(std::get<RpcError>(malformed).code ==
                 static_cast<std::int32_t>(ErrorCode::parse_error));

    for (const std::string_view invalid : {
             R"([])",
             R"({"jsonrpc":"1.0","id":1,"method":"x"})",
             R"({"jsonrpc":"2.0","id":null,"method":"x"})",
             R"({"jsonrpc":"2.0","id":1,"method":"x","params":[]})",
             R"({"jsonrpc":"2.0","id":1,"result":{},"error":{"code":1,"message":"x"}})",
             R"([{"jsonrpc":"2.0","id":1,"method":"x"}])",
         }) {
        const auto parsed = parse_message(invalid);
        CPPMCP_CHECK(std::holds_alternative<RpcError>(parsed));
        if (std::holds_alternative<RpcError>(parsed)) {
            CPPMCP_CHECK(std::get<RpcError>(parsed).code ==
                         static_cast<std::int32_t>(ErrorCode::invalid_request));
        }
    }

    const auto error = protocol_error(ErrorCode::header_mismatch, "Header mismatch");
    const auto encoded = error_response(error, RequestId{std::int64_t{9}});
    CPPMCP_CHECK(encoded["error"]["code"] == -32020);
    CPPMCP_CHECK(encoded["id"] == 9);
}
