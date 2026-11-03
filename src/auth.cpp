#include "cppmcp/auth/auth_context.hpp"

#include <algorithm>
#include <stdexcept>

#include <openssl/crypto.h>

namespace cppmcp::auth {

StaticBearerAuthenticator::StaticBearerAuthenticator(std::string token,
                                                     AuthContext context)
    : token_(std::move(token)), context_(std::move(context)) {
    if (token_.empty()) {
        throw std::invalid_argument("static bearer token must not be empty");
    }
}

AuthContext StaticBearerAuthenticator::verify(std::string_view token,
                                              std::string_view resource) const {
    if (token.size() != token_.size() ||
        CRYPTO_memcmp(token.data(), token_.data(), token.size()) != 0) {
        throw TokenVerificationError("invalid bearer token");
    }
    if (std::find(context_.audience.begin(), context_.audience.end(), resource) ==
        context_.audience.end()) {
        throw TokenVerificationError("token audience mismatch");
    }
    return context_;
}

}  // namespace cppmcp::auth
