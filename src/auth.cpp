#include "cppmcp/auth/auth_context.hpp"

#include "cppmcp/auth/scope_authorization.hpp"

#include <algorithm>
#include <cctype>
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

bool valid_scope_token(std::string_view scope) noexcept {
    if (scope.empty()) return false;
    return std::all_of(scope.begin(), scope.end(), [](unsigned char ch) {
        return ch == 0x21 || (ch >= 0x23 && ch <= 0x5b) ||
               (ch >= 0x5d && ch <= 0x7e);
    });
}

void validate_scope_set(const ScopeSet& scopes) {
    if (!std::all_of(scopes.begin(), scopes.end(), valid_scope_token)) {
        throw std::invalid_argument("scope values must use RFC 6749 scope-token syntax");
    }
}

void ScopeHierarchy::add_implication(std::string broader, std::string narrower) {
    if (!valid_scope_token(broader) || !valid_scope_token(narrower)) {
        throw std::invalid_argument("scope implication values must be non-empty scope tokens");
    }
    implications_[std::move(broader)].insert(std::move(narrower));
}

bool ScopeHierarchy::satisfies(const ScopeSet& granted, std::string_view required) const {
    std::vector<std::string> pending(granted.begin(), granted.end());
    ScopeSet visited;
    while (!pending.empty()) {
        auto current = std::move(pending.back());
        pending.pop_back();
        if (!visited.insert(current).second) continue;
        if (current == required) return true;
        const auto implications = implications_.find(current);
        if (implications != implications_.end()) {
            pending.insert(pending.end(), implications->second.begin(), implications->second.end());
        }
    }
    return false;
}

ScopeSet ScopeHierarchy::missing(const ScopeSet& granted, const ScopeSet& required) const {
    ScopeSet result;
    for (const auto& scope : required) {
        if (!satisfies(granted, scope)) result.insert(scope);
    }
    return result;
}

}  // namespace cppmcp::auth
