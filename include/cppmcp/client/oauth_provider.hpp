#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "cppmcp/auth/scope_authorization.hpp"
#include "cppmcp/net/network_policy.hpp"
#include "cppmcp/transport/http_client.hpp"

namespace cppmcp::client {

struct OAuthClientRegistration {
    std::string client_id;
    std::string client_secret;
    std::string token_endpoint_auth_method = "none";
};

struct OAuthToken {
    std::string access_token;
    std::string token_type = "Bearer";
    std::string refresh_token;
    auth::ScopeSet scopes;
    std::chrono::system_clock::time_point expires_at{};
};

struct OAuthTokenKey {
    std::string issuer;
    std::string resource;
    std::string client_id;

    friend bool operator<(const OAuthTokenKey& left, const OAuthTokenKey& right) {
        return std::tie(left.issuer, left.resource, left.client_id) <
               std::tie(right.issuer, right.resource, right.client_id);
    }
};

class ITokenStore {
public:
    virtual ~ITokenStore() = default;
    [[nodiscard]] virtual std::optional<OAuthToken> get(const OAuthTokenKey& key) const = 0;
    virtual void put(const OAuthTokenKey& key, const OAuthToken& token) = 0;
    virtual void erase(const OAuthTokenKey& key) = 0;
    [[nodiscard]] virtual std::optional<OAuthClientRegistration> get_registration(
        std::string_view issuer) const = 0;
    virtual void put_registration(std::string issuer,
                                  OAuthClientRegistration registration) = 0;
};

class MemoryTokenStore final : public ITokenStore {
public:
    [[nodiscard]] std::optional<OAuthToken> get(const OAuthTokenKey& key) const override;
    void put(const OAuthTokenKey& key, const OAuthToken& token) override;
    void erase(const OAuthTokenKey& key) override;
    [[nodiscard]] std::optional<OAuthClientRegistration> get_registration(
        std::string_view issuer) const override;
    void put_registration(std::string issuer,
                          OAuthClientRegistration registration) override;

private:
    mutable std::mutex mutex_;
    std::map<OAuthTokenKey, OAuthToken> tokens_;
    std::map<std::string, OAuthClientRegistration> registrations_;
};

class FileTokenStore final : public ITokenStore {
public:
    explicit FileTokenStore(std::string path, std::string encryption_key_env = {});
    [[nodiscard]] std::optional<OAuthToken> get(const OAuthTokenKey& key) const override;
    void put(const OAuthTokenKey& key, const OAuthToken& token) override;
    void erase(const OAuthTokenKey& key) override;
    [[nodiscard]] std::optional<OAuthClientRegistration> get_registration(
        std::string_view issuer) const override;
    void put_registration(std::string issuer,
                          OAuthClientRegistration registration) override;

private:
    void load();
    void save_locked() const;

    std::string path_;
    std::string encryption_key_env_;
    mutable std::mutex mutex_;
    std::map<OAuthTokenKey, OAuthToken> tokens_;
    std::map<std::string, OAuthClientRegistration> registrations_;
};

class ClientRegistrationStore {
public:
    void put(std::string issuer, OAuthClientRegistration registration);
    [[nodiscard]] std::optional<OAuthClientRegistration> get(
        std::string_view issuer) const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, OAuthClientRegistration> entries_;
};

class OAuthError : public std::runtime_error {
public:
    OAuthError(std::string reason, std::string message);
    [[nodiscard]] const std::string& reason() const noexcept;

private:
    std::string reason_;
};

#define CPPMCP_OAUTH_ERROR(name, reason_text)                    \
    class name final : public OAuthError {                       \
    public:                                                       \
        explicit name(std::string message = reason_text)         \
            : OAuthError(reason_text, std::move(message)) {}      \
    }

CPPMCP_OAUTH_ERROR(PkceUnsupported, "pkce_unsupported");
CPPMCP_OAUTH_ERROR(IssuerMismatch, "issuer_mismatch");
CPPMCP_OAUTH_ERROR(IssMismatch, "iss_mismatch");
CPPMCP_OAUTH_ERROR(IssMissing, "iss_missing");
CPPMCP_OAUTH_ERROR(StateMismatch, "state_mismatch");
CPPMCP_OAUTH_ERROR(ResourceMismatch, "resource_mismatch");
CPPMCP_OAUTH_ERROR(NoRegistrationMethod, "no_registration");
CPPMCP_OAUTH_ERROR(StepUpLimitExceeded, "stepup_limit");
CPPMCP_OAUTH_ERROR(PrmMissing, "prm_missing");
CPPMCP_OAUTH_ERROR(UserCancelled, "user_cancelled");

#undef CPPMCP_OAUTH_ERROR

class IClientInfoPrompt {
public:
    virtual ~IClientInfoPrompt() = default;
    virtual OAuthClientRegistration prompt(std::string_view issuer) = 0;
};

class IUserAgentLauncher {
public:
    virtual ~IUserAgentLauncher() = default;
    virtual void launch(std::string_view authorization_url) = 0;
};

struct OAuthProviderOptions {
    std::string resource_url;
    std::string client_id;
    std::string client_secret;
    std::string client_secret_auth_method;
    std::string cimd_url;
    std::string client_name = "cppmcp-client";
    std::string grant_type = "authorization_code";
    std::string launcher = "xdg-open";
    unsigned short redirect_port = 0;
    bool enable_dcr = false;
    bool allow_insecure_loopback = false;
    std::size_t step_up_limit = 3;
    auth::ScopeSet requested_scopes;
    net::NetworkPolicy network_policy;
    std::shared_ptr<ITokenStore> token_store;
    std::shared_ptr<ClientRegistrationStore> registrations;
    std::shared_ptr<IClientInfoPrompt> registration_prompt;
    std::shared_ptr<IUserAgentLauncher> user_agent_launcher;
    std::function<void(std::string_view)> event_handler;
};

class OAuthProvider final : public IAuthProvider {
public:
    explicit OAuthProvider(OAuthProviderOptions options);
    ~OAuthProvider() override;
    OAuthProvider(const OAuthProvider&) = delete;
    OAuthProvider& operator=(const OAuthProvider&) = delete;

    void apply(transport::Headers& headers) override;
    AuthAction on_challenge(const AuthChallenge& challenge) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cppmcp::client
