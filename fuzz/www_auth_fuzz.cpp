#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cppmcp/transport/http_client.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    (void)cppmcp::parse_www_authenticate(
        {std::string(reinterpret_cast<const char*>(data), size)});
    return 0;
}
