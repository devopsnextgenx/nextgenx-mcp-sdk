#include <cstddef>
#include <cstdint>
#include <string>

#include "mcp_features.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 8192) return 0;
    cppmcp::examples::FileResourceProvider provider("", 1024);
    const std::string path = size == 0
                                 ? std::string{}
                                 : std::string(reinterpret_cast<const char*>(data), size);
    (void)provider.read(path);
    return 0;
}
