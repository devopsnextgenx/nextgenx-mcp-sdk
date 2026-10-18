#include "test.hpp"

#include <string>
#include <vector>

#include "cppmcp/transport/http_client.hpp"

void test_www_auth() {
    const auto challenges = cppmcp::parse_www_authenticate({
        "Basic realm=\"legacy\", Bearer error=\"insufficient_scope\", "
        "scope=\"read:tools\", resource_metadata=\"https://example.test/meta\", "
        "error_description=\"Needs \\\"tool access\\\"\"",
    });
    CPPMCP_CHECK(challenges.size() == 2);
    if (challenges.size() == 2) {
        CPPMCP_CHECK(challenges[0].scheme == "Basic");
        CPPMCP_CHECK(challenges[1].scheme == "Bearer");
        CPPMCP_CHECK(challenges[1].error == "insufficient_scope");
        CPPMCP_CHECK(challenges[1].scope == "read:tools");
        CPPMCP_CHECK(challenges[1].resource_metadata == "https://example.test/meta");
        CPPMCP_CHECK(challenges[1].error_description == "Needs \"tool access\"");
    }

    const auto multiple_fields = cppmcp::parse_www_authenticate({
        "Bearer realm=\"one, two\", error=\"invalid_token\"",
        "Bearer scope=read",
    });
    CPPMCP_CHECK(multiple_fields.size() == 2);
    if (multiple_fields.size() == 2) {
        CPPMCP_CHECK(multiple_fields[0].parameters.at("realm") == "one, two");
        CPPMCP_CHECK(multiple_fields[0].error == "invalid_token");
        CPPMCP_CHECK(multiple_fields[1].scope == "read");
    }

    auto provider = cppmcp::StaticBearerProvider("test-token");
    cppmcp::transport::Headers headers;
    provider.apply(headers);
    CPPMCP_CHECK(headers.size() == 1);
    if (!headers.empty()) {
        CPPMCP_CHECK(headers[0].first == "Authorization");
        CPPMCP_CHECK(headers[0].second == "Bearer test-token");
    }
}
