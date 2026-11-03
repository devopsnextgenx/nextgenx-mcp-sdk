#include <cstddef>
#include <cstdint>
#include <string_view>

#include "cppmcp/auth/jwt_verifier.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    cppmcp::auth::JwtVerifierOptions options;
    options.issuers = {"https://issuer.example"};
    options.accepted_audiences = {"https://resource.example/mcp"};
    options.jwks_uri = "http://127.0.0.1:1/jwks";
    cppmcp::auth::JwtVerifier verifier(std::move(options));
    try {
        verifier.verify(std::string_view(reinterpret_cast<const char*>(data), size),
                        "https://resource.example/mcp");
    } catch (const std::exception&) {
    }
    return 0;
}
