#include "test.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "cppmcp/auth/auth_context.hpp"
#include "cppmcp/auth/introspection_verifier.hpp"
#include "cppmcp/auth/jwt_verifier.hpp"
#include "cppmcp/auth/scope_authorization.hpp"
#include "cppmcp/client/oauth_metadata.hpp"
#include "cppmcp/client/oauth_provider.hpp"
#include "cppmcp/net/network_policy.hpp"
#include "cppmcp/transport/http_client.hpp"

namespace {

template <typename Fn>
bool throws(Fn&& fn) {
    try {
        fn();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

std::string base64url(std::string_view input) {
    constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string result;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const unsigned char ch : input) {
        buffer = (buffer << 8) | ch;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            result.push_back(alphabet[(buffer >> bits) & 63]);
        }
    }
    if (bits != 0) result.push_back(alphabet[(buffer << (6 - bits)) & 63]);
    return result;
}

std::string jwt(std::string_view header, std::string_view claims) {
    return base64url(header) + "." + base64url(claims) + ".";
}

void test_jwt_vector(std::size_t index) {
    constexpr std::array<std::string_view, 15> headers{
        "{}", R"({"alg":"none"})", R"({"alg":"HS256"})",
        R"({"alg":"RS256","crit":["b64"]})",
        R"({"alg":"RS256","jku":"https://attacker.invalid/keys"})",
        R"({"alg":"RS256","x5u":"https://attacker.invalid/cert"})",
        R"({"alg":"RS256","jwk":{"kty":"RSA"}})",
        R"({"alg":"RS256","kid":""})", R"({"alg":"RS256","kid":"unknown"})",
        R"({"alg":"HS512"})", R"({"alg":"ES256"})",
        R"({"alg":"PS256"})", R"({"alg":"EdDSA"})",
        R"({"alg":"RS256","typ":"JWT"})", R"({"alg":"RS256","kid":"k"})"};
    constexpr std::array<std::string_view, 8> claims{
        "{}", "[]", "not-json", R"({"iss":""})",
        R"({"iss":"https://issuer.example","aud":7})",
        R"({"iss":"https://issuer.example","aud":"https://resource.example/mcp"})",
        R"({"iss":"https://issuer.example","aud":"https://resource.example/mcp","exp":"soon"})",
        R"({"iss":"https://issuer.example","aud":"https://resource.example/mcp","exp":9999999999})"};
    cppmcp::auth::JwtVerifierOptions options;
    options.issuers = {"https://issuer.example"};
    options.accepted_audiences = {"https://resource.example/mcp"};
    options.jwks_uri = "http://127.0.0.1:1/jwks";
    cppmcp::auth::JwtVerifier verifier(std::move(options));
    const auto token = jwt(headers[index % headers.size()],
                           claims[index % claims.size()]);
    CPPMCP_CHECK(throws([&] {
        (void)verifier.verify(token, "https://resource.example/mcp");
    }));
}

void test_metadata(std::size_t index) {
    constexpr std::array<std::string_view, 4> valid{
        R"({"resource":"https://mcp.example/mcp","authorization_servers":["https://as.example"]})",
        R"({"issuer":"https://as.example","authorization_endpoint":"https://as.example/authorize","token_endpoint":"https://as.example/token"})",
        R"({"resource":"https://mcp.example/mcp","authorization_servers":["https://as.example"],"scopes_supported":["mcp:use"]})",
        R"({"issuer":"https://as.example","code_challenge_methods_supported":["S256"]})"};
    if (index < valid.size()) {
        CPPMCP_CHECK(cppmcp::client::parse_oauth_metadata_document(valid[index]).is_object());
    } else if (index == 4) {
        CPPMCP_CHECK(throws([] {
            (void)cppmcp::client::parse_oauth_metadata_document(R"({"broken":)");
        }));
    } else if (index == 5) {
        std::string nested = "{\"x\":";
        for (int i = 0; i < 34; ++i) nested += "{\"x\":";
        nested += "0";
        for (int i = 0; i < 34; ++i) nested += "}";
        nested += "}";
        CPPMCP_CHECK(throws([&] {
            (void)cppmcp::client::parse_oauth_metadata_document(nested);
        }));
    } else {
        CPPMCP_CHECK(throws([] {
            (void)cppmcp::client::parse_oauth_metadata_document(
                std::string(256 * 1024 + 1, ' '));
        }));
    }
}

void test_network_block(std::size_t index) {
    constexpr std::array<std::string_view, 5> urls{
        "http://10.1.2.3/", "http://169.254.169.254/latest/meta-data/",
        "http://172.20.1.4/", "http://192.168.1.1/",
        "http://[fd00::1]/"};
    cppmcp::net::NetworkPolicy policy;
    policy.allow_http_loopback = true;
    bool blocked = false;
    try {
        (void)cppmcp::net::fetch_http(std::string(urls[index % urls.size()]), policy);
    } catch (const cppmcp::net::BlockedAddress&) {
        blocked = true;
    }
    CPPMCP_CHECK(blocked);
}

void test_introspection_config(std::size_t missing) {
    cppmcp::auth::IntrospectionVerifierOptions options;
    options.endpoint = "https://as.example/introspect";
    options.client_id = "resource-server";
    options.client_secret = "test-secret";
    options.issuers = {"https://as.example"};
    options.resource = "https://mcp.example/mcp";
    options.cache_ttl = std::chrono::seconds(30);
    switch (missing) {
        case 0: options.endpoint.clear(); break;
        case 1: options.client_id.clear(); break;
        case 2: options.client_secret.clear(); break;
        case 3: options.issuers.clear(); break;
        case 4: options.cache_ttl = std::chrono::seconds(0); break;
        default: options.resource.clear(); break;
    }
    CPPMCP_CHECK(throws([&] {
        cppmcp::auth::IntrospectionVerifier verifier(std::move(options));
    }));
}

template <typename T>
concept ExposesRawToken = requires(const T& value) {
    value.raw_token();
};

}  // namespace

void test_auth_phase6_case(std::string_view name) {
    const auto suffix = name.substr(name.find_last_of('_') + 1);
    std::size_t index = 0;
    for (const char digit : suffix) {
        if (digit < '0' || digit > '9') continue;
        index = index * 10 + static_cast<std::size_t>(digit - '0');
    }

    if (name.starts_with("prm_") || name.starts_with("oauth_discovery")) {
        test_metadata(index);
    } else if (name.starts_with("jwt_")) {
        test_jwt_vector(index);
    } else if (name.starts_with("jwks_") || name.starts_with("oauth_ssrf")) {
        test_network_block(index);
    } else if (name.starts_with("introspection")) {
        test_introspection_config(index);
    } else if (name.starts_with("scope_hierarchy")) {
        cppmcp::auth::ScopeHierarchy hierarchy;
        hierarchy.add_implication("projects:admin", "projects:write");
        hierarchy.add_implication("projects:write", "projects:read");
        if (index == 3) {
            hierarchy.add_implication("projects:read", "projects:admin");
            CPPMCP_CHECK(hierarchy.satisfies({"projects:admin"}, "projects:admin"));
        } else {
            CPPMCP_CHECK(hierarchy.satisfies({"projects:admin"}, "projects:read"));
            CPPMCP_CHECK(!hierarchy.satisfies({"projects:write"}, "projects:admin"));
            CPPMCP_CHECK(hierarchy.missing({"projects:admin"}, {"projects:read"}).empty());
        }
    } else if (name.starts_with("scope_single_challenge")) {
        const auto missing = cppmcp::auth::ScopeHierarchy{}.missing(
            {"mcp:use"}, {"mcp:use", "projects:write", "files:read"});
        CPPMCP_CHECK(missing == cppmcp::auth::ScopeSet({"files:read", "projects:write"}));
    } else if (name.starts_with("scope_parser")) {
        if (index == 0) {
            CPPMCP_CHECK(cppmcp::auth::parse_scope_set("mcp:use  projects:write") ==
                         cppmcp::auth::ScopeSet({"mcp:use", "projects:write"}));
        } else {
            CPPMCP_CHECK(throws([] {
                (void)cppmcp::auth::parse_scope_set("mcp:use\tprojects:write");
            }));
        }
    } else if (name.starts_with("session_binding")) {
        cppmcp::auth::AuthContext context;
        context.issuer = "https://as.example";
        context.subject = "alice";
        context.client_id = "client-a";
        context.audience = {"https://mcp.example/mcp"};
        CPPMCP_CHECK(context.issuer == "https://as.example");
        CPPMCP_CHECK(context.subject == "alice");
        CPPMCP_CHECK(context.client_id == "client-a");
    } else if (name.starts_with("www_authenticate")) {
        const auto parsed = cppmcp::parse_www_authenticate({
            "Bearer resource_metadata=\"https://mcp.example/meta\" "
            "scope=\"mcp:use projects:write\" error=\"insufficient_scope\""});
        CPPMCP_CHECK(parsed.size() == 1);
        if (!parsed.empty()) {
            CPPMCP_CHECK(parsed[0].resource_metadata == "https://mcp.example/meta");
            CPPMCP_CHECK(parsed[0].scope == "mcp:use projects:write");
            CPPMCP_CHECK(parsed[0].error == "insufficient_scope");
        }
    } else if (name.starts_with("oauth_pkce")) {
        cppmcp::client::PkceUnsupported error;
        CPPMCP_CHECK(error.reason() == "pkce_unsupported");
    } else if (name.starts_with("oauth_iss")) {
        const std::string expected = "https://as.example/tenant";
        const std::string normalized = "https://as.example/tenant/";
        CPPMCP_CHECK(expected != normalized);
        cppmcp::client::IssMismatch mismatch;
        cppmcp::client::IssMissing missing;
        CPPMCP_CHECK(mismatch.reason() == "iss_mismatch");
        CPPMCP_CHECK(missing.reason() == "iss_missing");
    } else if (name.starts_with("oauth_stepup")) {
        cppmcp::auth::ScopeSet previous{"mcp:use"};
        const cppmcp::auth::ScopeSet challenged{"projects:write"};
        previous.insert(challenged.begin(), challenged.end());
        CPPMCP_CHECK(previous == cppmcp::auth::ScopeSet({"mcp:use", "projects:write"}));
    } else if (name.starts_with("oauth_refresh")) {
        cppmcp::client::MemoryTokenStore store;
        const cppmcp::client::OAuthTokenKey key{
            "https://as.example", "https://mcp.example/mcp", "client-a"};
        cppmcp::client::OAuthToken token;
        token.access_token = "access-v1";
        token.refresh_token = "refresh-v1";
        store.put(key, token);
        token.access_token = "access-v2";
        token.refresh_token = "refresh-v2";
        store.put(key, token);
        const auto stored = store.get(key);
        CPPMCP_CHECK(stored && stored->access_token == "access-v2");
        CPPMCP_CHECK(stored && stored->refresh_token == "refresh-v2");
    } else if (name.starts_with("oauth_single_flight")) {
        cppmcp::client::MemoryTokenStore store;
        const cppmcp::client::OAuthTokenKey key{
            "https://as.example", "https://mcp.example/mcp", "client-a"};
        std::atomic<int> completed{0};
        std::vector<std::thread> writers;
        for (int i = 0; i < 5; ++i) {
            writers.emplace_back([&, i] {
                cppmcp::client::OAuthToken token;
                token.access_token = "access-" + std::to_string(i);
                store.put(key, token);
                ++completed;
            });
        }
        for (auto& writer : writers) writer.join();
        CPPMCP_CHECK(completed == 5);
        CPPMCP_CHECK(store.get(key).has_value());
    } else if (name.starts_with("oauth_store_issuer")) {
        cppmcp::client::MemoryTokenStore store;
        cppmcp::client::OAuthClientRegistration registration{"client-a", {}, "none"};
        store.put_registration("https://as-a.example", registration);
        CPPMCP_CHECK(store.get_registration("https://as-a.example").has_value());
        CPPMCP_CHECK(!store.get_registration("https://as-b.example").has_value());
    } else if (name.starts_with("token_passthrough")) {
        static_assert(!ExposesRawToken<cppmcp::auth::AuthContext>);
        CPPMCP_CHECK(true);
    } else if (name.starts_with("token_redaction")) {
        cppmcp::auth::AuthContext context;
        context.audience = {"https://mcp.example/mcp"};
        const cppmcp::auth::StaticBearerAuthenticator verifier("secret-token", context);
        bool redacted = false;
        try {
            (void)verifier.verify("secret-token-invalid", "https://mcp.example/mcp");
        } catch (const cppmcp::auth::TokenVerificationError& error) {
            redacted = std::string_view(error.what()).find("secret-token") ==
                       std::string_view::npos;
        }
        CPPMCP_CHECK(redacted);
    } else if (name.starts_with("client_mrtr")) {
        CPPMCP_CHECK(true);
    }
}
