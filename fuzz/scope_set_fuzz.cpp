#include <cstddef>
#include <cstdint>
#include <string_view>

#include "cppmcp/auth/scope_authorization.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    try {
        (void)cppmcp::auth::parse_scope_set(
            std::string_view(reinterpret_cast<const char*>(data), size));
    } catch (const std::invalid_argument&) {
    }
    return 0;
}
