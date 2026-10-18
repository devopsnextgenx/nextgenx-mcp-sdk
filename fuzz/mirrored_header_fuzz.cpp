#include "cppmcp/transport/http.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 4096) {
        return 0;
    }
    const std::string_view expected(reinterpret_cast<const char*>(data), size);
    const auto encoded = cppmcp::transport::encode_mirrored_header_value(expected);
    std::string decoded;
    const bool encoded_matches =
        cppmcp::transport::decode_mirrored_header_value(encoded, decoded) &&
        decoded == expected;
    (void)encoded_matches;

    const std::string_view raw(reinterpret_cast<const char*>(data), size);
    decoded.clear();
    const bool accepted = cppmcp::transport::decode_mirrored_header_value(raw, decoded);
    if (accepted) {
        const bool matches = decoded == expected;
        (void)matches;
    }
    return 0;
}
