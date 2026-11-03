#include "cppmcp/client/oauth_metadata.hpp"

#include <cstddef>
#include <stdexcept>

namespace cppmcp::client {

nlohmann::json parse_oauth_metadata_document(std::string_view document) {
    if (document.empty() || document.size() > 256 * 1024) {
        throw std::invalid_argument("OAuth metadata document has invalid size");
    }
    std::size_t depth = 0;
    bool too_deep = false;
    auto parsed = nlohmann::json::parse(
        document,
        [&](int, nlohmann::json::parse_event_t event, nlohmann::json&) {
            if (event == nlohmann::json::parse_event_t::object_start ||
                event == nlohmann::json::parse_event_t::array_start) {
                if (++depth > 32) too_deep = true;
            } else if (event == nlohmann::json::parse_event_t::object_end ||
                       event == nlohmann::json::parse_event_t::array_end) {
                if (depth > 0) --depth;
            }
            return true;
        },
        false);
    if (too_deep || parsed.is_discarded() || !parsed.is_object()) {
        throw std::invalid_argument("OAuth metadata must be a shallow JSON object");
    }
    return parsed;
}

}  // namespace cppmcp::client
