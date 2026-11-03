#pragma once

#include <chrono>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace cppmcp::auth {

struct AuthContext {
    std::string issuer;
    std::string subject;
    std::string client_id;
    std::set<std::string> scopes;
    std::vector<std::string> audience;
    std::chrono::system_clock::time_point expires_at{};
    std::chrono::system_clock::time_point issued_at{};
    std::string token_id;
    nlohmann::json claims = nlohmann::json::object();
};

class TokenVerificationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ITokenVerifier {
public:
    virtual ~ITokenVerifier() = default;
    virtual AuthContext verify(std::string_view token,
                               std::string_view resource) const = 0;
};

class IRateLimiter {
public:
    virtual ~IRateLimiter() = default;
    [[nodiscard]] virtual bool allow(std::string_view peer) const = 0;
};

struct ExchangedToken {
    std::string access_token;
    std::string token_type = "Bearer";
    std::string resource;
    std::set<std::string> scopes;
    std::chrono::system_clock::time_point expires_at{};
};

class ITokenExchanger {
public:
    virtual ~ITokenExchanger() = default;
    virtual ExchangedToken exchange(const AuthContext& subject,
                                    std::string_view resource,
                                    const std::set<std::string>& scopes) const = 0;
};

class StaticBearerAuthenticator final : public ITokenVerifier {
public:
    StaticBearerAuthenticator(std::string token, AuthContext context);
    AuthContext verify(std::string_view token,
                       std::string_view resource) const override;

private:
    std::string token_;
    AuthContext context_;
};

}  // namespace cppmcp::auth
