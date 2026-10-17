#include "cppmcp/core/json_rpc.hpp"
#include "test.hpp"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::vector<std::string> sorted_strings(const cppmcp::core::Json& values) {
    std::vector<std::string> result;
    for (const auto& value : values) {
        result.push_back(value.get<std::string>());
    }
    std::sort(result.begin(), result.end());
    return result;
}

}  // namespace

void test_schema_contract() {
    using cppmcp::core::Json;

    std::ifstream schema_file(CPPMCP_SCHEMA_PATH);
    CPPMCP_CHECK(schema_file.good());
    if (!schema_file.good()) {
        return;
    }
    const Json schema = Json::parse(schema_file);
    const auto& definitions = schema.at("$defs");
    CPPMCP_CHECK(definitions.contains("JSONRPCRequest"));
    CPPMCP_CHECK(definitions.contains("JSONRPCNotification"));
    CPPMCP_CHECK(definitions.contains("JSONRPCResultResponse"));
    CPPMCP_CHECK(definitions.contains("JSONRPCErrorResponse"));
    CPPMCP_CHECK(definitions.contains("HeaderMismatchError"));
    CPPMCP_CHECK(definitions.contains("MissingRequiredClientCapabilityError"));
    CPPMCP_CHECK(definitions.contains("UnsupportedProtocolVersionError"));

    CPPMCP_CHECK(sorted_strings(definitions.at("JSONRPCRequest").at("required")) ==
                 std::vector<std::string>({"id", "jsonrpc", "method"}));
    CPPMCP_CHECK(sorted_strings(definitions.at("JSONRPCNotification").at("required")) ==
                 std::vector<std::string>({"jsonrpc", "method"}));
    CPPMCP_CHECK(sorted_strings(definitions.at("JSONRPCResultResponse").at("required")) ==
                 std::vector<std::string>({"id", "jsonrpc", "result"}));
    CPPMCP_CHECK(sorted_strings(definitions.at("JSONRPCErrorResponse").at("required")) ==
                 std::vector<std::string>({"error", "jsonrpc"}));

    CPPMCP_CHECK(definitions.at("RequestId").at("type") ==
                 Json::array({"string", "integer"}));
    CPPMCP_CHECK(definitions.at("HeaderMismatchError").at("properties")
                     .at("error").at("allOf").at(1).at("properties").at("code")
                     .at("const") == -32020);
    CPPMCP_CHECK(definitions.at("MissingRequiredClientCapabilityError")
                     .at("properties").at("error").at("allOf").at(1)
                     .at("properties").at("code").at("const") == -32021);
    CPPMCP_CHECK(definitions.at("UnsupportedProtocolVersionError")
                     .at("properties").at("error").at("allOf").at(1)
                     .at("properties").at("code").at("const") == -32022);
}
