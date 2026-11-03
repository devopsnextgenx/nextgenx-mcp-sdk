#pragma once

#include <string_view>

#include <nlohmann/json.hpp>

namespace cppmcp::client {

[[nodiscard]] nlohmann::json parse_oauth_metadata_document(std::string_view document);

}  // namespace cppmcp::client
