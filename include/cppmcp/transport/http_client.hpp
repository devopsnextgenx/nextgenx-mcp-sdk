#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "cppmcp/core/transport.hpp"
#include "cppmcp/transport/http.hpp"

namespace cppmcp {

enum class NegotiationPolicy {
    prefer_modern,
    modern_only,
    legacy_only,
};

struct AuthChallenge {
    std::string scheme;
    std::map<std::string, std::string> parameters;
    std::string error;
    std::string scope;
    std::string resource_metadata;
    std::string error_description;
};

enum class AuthAction {
    retry,
    fail,
};

class IAuthProvider {
public:
    virtual ~IAuthProvider() = default;
    virtual void apply(transport::Headers& headers) = 0;
    virtual AuthAction on_challenge(const AuthChallenge& challenge) = 0;
};

class StaticBearerProvider final : public IAuthProvider {
public:
    explicit StaticBearerProvider(std::string token);
    void apply(transport::Headers& headers) override;
    AuthAction on_challenge(const AuthChallenge&) override;

private:
    std::string token_;
};

struct HttpClientOptions {
    std::string url;
    NegotiationPolicy policy = NegotiationPolicy::prefer_modern;
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds request_timeout{15000};
    std::size_t max_response_bytes = 16 * 1024 * 1024;
    std::string ca_file;
    std::shared_ptr<IAuthProvider> auth_provider;
};

class AuthRequired : public std::runtime_error {
public:
    explicit AuthRequired(AuthChallenge challenge);
    const AuthChallenge& challenge() const noexcept;

private:
    AuthChallenge challenge_;
};

class InsufficientScope : public std::runtime_error {
public:
    explicit InsufficientScope(AuthChallenge challenge);
    const AuthChallenge& challenge() const noexcept;

private:
    AuthChallenge challenge_;
};

[[nodiscard]] std::vector<AuthChallenge> parse_www_authenticate(
    const std::vector<std::string>& values);

}  // namespace cppmcp

namespace cppmcp::transport {

struct ListenCallbacks {
    std::function<void()> on_stream_lost;
    std::function<void()> on_reconnected;
    std::function<void(const core::Request&)> on_server_request;
};

struct ListenOptions {
    std::chrono::milliseconds backoff_base{200};
    std::chrono::milliseconds backoff_cap{5000};
    std::size_t max_attempts = 0;
    double jitter_fraction = 0.2;
};

class ListenHandle {
public:
    ~ListenHandle();
    ListenHandle(const ListenHandle&) = delete;
    ListenHandle& operator=(const ListenHandle&) = delete;
    void close();

private:
    struct Impl;
    explicit ListenHandle(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class HttpClientTransport;
};

class HttpClientTransport final : public core::ITransport {
public:
    explicit HttpClientTransport(HttpClientOptions options);
    ~HttpClientTransport() override;

    void start() override;
    void send(const core::Message& message) override;
    void close() override;
    core::CancellationStrategy cancellation_strategy() const noexcept override;
    void cancel_request(const core::RequestId& request_id) override;
    [[nodiscard]] bool is_modern() const noexcept;
    [[nodiscard]] std::shared_ptr<ListenHandle> listen(
        nlohmann::json filter,
        std::function<void(const core::Notification&)> callback,
        ListenCallbacks callbacks = {}, ListenOptions options = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cppmcp::transport
