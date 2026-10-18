#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>

#include "cppmcp/transport/http.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    cppmcp::transport::SseParser parser(4096);
    try {
        for (std::size_t offset = 0; offset < size;) {
            const auto chunk_size = std::min<std::size_t>(1 + (data[offset] % 16),
                                                          size - offset);
            (void)parser.feed(std::string_view(
                reinterpret_cast<const char*>(data + offset), chunk_size));
            offset += chunk_size;
        }
    } catch (const std::exception&) {
    }
    return 0;
}
