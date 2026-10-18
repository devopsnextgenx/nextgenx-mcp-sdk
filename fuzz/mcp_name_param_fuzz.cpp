#include "cppmcp/transport/http.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    const auto separator = input.find('\0');
    const auto name = input.substr(0, separator);
    const auto param = separator == std::string_view::npos
                           ? std::string_view{}
                           : input.substr(separator + 1);
    std::string decoded_name;
    std::string decoded_param;
    (void)cppmcp::transport::decode_mirrored_header_value(name, decoded_name);
    (void)cppmcp::transport::decode_mirrored_header_value(param, decoded_param);
    return 0;
}
