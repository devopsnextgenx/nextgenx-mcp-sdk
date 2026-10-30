#include "cppmcp/core/json_schema.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 16384) return 0;
    try {
        const auto document = nlohmann::json::parse(data, data + size,
            [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
                if (depth > 64) throw std::invalid_argument("JSON depth limit");
                return true;
            });
        auto validator = cppmcp::core::make_json_schema_validator();
        const auto schema = document.is_object() && document.contains("schema")
                                ? document["schema"] : document;
        const auto compiled = validator->compile(schema);
        if (document.is_object() && document.contains("instance")) {
            (void)compiled->validate(document["instance"]);
        }
    } catch (const std::exception&) {
    }
    return 0;
}