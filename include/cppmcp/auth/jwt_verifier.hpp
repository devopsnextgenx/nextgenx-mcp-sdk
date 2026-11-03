#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "cppmcp/auth/auth_context.hpp"
#include "cppmcp/net/network_policy.hpp"

namespace cppmcp::auth {

struct JwtVerifierOptions {
    std::vector<std::string> issuers;
    std::vector<std::string> accepted_audiences;
    std::string jwks_uri;
    std::chrono::seconds clock_skew{60};
    std::chrono::seconds min_jwks_refresh{10};
    std::chrono::seconds stale_key_grace{300};
    std::size_t max_token_bytes = 16 * 1024;
    bool require_subject = true;
    bool require_access_token_type = false;
    std::vector<std::string> allowed_algorithms{"RS256", "PS256", "ES256", "EdDSA"};
    net::NetworkPolicy network_policy;
};

class JwtVerifier final : public ITokenVerifier {
public:
    explicit JwtVerifier(JwtVerifierOptions options);
    ~JwtVerifier() override;
    JwtVerifier(const JwtVerifier&) = delete;
    JwtVerifier& operator=(const JwtVerifier&) = delete;

    AuthContext verify(std::string_view token,
                       std::string_view resource) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cppmcp::auth
