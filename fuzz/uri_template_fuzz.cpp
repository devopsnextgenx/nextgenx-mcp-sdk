#include <cstddef>
#include <cstdint>
#include <string_view>

#include "mcp_features.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 8192) return 0;
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    const auto separator = input.find('\0');
    const auto uri_template = input.substr(0, separator);
    const auto uri = separator == std::string_view::npos
                         ? std::string_view{}
                         : input.substr(separator + 1);
    (void)cppmcp::examples::detail::match_uri_template(uri_template, uri);
    return 0;
}
