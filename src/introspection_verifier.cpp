#include "cppmcp/auth/introspection_verifier.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <sstream>

#include <openssl/crypto.h>
#include <openssl/evp.h>

namespace cppmcp::auth {
namespace {

using Json = nlohmann::json;
using Clock = std::chrono::system_clock;

std::string base64_encode(std::string_view input) {
    constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((input.size() + 2) / 3 * 4);
    for (std::size_t offset = 0; offset < input.size(); offset += 3) {
        const auto a = static_cast<unsigned char>(input[offset]);
        const auto b = offset + 1 < input.size()
                           ? static_cast<unsigned char>(input[offset + 1]) : 0;
        const auto c = offset + 2 < input.size()
                           ? static_cast<unsigned char>(input[offset + 2]) : 0;
        const auto block = (static_cast<unsigned>(a) << 16) |
                           (static_cast<unsigned>(b) << 8) | c;
        result.push_back(alphabet[(block >> 18) & 63]);
        result.push_back(alphabet[(block >> 12) & 63]);
        result.push_back(offset + 1 < input.size() ? alphabet[(block >> 6) & 63] : '=');
        result.push_back(offset + 2 < input.size() ? alphabet[block & 63] : '=');
    }
    return result;
}

std::string form_encode(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
            ch == '.' || ch == '~') {
            result.push_back(static_cast<char>(ch));
        } else {
            result.push_back('%');
            result.push_back(hex[ch >> 4]);
            result.push_back(hex[ch & 15]);
        }
    }
    return result;
}

std::string token_cache_key(std::string_view token) {
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned digest_size = 0;
    if (EVP_Digest(token.data(), token.size(), digest, &digest_size,
                   EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("token cache key generation failed");
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest_size * 2);
    for (unsigned i = 0; i < digest_size; ++i) {
        result.push_back(hex[digest[i] >> 4]);
        result.push_back(hex[digest[i] & 15]);
    }
    return result;
}

std::int64_t integer_claim(const Json& value, const char* name) {
    const auto found = value.find(name);
    if (found == value.end() || !found->is_number_integer()) {
        throw std::runtime_error(std::string("malformed introspection claim: ") + name);
    }
    try {
        return found->get<std::int64_t>();
    } catch (const Json::exception&) {
        throw std::runtime_error(std::string("malformed introspection claim: ") + name);
    }
}

std::string string_claim(const Json& value, const char* name) {
    const auto found = value.find(name);
    if (found == value.end() || !found->is_string()) {
        throw std::runtime_error(std::string("malformed introspection claim: ") + name);
    }
    return found->get<std::string>();
}

std::vector<std::string> audiences(const Json& value) {
    const auto found = value.find("aud");
    if (found == value.end()) throw std::runtime_error("introspection response has no audience");
    if (found->is_string()) return {found->get<std::string>()};
    if (!found->is_array()) throw std::runtime_error("introspection audience is malformed");
    std::vector<std::string> result;
    for (const auto& item : *found) {
        if (!item.is_string()) throw std::runtime_error("introspection audience is malformed");
        result.push_back(item.get<std::string>());
    }
    return result;
}

std::set<std::string> scopes(const Json& value) {
    std::set<std::string> result;
    if (const auto scope = value.find("scope"); scope != value.end()) {
        if (!scope->is_string()) throw std::runtime_error("introspection scope is malformed");
        std::istringstream input(scope->get<std::string>());
        for (std::string item; input >> item;) result.insert(std::move(item));
    }
    for (const auto* key : {"scp", "scopes"}) {
        if (const auto found = value.find(key); found != value.end()) {
            if (!found->is_array()) throw std::runtime_error("introspection scopes are malformed");
            for (const auto& item : *found) {
                if (!item.is_string()) throw std::runtime_error("introspection scopes are malformed");
                result.insert(item.get<std::string>());
            }
        }
    }
    return result;
}

}  // namespace

struct IntrospectionVerifier::Impl {
    struct Cached {
        AuthContext context;
        Clock::time_point expires_at;
    };

    explicit Impl(IntrospectionVerifierOptions value) : options(std::move(value)) {
        if (options.endpoint.empty() || options.client_id.empty() ||
            options.client_secret.empty() || options.resource.empty() ||
            options.issuers.empty() || options.cache_ttl.count() <= 0) {
            throw std::invalid_argument("invalid introspection verifier configuration");
        }
    }

    IntrospectionVerifierOptions options;
    mutable std::mutex mutex;
    mutable std::map<std::string, Cached> cache;
};

IntrospectionVerifier::IntrospectionVerifier(IntrospectionVerifierOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

IntrospectionVerifier::~IntrospectionVerifier() {
    if (impl_ && !impl_->options.client_secret.empty()) {
        OPENSSL_cleanse(impl_->options.client_secret.data(),
                        impl_->options.client_secret.size());
    }
}

AuthContext IntrospectionVerifier::verify(std::string_view token,
                                          std::string_view resource) const {
    if (token.empty() || token.size() > 16 * 1024) {
        throw TokenVerificationError("opaque token size is invalid");
    }
    if (resource != impl_->options.resource) {
        throw TokenVerificationError("token audience mismatch");
    }
    const auto cache_key = token_cache_key(token);
    const auto now = Clock::now();
    {
        std::lock_guard lock(impl_->mutex);
        const auto found = impl_->cache.find(cache_key);
        if (found != impl_->cache.end() && now < found->second.expires_at) {
            return found->second.context;
        }
    }

    const auto basic = base64_encode(impl_->options.client_id + ":" +
                                     impl_->options.client_secret);
    const auto body = "token=" + form_encode(token);
    auto response = net::fetch_http(
        impl_->options.endpoint, impl_->options.network_policy, "POST",
        {{"Authorization", "Basic " + basic},
         {"Content-Type", "application/x-www-form-urlencoded"},
         {"Accept", "application/json"}},
        body);
    if (response.status < 200 || response.status >= 300) {
        throw std::runtime_error("introspection endpoint returned a non-success status");
    }
    Json claims;
    try {
        claims = Json::parse(response.body);
    } catch (const Json::exception&) {
        throw std::runtime_error("introspection endpoint returned malformed JSON");
    }
    if (!claims.is_object() || !claims.contains("active") ||
        !claims["active"].is_boolean()) {
        throw std::runtime_error("introspection response is malformed");
    }
    if (!claims["active"].get<bool>()) {
        throw TokenVerificationError("inactive token");
    }

    AuthContext context;
    context.issuer = string_claim(claims, "iss");
    if (std::find(impl_->options.issuers.begin(), impl_->options.issuers.end(),
                  context.issuer) == impl_->options.issuers.end()) {
        throw TokenVerificationError("token issuer mismatch");
    }
    context.subject = string_claim(claims, "sub");
    context.client_id = claims.value("client_id", std::string{});
    if (context.client_id.empty()) context.client_id = claims.value("azp", std::string{});
    if (context.client_id.empty()) throw std::runtime_error("introspection client id is missing");
    context.audience = audiences(claims);
    if (std::find(context.audience.begin(), context.audience.end(),
                  impl_->options.resource) == context.audience.end()) {
        throw TokenVerificationError("token audience mismatch");
    }
    const auto expires = integer_claim(claims, "exp");
    const auto epoch_now = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    if (expires <= epoch_now) throw TokenVerificationError("token has expired");
    context.expires_at = Clock::time_point(std::chrono::seconds(expires));
    if (const auto issued = claims.find("iat"); issued != claims.end()) {
        if (!issued->is_number_integer()) throw std::runtime_error("introspection iat is malformed");
        context.issued_at = Clock::time_point(
            std::chrono::seconds(issued->get<std::int64_t>()));
    }
    if (const auto token_id = claims.find("jti");
        token_id != claims.end() && token_id->is_string()) {
        context.token_id = token_id->get<std::string>();
    }
    context.scopes = scopes(claims);
    context.claims = claims;
    const auto cache_expiry = std::min(
        context.expires_at, now + impl_->options.cache_ttl);
    {
        std::lock_guard lock(impl_->mutex);
        impl_->cache.insert_or_assign(cache_key, Impl::Cached{context, cache_expiry});
    }
    return context;
}

}  // namespace cppmcp::auth
