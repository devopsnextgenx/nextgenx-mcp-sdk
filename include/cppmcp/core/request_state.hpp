#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace cppmcp::core {

struct RequestStateKey {
    std::string id;
    std::string secret_hex;
};

struct RequestStateBinding {
    std::string method;
    std::string name;
    nlohmann::json params = nlohmann::json::object();
    std::string subject = "anonymous";
    nlohmann::json private_payload = nlohmann::json::object();
    std::vector<std::string> input_request_ids;
    std::size_t round = 0;
};

struct RequestStateClaims {
    std::string method;
    std::string name;
    std::string subject;
    nlohmann::json private_payload = nlohmann::json::object();
    std::vector<std::string> input_request_ids;
    std::size_t round = 0;
    std::int64_t issued_at = 0;
    std::int64_t expires_at = 0;
};

class IRequestStateCodec {
public:
    virtual ~IRequestStateCodec() = default;
    virtual std::string issue(const RequestStateBinding& binding) const = 0;
    virtual RequestStateClaims verify(std::string_view token,
                                     const RequestStateBinding& expected) const = 0;
};

class HmacRequestStateCodec final : public IRequestStateCodec {
public:
    using Clock = std::function<std::chrono::system_clock::time_point()>;

    HmacRequestStateCodec(std::vector<RequestStateKey> keys,
                          std::chrono::seconds ttl = std::chrono::minutes(10),
                          std::size_t max_token_bytes = 8192, Clock clock = {});

    std::string issue(const RequestStateBinding& binding) const override;
    RequestStateClaims verify(std::string_view token,
                              const RequestStateBinding& expected) const override;

private:
    struct Key {
        std::string id;
        std::vector<unsigned char> secret;
    };

    std::vector<Key> keys_;
    std::chrono::seconds ttl_;
    std::size_t max_token_bytes_;
    Clock clock_;
};

}  // namespace cppmcp::core