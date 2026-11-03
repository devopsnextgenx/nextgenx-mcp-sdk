#include "cppmcp/auth/jwt_verifier.hpp"
#include "cppmcp/auth/scope_authorization.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <set>
#include <sstream>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/params.h>
#include <openssl/rsa.h>

namespace cppmcp::auth {
namespace {

using Json = nlohmann::json;
using Clock = std::chrono::system_clock;

struct ParsedUrl {
    std::string origin;
    std::string path;
};

ParsedUrl split_issuer(const std::string& issuer) {
    const auto scheme_end = issuer.find("://");
    if (scheme_end == std::string::npos) throw TokenVerificationError("invalid configured issuer");
    const auto authority_start = scheme_end + 3;
    const auto path_start = issuer.find('/', authority_start);
    if (path_start == std::string::npos) return {issuer, {}};
    return {issuer.substr(0, path_start), issuer.substr(path_start)};
}

std::vector<std::string> discovery_urls(const std::string& issuer) {
    const auto parsed = split_issuer(issuer);
    if (parsed.path.empty() || parsed.path == "/") {
        return {parsed.origin + "/.well-known/oauth-authorization-server",
                parsed.origin + "/.well-known/openid-configuration"};
    }
    auto suffix = parsed.path;
    while (!suffix.empty() && suffix.front() == '/') suffix.erase(suffix.begin());
    return {parsed.origin + "/.well-known/oauth-authorization-server/" + suffix,
            parsed.origin + "/.well-known/openid-configuration/" + suffix,
            parsed.origin + parsed.path + "/.well-known/openid-configuration"};
}

std::string base64url_encode(std::string_view input) {
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
            result.push_back(alphabet[(buffer >> bits) & 0x3f]);
        }
    }
    if (bits != 0) result.push_back(alphabet[(buffer << (6 - bits)) & 0x3f]);
    return result;
}

std::string base64url_decode(std::string_view input) {
    if (input.empty() || input.find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") !=
            std::string_view::npos) {
        throw TokenVerificationError("invalid base64url encoding");
    }
    std::string result;
    result.reserve(input.size() * 3 / 4);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const unsigned char ch : input) {
        unsigned value = 0;
        if (ch >= 'A' && ch <= 'Z') value = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') value = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') value = ch - '0' + 52;
        else if (ch == '-') value = 62;
        else value = 63;
        buffer = (buffer << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<char>((buffer >> bits) & 0xff));
        }
    }
    if (bits >= 6 || (bits != 0 && (buffer & ((1U << bits) - 1U)) != 0) ||
        base64url_encode(result) != input) {
        throw TokenVerificationError("non-canonical base64url encoding");
    }
    return result;
}

Json parse_limited_json(std::string_view value) {
    if (value.empty() || value.size() > 16 * 1024) {
        throw TokenVerificationError("JWT JSON segment has invalid size");
    }
    std::size_t depth = 0;
    bool too_deep = false;
    auto parsed = Json::parse(value, [&](int, Json::parse_event_t event, Json&) {
        if (event == Json::parse_event_t::object_start ||
            event == Json::parse_event_t::array_start) {
            if (++depth > 32) too_deep = true;
        } else if (event == Json::parse_event_t::object_end ||
                   event == Json::parse_event_t::array_end) {
            if (depth > 0) --depth;
        }
        return true;
    }, false);
    if (too_deep || parsed.is_discarded() || !parsed.is_object()) {
        throw TokenVerificationError("invalid or deeply nested JWT JSON");
    }
    return parsed;
}

std::string required_string(const Json& value, const char* name) {
    const auto found = value.find(name);
    if (found == value.end() || !found->is_string() ||
        found->get_ref<const std::string&>().empty()) {
        throw TokenVerificationError(std::string("missing or invalid JWT claim: ") + name);
    }
    return found->get<std::string>();
}

std::int64_t required_integer(const Json& value, const char* name) {
    const auto found = value.find(name);
    if (found == value.end() || !found->is_number_integer()) {
        throw TokenVerificationError(std::string("missing or invalid JWT claim: ") + name);
    }
    try {
        return found->get<std::int64_t>();
    } catch (const nlohmann::json::exception&) {
        throw TokenVerificationError(std::string("invalid JWT claim: ") + name);
    }
}

std::vector<std::string> audience_values(const Json& claims) {
    const auto found = claims.find("aud");
    if (found == claims.end()) throw TokenVerificationError("missing JWT audience");
    std::vector<std::string> result;
    if (found->is_string()) {
        result.push_back(found->get<std::string>());
    } else if (found->is_array() && !found->empty()) {
        for (const auto& item : *found) {
            if (!item.is_string()) throw TokenVerificationError("invalid JWT audience");
            result.push_back(item.get<std::string>());
        }
    } else {
        throw TokenVerificationError("invalid JWT audience");
    }
    return result;
}

std::set<std::string> scope_values(const Json& claims) {
    std::set<std::string> result;
    if (const auto scope = claims.find("scope"); scope != claims.end()) {
        if (!scope->is_string()) throw TokenVerificationError("invalid JWT scope claim");
        try {
            const auto parsed = parse_scope_set(scope->get_ref<const std::string&>());
            result.insert(parsed.begin(), parsed.end());
        } catch (const std::invalid_argument&) {
            throw TokenVerificationError("invalid JWT scope claim");
        }
    }
    for (const auto* key : {"scp", "scopes"}) {
        if (const auto values = claims.find(key); values != claims.end()) {
            if (!values->is_array()) throw TokenVerificationError("invalid JWT scopes claim");
            for (const auto& item : *values) {
                if (!item.is_string()) throw TokenVerificationError("invalid JWT scopes claim");
                const auto scope = item.get<std::string>();
                if (!valid_scope_token(scope)) {
                    throw TokenVerificationError("invalid JWT scopes claim");
                }
                result.insert(scope);
            }
        }
    }
    return result;
}

std::string decode_header_value(const Json& jwk, const char* name) {
    const auto found = jwk.find(name);
    if (found == jwk.end() || !found->is_string()) {
        throw TokenVerificationError("JWK is missing a required key parameter");
    }
    return base64url_decode(found->get_ref<const std::string&>());
}

EVP_PKEY* build_rsa_key(const Json& jwk) {
    auto modulus = decode_header_value(jwk, "n");
    auto exponent = decode_header_value(jwk, "e");
    BIGNUM* n = BN_bin2bn(reinterpret_cast<const unsigned char*>(modulus.data()),
                          static_cast<int>(modulus.size()), nullptr);
    BIGNUM* e = BN_bin2bn(reinterpret_cast<const unsigned char*>(exponent.data()),
                          static_cast<int>(exponent.size()), nullptr);
    OSSL_PARAM_BLD* builder = OSSL_PARAM_BLD_new();
    EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
    OSSL_PARAM* parameters = nullptr;
    EVP_PKEY* key = nullptr;
    const bool ready = n && e && builder && context &&
        OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_N, n) == 1 &&
        OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_E, e) == 1 &&
        EVP_PKEY_fromdata_init(context) == 1 &&
        (parameters = OSSL_PARAM_BLD_to_param(builder)) != nullptr &&
        EVP_PKEY_fromdata(context, &key, EVP_PKEY_PUBLIC_KEY, parameters) == 1;
    BN_free(n);
    BN_free(e);
    OSSL_PARAM_free(parameters);
    OSSL_PARAM_BLD_free(builder);
    EVP_PKEY_CTX_free(context);
    if (!ready) {
        EVP_PKEY_free(key);
        throw TokenVerificationError("invalid RSA JWK");
    }
    return key;
}

EVP_PKEY* build_ec_key(const Json& jwk) {
    const auto x = decode_header_value(jwk, "x");
    const auto y = decode_header_value(jwk, "y");
    if (x.size() != 32 || y.size() != 32) throw TokenVerificationError("invalid P-256 JWK");
    std::string point(1, '\x04');
    point += x;
    point += y;
    char group[] = "prime256v1";
    OSSL_PARAM_BLD* builder = OSSL_PARAM_BLD_new();
    EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    OSSL_PARAM* parameters = nullptr;
    EVP_PKEY* key = nullptr;
    const bool ready = builder && context &&
        OSSL_PARAM_BLD_push_utf8_string(builder, OSSL_PKEY_PARAM_GROUP_NAME, group, 0) == 1 &&
        OSSL_PARAM_BLD_push_octet_string(
            builder, OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size()) == 1 &&
        EVP_PKEY_fromdata_init(context) == 1 &&
        (parameters = OSSL_PARAM_BLD_to_param(builder)) != nullptr &&
        EVP_PKEY_fromdata(context, &key, EVP_PKEY_PUBLIC_KEY, parameters) == 1;
    OSSL_PARAM_free(parameters);
    OSSL_PARAM_BLD_free(builder);
    EVP_PKEY_CTX_free(context);
    if (!ready) {
        EVP_PKEY_free(key);
        throw TokenVerificationError("invalid EC JWK");
    }
    return key;
}

EVP_PKEY* build_ed25519_key(const Json& jwk) {
    const auto x = decode_header_value(jwk, "x");
    if (x.size() != 32) throw TokenVerificationError("invalid Ed25519 JWK");
    EVP_PKEY* key = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char*>(x.data()), x.size());
    if (!key) throw TokenVerificationError("invalid Ed25519 JWK");
    return key;
}

std::vector<unsigned char> ecdsa_der_signature(std::string_view raw) {
    if (raw.size() != 64) throw TokenVerificationError("invalid ES256 signature length");
    ECDSA_SIG* signature = ECDSA_SIG_new();
    BIGNUM* r = BN_bin2bn(reinterpret_cast<const unsigned char*>(raw.data()), 32, nullptr);
    BIGNUM* s = BN_bin2bn(reinterpret_cast<const unsigned char*>(raw.data() + 32), 32, nullptr);
    if (!signature || !r || !s || ECDSA_SIG_set0(signature, r, s) != 1) {
        ECDSA_SIG_free(signature);
        BN_free(r);
        BN_free(s);
        throw TokenVerificationError("invalid ES256 signature");
    }
    const int size = i2d_ECDSA_SIG(signature, nullptr);
    std::vector<unsigned char> result(static_cast<std::size_t>(size));
    auto* out = result.data();
    if (size <= 0 || i2d_ECDSA_SIG(signature, &out) != size) {
        ECDSA_SIG_free(signature);
        throw TokenVerificationError("invalid ES256 signature");
    }
    ECDSA_SIG_free(signature);
    return result;
}

bool verify_signature(const std::string& algorithm, const Json& jwk,
                      std::string_view message, std::string_view signature) {
    EVP_PKEY* key = nullptr;
    const auto key_type = jwk.value("kty", std::string{});
    if ((algorithm == "RS256" || algorithm == "PS256") && key_type == "RSA") {
        key = build_rsa_key(jwk);
    } else if (algorithm == "ES256" && key_type == "EC" &&
               jwk.value("crv", std::string{}) == "P-256") {
        key = build_ec_key(jwk);
    } else if (algorithm == "EdDSA" && key_type == "OKP" &&
               jwk.value("crv", std::string{}) == "Ed25519") {
        key = build_ed25519_key(jwk);
    } else {
        throw TokenVerificationError("JWK type does not match JWT algorithm");
    }

    std::vector<unsigned char> converted;
    if (algorithm == "ES256") {
        converted = ecdsa_der_signature(signature);
        signature = std::string_view(reinterpret_cast<const char*>(converted.data()),
                                     converted.size());
    }
    EVP_MD_CTX* digest = EVP_MD_CTX_new();
    EVP_PKEY_CTX* key_context = nullptr;
    const EVP_MD* hash = algorithm == "EdDSA" ? nullptr : EVP_sha256();
    bool valid = digest &&
        EVP_DigestVerifyInit(digest, &key_context, hash, nullptr, key) == 1;
    if (valid && algorithm == "PS256") {
        valid = EVP_PKEY_CTX_set_rsa_padding(key_context, RSA_PKCS1_PSS_PADDING) == 1 &&
                EVP_PKEY_CTX_set_rsa_pss_saltlen(key_context, RSA_PSS_SALTLEN_DIGEST) == 1;
    }
    if (valid) {
        valid = EVP_DigestVerify(
            digest, reinterpret_cast<const unsigned char*>(signature.data()),
            signature.size(), reinterpret_cast<const unsigned char*>(message.data()),
            message.size()) == 1;
    }
    EVP_MD_CTX_free(digest);
    EVP_PKEY_free(key);
    return valid;
}

std::chrono::seconds cache_age(const net::HttpResponse& response) {
    const auto found = response.headers.find("cache-control");
    if (found == response.headers.end()) return std::chrono::seconds(60);
    const auto marker = found->second.find("max-age=");
    if (marker == std::string::npos) return std::chrono::seconds(60);
    const auto begin = found->second.data() + marker + 8;
    const auto end = found->second.data() + found->second.size();
    std::int64_t seconds = 0;
    const auto parsed = std::from_chars(begin, end, seconds);
    if (parsed.ec != std::errc{} || seconds <= 0) return std::chrono::seconds(1);
    return std::chrono::seconds(std::min<std::int64_t>(seconds, 3600));
}

}  // namespace

struct JwtVerifier::Impl {
    struct IssuerKeys {
        std::string jwks_uri;
        Json keys = Json::array();
        Clock::time_point expires_at{};
        Clock::time_point refreshed_at{};
    };

    explicit Impl(JwtVerifierOptions value) : options(std::move(value)) {
        if (options.issuers.empty() || options.max_token_bytes == 0 ||
            options.clock_skew.count() < 0 || options.min_jwks_refresh.count() <= 0 ||
            options.stale_key_grace.count() < 0 || options.allowed_algorithms.empty()) {
            throw std::invalid_argument("invalid JWT verifier configuration");
        }
        for (const auto& algorithm : options.allowed_algorithms) {
            if (algorithm != "RS256" && algorithm != "PS256" &&
                algorithm != "ES256" && algorithm != "EdDSA") {
                throw std::invalid_argument("JWT algorithm is not supported");
            }
        }
    }

    std::string jwks_uri_for(const std::string& issuer) const {
        if (!options.jwks_uri.empty()) return options.jwks_uri;
        for (const auto& url : discovery_urls(issuer)) {
            try {
                const auto response = net::fetch_http(url, options.network_policy);
                if (response.status < 200 || response.status >= 300) continue;
                const auto metadata = Json::parse(response.body);
                if (!metadata.is_object() ||
                    metadata.value("issuer", std::string{}) != issuer) {
                    continue;
                }
                const auto uri = metadata.find("jwks_uri");
                if (uri != metadata.end() && uri->is_string() &&
                    !uri->get_ref<const std::string&>().empty()) {
                    return uri->get<std::string>();
                }
            } catch (const net::BlockedAddress&) {
                throw;
            } catch (const std::exception&) {
                continue;
            }
        }
        throw TokenVerificationError("no validated JWKS endpoint is available");
    }

    void refresh(const std::string& issuer, IssuerKeys& entry) const {
        entry.refreshed_at = Clock::now();
        if (entry.jwks_uri.empty()) entry.jwks_uri = jwks_uri_for(issuer);
        const auto response = net::fetch_http(entry.jwks_uri, options.network_policy);
        if (response.status < 200 || response.status >= 300) {
            throw std::runtime_error("JWKS endpoint returned a non-success status");
        }
        const auto document = Json::parse(response.body);
        if (!document.is_object() || !document.contains("keys") ||
            !document["keys"].is_array()) {
            throw TokenVerificationError("invalid JWKS document");
        }
        entry.keys = document["keys"];
        entry.expires_at = Clock::now() + cache_age(response);
    }

    Json find_jwk(const std::string& issuer, const std::string& kid,
                  const std::string& algorithm) const {
        std::lock_guard lock(mutex);
        auto& entry = issuers[issuer];
        const auto find = [&]() -> Json {
            for (const auto& key : entry.keys) {
                if (key.is_object() && key.value("kid", std::string{}) == kid &&
                    (key.value("alg", std::string{}).empty() ||
                     key.value("alg", std::string{}) == algorithm) &&
                    (key.value("use", std::string{"sig"}) == "sig")) {
                    return key;
                }
            }
            return Json{};
        };
        const auto now = Clock::now();
        if (entry.keys.empty() || now >= entry.expires_at) {
            try {
                refresh(issuer, entry);
            } catch (const std::exception&) {
                if (entry.keys.empty() ||
                    now > entry.expires_at + options.stale_key_grace) {
                    throw TokenVerificationError("JWKS keys are unavailable");
                }
            }
        }
        auto jwk = find();
        if (!jwk.is_null()) return jwk;
        if (now - entry.refreshed_at >= options.min_jwks_refresh) {
            try {
                refresh(issuer, entry);
            } catch (const std::exception&) {
                if (entry.keys.empty() ||
                    now > entry.expires_at + options.stale_key_grace) {
                    throw TokenVerificationError("JWKS keys are unavailable");
                }
            }
            jwk = find();
        }
        if (jwk.is_null()) throw TokenVerificationError("JWT signing key was not found");
        return jwk;
    }

    JwtVerifierOptions options;
    mutable std::mutex mutex;
    mutable std::map<std::string, IssuerKeys> issuers;
};

JwtVerifier::JwtVerifier(JwtVerifierOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

JwtVerifier::~JwtVerifier() = default;

AuthContext JwtVerifier::verify(std::string_view token,
                                std::string_view resource) const {
    if (token.empty() || token.size() > impl_->options.max_token_bytes) {
        throw TokenVerificationError("JWT size is invalid");
    }
    const auto first_dot = token.find('.');
    const auto second_dot = first_dot == std::string_view::npos
                                ? std::string_view::npos
                                : token.find('.', first_dot + 1);
    if (first_dot == std::string_view::npos || second_dot == std::string_view::npos ||
        token.find('.', second_dot + 1) != std::string_view::npos) {
        throw TokenVerificationError("malformed JWT");
    }
    const auto header = parse_limited_json(base64url_decode(token.substr(0, first_dot)));
    const auto claims = parse_limited_json(
        base64url_decode(token.substr(first_dot + 1, second_dot - first_dot - 1)));
    if (header.contains("crit") || header.contains("jku") ||
        header.contains("x5u") || header.contains("jwk")) {
        throw TokenVerificationError("unsupported JWT header parameter");
    }
    const auto algorithm = required_string(header, "alg");
    if (std::find(impl_->options.allowed_algorithms.begin(),
                  impl_->options.allowed_algorithms.end(), algorithm) ==
        impl_->options.allowed_algorithms.end()) {
        throw TokenVerificationError("JWT algorithm is not allowed");
    }
    if (impl_->options.require_access_token_type &&
        header.value("typ", std::string{}) != "at+jwt") {
        throw TokenVerificationError("JWT is not an access token");
    }
    const auto issuer = required_string(claims, "iss");
    if (std::find(impl_->options.issuers.begin(), impl_->options.issuers.end(), issuer) ==
        impl_->options.issuers.end()) {
        throw TokenVerificationError("JWT issuer mismatch");
    }
    const auto audiences = audience_values(claims);
    const auto expected = impl_->options.accepted_audiences.empty()
                              ? std::vector<std::string>{std::string(resource)}
                              : impl_->options.accepted_audiences;
    if (std::none_of(audiences.begin(), audiences.end(), [&](const auto& audience) {
            return std::find(expected.begin(), expected.end(), audience) != expected.end();
        })) {
        throw TokenVerificationError("JWT audience mismatch");
    }
    const auto expires = required_integer(claims, "exp");
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        Clock::now().time_since_epoch()).count();
    const auto skew = impl_->options.clock_skew.count();
    if (expires <= now - skew) throw TokenVerificationError("JWT has expired");
    if (const auto nbf = claims.find("nbf"); nbf != claims.end()) {
        if (!nbf->is_number_integer() || nbf->get<std::int64_t>() > now + skew) {
            throw TokenVerificationError("JWT is not yet valid");
        }
    }
    std::int64_t issued = 0;
    if (const auto iat = claims.find("iat"); iat != claims.end()) {
        if (!iat->is_number_integer() || iat->get<std::int64_t>() > now + skew) {
            throw TokenVerificationError("JWT issued-at claim is invalid");
        }
        issued = iat->get<std::int64_t>();
    }
    std::string subject;
    if (impl_->options.require_subject) subject = required_string(claims, "sub");
    else if (const auto sub = claims.find("sub"); sub != claims.end() && sub->is_string()) {
        subject = sub->get<std::string>();
    }

    const auto kid = required_string(header, "kid");
    const auto jwk = impl_->find_jwk(issuer, kid, algorithm);
    const auto signing_input = token.substr(0, second_dot);
    const auto signature = base64url_decode(token.substr(second_dot + 1));
    if (!verify_signature(algorithm, jwk, signing_input, signature)) {
        throw TokenVerificationError("JWT signature verification failed");
    }

    AuthContext context;
    context.issuer = issuer;
    context.subject = std::move(subject);
    context.client_id = claims.value("client_id", claims.value("azp", std::string{}));
    if (!context.client_id.empty() && !claims.value("client_id", context.client_id).empty() &&
        claims.contains("azp") && claims["azp"].is_string() &&
        claims["azp"].get<std::string>() != context.client_id) {
        throw TokenVerificationError("JWT authorized party mismatch");
    }
    context.scopes = scope_values(claims);
    context.audience = audiences;
    context.expires_at = Clock::time_point(std::chrono::seconds(expires));
    if (issued != 0) context.issued_at = Clock::time_point(std::chrono::seconds(issued));
    if (const auto jti = claims.find("jti"); jti != claims.end() && jti->is_string()) {
        context.token_id = jti->get<std::string>();
    }
    context.claims = claims;
    return context;
}

}  // namespace cppmcp::auth
