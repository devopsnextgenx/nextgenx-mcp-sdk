#include <cstddef>
#include <cstdint>
#include <string_view>

#include "cppmcp/client/oauth_metadata.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    try {
        (void)cppmcp::client::parse_oauth_metadata_document(
            std::string_view(reinterpret_cast<const char*>(data), size));
    } catch (const std::exception&) {
    }
    return 0;
}
