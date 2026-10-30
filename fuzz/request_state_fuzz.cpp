#include "cppmcp/core/request_state.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 8192) return 0;
    static const cppmcp::core::HmacRequestStateCodec codec(
        std::vector<cppmcp::core::RequestStateKey>{{"fuzz", std::string(64, 'a')}});
    cppmcp::core::RequestStateBinding binding;
    binding.method = "tools/call";
    binding.name = "fuzz";
    try {
        (void)codec.verify(std::string_view(reinterpret_cast<const char*>(data), size), binding);
    } catch (const std::exception&) {
    }
    try {
        binding.private_payload = nlohmann::json::parse(data, data + size,
            [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
                if (depth > 32) throw std::invalid_argument("JSON depth limit");
                return true;
            });
        const auto token = codec.issue(binding);
        if (codec.verify(token, binding).private_payload != binding.private_payload) std::abort();
    } catch (const std::exception&) {
    }
    return 0;
}