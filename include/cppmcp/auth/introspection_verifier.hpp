#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "cppmcp/auth/auth_context.hpp"
#include "cppmcp/net/network_policy.hpp"

namespace cppmcp::auth {

struct IntrospectionVerifierOptions {
    std::string endpoint;
    std::string client_id;
    std::string client_secret;
    std::vector<std::string> issuers;
    std::string resource;
    std::chrono::seconds cache_ttl{30};
    net::NetworkPolicy network_policy;
};

class IntrospectionVerifier final : public ITokenVerifier {
public:
    explicit IntrospectionVerifier(IntrospectionVerifierOptions options);
    ~IntrospectionVerifier() override;
    IntrospectionVerifier(const IntrospectionVerifier&) = delete;
    IntrospectionVerifier& operator=(const IntrospectionVerifier&) = delete;

    AuthContext verify(std::string_view token,
                       std::string_view resource) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cppmcp::auth
