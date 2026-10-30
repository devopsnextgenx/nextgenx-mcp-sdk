#include "cppmcp/core/request_state.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

namespace cppmcp::core {
namespace {

int hex_digit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::vector<unsigned char> decode_key(std::string_view hex) {
    if (hex.size() != 64) {
        throw std::invalid_argument("request-state keys must be 64 hexadecimal characters");
    }
    std::vector<unsigned char> bytes;
    bytes.reserve(32);
    for (std::size_t index = 0; index < hex.size(); index += 2) {
        const auto high = hex_digit(hex[index]);
        const auto low = hex_digit(hex[index + 1]);
        if (high < 0 || low < 0) {
            throw std::invalid_argument("request-state keys must be 64 hexadecimal characters");
        }
        bytes.push_back(static_cast<unsigned char>((high << 4) | low));
    }
    return bytes;
}

std::string encode_hex(const unsigned char* data, std::size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string encoded(size * 2, '\0');
    for (std::size_t index = 0; index < size; ++index) {
        encoded[index * 2] = digits[data[index] >> 4];
        encoded[index * 2 + 1] = digits[data[index] & 15];
    }
    return encoded;
}

std::vector<unsigned char> decode_hex(std::string_view hex) {
    if (hex.size() % 2 != 0) {
        throw std::invalid_argument("invalid requestState");
    }
    std::vector<unsigned char> decoded;
    decoded.reserve(hex.size() / 2);
    for (std::size_t index = 0; index < hex.size(); index += 2) {
        const auto high = hex_digit(hex[index]);
        const auto low = hex_digit(hex[index + 1]);
        if (high < 0 || low < 0) {
            throw std::invalid_argument("invalid requestState");
        }
        decoded.push_back(static_cast<unsigned char>((high << 4) | low));
    }
    return decoded;
}

std::string sha256(std::string_view input) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (EVP_Digest(input.data(), input.size(), digest.data(), &length,
                   EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("request-state digest failed");
    }
    return encode_hex(digest.data(), length);
}

std::string digest_params(const nlohmann::json& params) {
    auto canonical = params.is_object() ? params : nlohmann::json::object();
    canonical.erase("inputResponses");
    canonical.erase("requestState");
    return sha256(canonical.dump());
}

std::string mac_hex(const std::vector<unsigned char>& secret, std::string_view input) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
             reinterpret_cast<const unsigned char*>(input.data()), input.size(),
             digest.data(), &length) == nullptr) {
        throw std::runtime_error("request-state authentication failed");
    }
    return encode_hex(digest.data(), length);
}

std::int64_t epoch_seconds(std::chrono::system_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
}

std::vector<std::string> string_array(const nlohmann::json& value) {
    if (!value.is_array()) {
        throw std::invalid_argument("invalid requestState");
    }
    std::vector<std::string> output;
    output.reserve(value.size());
    for (const auto& item : value) {
        if (!item.is_string()) {
            throw std::invalid_argument("invalid requestState");
        }
        output.push_back(item.get<std::string>());
    }
    return output;
}

}  // namespace

HmacRequestStateCodec::HmacRequestStateCodec(std::vector<RequestStateKey> keys,
                                             std::chrono::seconds ttl,
                                             std::size_t max_token_bytes,
                                             Clock clock)
    : ttl_(ttl), max_token_bytes_(max_token_bytes), clock_(std::move(clock)) {
    if (ttl_.count() <= 0 || max_token_bytes_ < 256) {
        throw std::invalid_argument("invalid request-state TTL or size limit");
    }
    if (keys.empty()) {
        std::array<unsigned char, 32> random_key{};
        if (RAND_bytes(random_key.data(), static_cast<int>(random_key.size())) != 1) {
            throw std::runtime_error("unable to generate request-state key");
        }
        keys.push_back({"process-1", encode_hex(random_key.data(), random_key.size())});
    }
    std::unordered_set<std::string> ids;
    keys_.reserve(keys.size());
    for (auto& key : keys) {
        if (key.id.empty() || key.id.size() > 64 ||
            !std::all_of(key.id.begin(), key.id.end(), [](unsigned char ch) {
                return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                       (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
            }) || !ids.insert(key.id).second) {
            throw std::invalid_argument("request-state key IDs must be non-empty and unique");
        }
        keys_.push_back({std::move(key.id), decode_key(key.secret_hex)});
    }
    if (!clock_) {
        clock_ = [] { return std::chrono::system_clock::now(); };
    }
}

std::string HmacRequestStateCodec::issue(const RequestStateBinding& binding) const {
    const auto now = epoch_seconds(clock_());
    if (ttl_.count() > std::numeric_limits<std::int64_t>::max() - now) {
        throw std::invalid_argument("invalid requestState expiry");
    }
    nlohmann::json payload = {
        {"version", 1},
        {"method", binding.method},
        {"name", binding.name},
        {"params", digest_params(binding.params)},
        {"subject", binding.subject},
        {"issuedAt", now},
        {"expiresAt", now + ttl_.count()},
        {"round", binding.round},
        {"privatePayload", binding.private_payload},
        {"inputRequestIds", binding.input_request_ids},
    };
    const auto payload_text = payload.dump();
    const auto payload_hex = encode_hex(
        reinterpret_cast<const unsigned char*>(payload_text.data()), payload_text.size());
    const auto& key = keys_.back();
    const auto signed_text = "v1." + key.id + "." + payload_hex;
    const auto token = signed_text + "." + mac_hex(key.secret, signed_text);
    if (token.size() > max_token_bytes_) {
        throw std::invalid_argument("requestState exceeds the configured size limit");
    }
    return token;
}

RequestStateClaims HmacRequestStateCodec::verify(
    std::string_view token, const RequestStateBinding& expected) const {
    if (token.empty() || token.size() > max_token_bytes_) {
        throw std::invalid_argument("invalid requestState");
    }
    const auto version_separator = token.find('.');
    const auto key_separator = token.find('.', version_separator + 1);
    const auto mac_separator = token.find('.', key_separator + 1);
    if (version_separator == std::string_view::npos ||
        key_separator == std::string_view::npos || mac_separator == std::string_view::npos ||
        token.find('.', mac_separator + 1) != std::string_view::npos ||
        token.substr(0, version_separator) != "v1") {
        throw std::invalid_argument("invalid requestState");
    }
    const auto key_id = token.substr(version_separator + 1,
                                     key_separator - version_separator - 1);
    const auto key = std::find_if(keys_.begin(), keys_.end(), [&](const auto& item) {
        return item.id == key_id;
    });
    if (key == keys_.end()) {
        throw std::invalid_argument("invalid requestState");
    }
    const auto signed_text = token.substr(0, mac_separator);
    const auto expected_mac = mac_hex(key->secret, signed_text);
    const auto received_mac = token.substr(mac_separator + 1);
    if (received_mac.size() != expected_mac.size() ||
        CRYPTO_memcmp(received_mac.data(), expected_mac.data(), expected_mac.size()) != 0) {
        throw std::invalid_argument("invalid requestState");
    }

    try {
        const auto payload_bytes = decode_hex(token.substr(key_separator + 1,
                                                            mac_separator - key_separator - 1));
        const auto payload = nlohmann::json::parse(payload_bytes.begin(), payload_bytes.end());
        if (payload.at("version") != 1 ||
            payload.at("method") != expected.method ||
            payload.at("name") != expected.name ||
            payload.at("params") != digest_params(expected.params) ||
            payload.at("subject") != expected.subject) {
            throw std::invalid_argument("invalid requestState");
        }
        RequestStateClaims claims{
            .method = payload.at("method").get<std::string>(),
            .name = payload.at("name").get<std::string>(),
            .subject = payload.at("subject").get<std::string>(),
            .private_payload = payload.at("privatePayload"),
            .input_request_ids = string_array(payload.at("inputRequestIds")),
            .round = payload.at("round").get<std::size_t>(),
            .issued_at = payload.at("issuedAt").get<std::int64_t>(),
            .expires_at = payload.at("expiresAt").get<std::int64_t>(),
        };
        const auto now = epoch_seconds(clock_());
        if (claims.issued_at > now || claims.expires_at <= now) {
            throw std::invalid_argument("invalid requestState");
        }
        return claims;
    } catch (const std::invalid_argument&) {
        throw;
    } catch (const std::exception&) {
        throw std::invalid_argument("invalid requestState");
    }
}

}  // namespace cppmcp::core