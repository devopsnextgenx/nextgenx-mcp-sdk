#include <cstddef>
#include <cstdint>
#include <string>

#include "mcp_features.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 8192) return 0;
    const std::string cursor = size == 0
                                   ? std::string{}
                                   : std::string(reinterpret_cast<const char*>(data), size);
    const nlohmann::json params{{"cursor", cursor}};
    (void)cppmcp::examples::detail::parse_cursor(params, 0x1234, "resources");
    return 0;
}
